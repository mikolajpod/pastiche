// Diffusion-based style transfer: IP-Adapter + img2img on Stable Diffusion 1.5,
// through stock stable-diffusion.cpp (D7). The content image is the img2img
// starting point, the style image is fed to IP-Adapter as an image prompt, so
// no text prompt is required and any style image works without training.
//
// Needs three downloads (`pastiche download sd15 ip-adapter-sd15 clip-vision`):
// the SD 1.5 checkpoint, the IP-Adapter weights and the CLIP vision tower that
// encodes the style image.
//
// Unlike the feed-forward algorithms this one cannot be verified by comparing
// pixels against a CPU reference: sampling is iterative and chaotic, so any
// numerical difference grows into a different but equally valid image (D23).
#include "backends/ort_session.hpp"   // human_size, gpu_usage_line
#include "backends/sd_session.hpp"
#include "core/algorithm.hpp"
#include "core/fs.hpp"
#include "core/gpu_info.hpp"

#include <algorithm>
#include <mutex>
#include <string>

namespace pastiche {

namespace {

// Relative to the models directory. Must stay in step with models.json; the
// error message below points the user at the matching download names.
constexpr const char* kCheckpoint = "sd15/v1-5-pruned-emaonly.safetensors";
constexpr const char* kIpAdapter = "ip-adapter/ip-adapter_sd15.safetensors";
constexpr const char* kClipVision = "ip-adapter/clip_vision_h.safetensors";

// SD 1.5 works in a latent space downsampled by 8, and the UNet halves the
// resolution three more times, so both edges must be multiples of 64.
constexpr int kAlign = 64;

// VRAM model. Unlike the ONNX algorithms, whose footprint could be read
// straight off a DXGI delta, stable-diffusion.cpp segments the UNet and streams
// weights on and off the GPU, so a reading taken after a run only shows what
// stayed resident (2.19 GiB at q8_0, whatever the resolution). The constants
// below are therefore fitted to where runs actually start failing, which is
// what preflight has to predict anyway.
//
// Measured on a Quadro T2000, 3.27 GiB usable, with the full IP-Adapter
// pipeline loaded (content 320x240 scaled by --size, so the pixel counts are
// the real ones, not the square of the flag):
//   q8_0  512 ->  512x384 (196k px)  works
//   q8_0  640 ->  640x480 (307k px)  works
//   q8_0  704 ->  704x512 (360k px)  out of memory
//   q8_0  768 ->  768x576 (442k px)  out of memory
//   f16   256 ->  256x192  (49k px)  out of memory
//   f16   384..512                   out of memory
//
// The jump between q8_0 and f16 is far larger than the 315 MB the weights
// differ by, because the compute buffers are sized by the weight dtype too.
// Hence a per-precision overhead rather than one shared constant.
uint64_t weight_bytes(const std::string& precision)
{
    if (precision == "f32") return 2784ull << 20;
    if (precision == "q8_0") return 1796ull << 20;
    return 2111ull << 20;  // f16
}

uint64_t overhead_bytes(const std::string& precision)
{
    if (precision == "f32") return 1600ull << 20;
    if (precision == "q8_0") return 450ull << 20;
    return 1300ull << 20;  // f16
}

constexpr uint64_t kBytesPerPixel = 4000;

// Params::get_str has no fallback and returns "" for an unset key, which is a
// legitimate value for the text prompts but not for the enums below.
std::string str_or(const Params& p, const char* key, const char* fallback)
{
    const std::string& value = p.get_str(key);
    return value.empty() ? fallback : value;
}

int round_to_align(int v)
{
    const int r = (v + kAlign / 2) / kAlign * kAlign;
    return std::max(kAlign, r);
}

sd_image_t as_sd_image(Image& img)
{
    sd_image_t out{};
    out.width = static_cast<uint32_t>(img.width);
    out.height = static_cast<uint32_t>(img.height);
    out.channel = static_cast<uint32_t>(img.channels);
    out.data = img.data.data();
    return out;
}

Image from_sd_image(const sd_image_t& img)
{
    Image out(static_cast<int>(img.width), static_cast<int>(img.height),
              static_cast<int>(img.channel));
    if (img.data) std::copy(img.data, img.data + out.data.size(), out.data.begin());
    return out;
}

// stable-diffusion.cpp installs progress and preview callbacks globally rather
// than per context, so the run in flight has to be reachable from a free
// function. There is one worker thread by design (D9); the mutex is here so
// that a second caller cannot observe a half-updated target rather than to
// allow concurrent runs.
struct CallbackTarget {
    Progress* progress = nullptr;
    sd_ctx_t* ctx = nullptr;
    const SdApi* api = nullptr;
    bool cancel_requested = false;
    int configured_steps = 0;   // what the user asked for; see progress_callback
    float last_fraction = 0.f;  // the bar must never run backwards
};

std::mutex g_target_mutex;
CallbackTarget g_target;

void progress_callback(int step, int steps, float, void*)
{
    std::lock_guard<std::mutex> lock(g_target_mutex);
    if (!g_target.progress || steps <= 0) return;

    // Cancellation needs no watchdog thread here, unlike the ONNX path (D18):
    // the library asks us between steps and stops itself when told to. This
    // runs for every callback, whatever it is about, so a cancel is picked up
    // during weight streaming too.
    if (!g_target.cancel_requested && g_target.progress->cancelled() && g_target.ctx && g_target.api) {
        g_target.cancel_requested = true;
        g_target.api->cancel_generation(g_target.ctx, SD_CANCEL_ALL);
    }

    // The library funnels several unrelated activities through this one
    // callback: the sampler, the VAE tile loop, and streaming weights onto the
    // GPU, the last of which counts tensors and reports totals in the hundreds.
    // They are indistinguishable by signature, so filter by plausibility - the
    // sampler can never run more steps than were asked for - and let the
    // monotonic clamp below absorb whatever slips through.
    if (steps == 1 || steps > g_target.configured_steps) return;

    float fraction = 0.15f + 0.75f * static_cast<float>(step) / static_cast<float>(steps);
    fraction = std::max(fraction, g_target.last_fraction);

    // Once the clamp has pinned us at the top, sampling has finished and what
    // is still arriving is the VAE tile loop. Reporting its counter as a step
    // number would be wrong, so say what is actually happening instead.
    if (g_target.last_fraction >= 0.9f) {
        g_target.progress->report(0.9f, "decoding image");
        return;
    }

    g_target.last_fraction = fraction;
    g_target.progress->report(fraction, "generating, step " + std::to_string(step) + "/" +
                                            std::to_string(steps));
}

void preview_callback(int, int frame_count, sd_image_t* frames, bool, void*)
{
    std::lock_guard<std::mutex> lock(g_target_mutex);
    if (!g_target.progress || frame_count <= 0 || !frames) return;
    g_target.progress->preview(from_sd_image(frames[0]));
}

// Installs the callbacks for the duration of a run and takes them down after,
// so a later run cannot be handed a dangling Progress.
class CallbackScope {
public:
    CallbackScope(Progress& progress, const SdApi& api, bool want_preview, int interval, int steps)
        : api_(api)
    {
        {
            std::lock_guard<std::mutex> lock(g_target_mutex);
            g_target = CallbackTarget{&progress, nullptr, &api, false, steps, 0.f};
        }
        api_.set_progress_callback(&progress_callback, nullptr);
        if (want_preview) {
            // PREVIEW_PROJ is a cheap linear projection of the latent. TAE would
            // need another model download and VAE would decode in full, which
            // this card cannot spare the memory for.
            api_.set_preview_callback(&preview_callback, PREVIEW_PROJ, interval, true, false, nullptr);
        }
    }

    void set_context(sd_ctx_t* ctx)
    {
        std::lock_guard<std::mutex> lock(g_target_mutex);
        g_target.ctx = ctx;
    }

    bool cancel_requested() const
    {
        std::lock_guard<std::mutex> lock(g_target_mutex);
        return g_target.cancel_requested;
    }

    ~CallbackScope()
    {
        api_.set_progress_callback(nullptr, nullptr);
        api_.set_preview_callback(nullptr, PREVIEW_NONE, 0, false, false, nullptr);
        std::lock_guard<std::mutex> lock(g_target_mutex);
        g_target = CallbackTarget{};
    }

    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;

private:
    const SdApi& api_;
};

} // namespace

class DiffusionAlgorithm final : public IStyleAlgorithm {
public:
    std::string id() const override { return "diffusion"; }
    std::string description() const override
    {
        return "Highest quality, slowest: IP-Adapter image prompt over SD 1.5 img2img (needs downloaded models)";
    }

    std::vector<ParamSpec> params() const override
    {
        return {
            ParamSpec::Int("steps", 20, 1, 150,
                           "Denoising steps. More is slower and only slightly better above about 30"),
            ParamSpec::Float("strength", 0.6, 0.0, 1.0,
                             "How far from the content image to travel: 0 returns it unchanged, 1 ignores it entirely"),
            ParamSpec::Float("ip_scale", 1.0, 0.0, 2.0,
                             "Weight of the style image as an image prompt; above 1 the style starts to overrule the content"),
            ParamSpec::Float("cfg_scale", 7.0, 1.0, 20.0,
                             "Guidance strength; high values sharpen the style but can burn colours"),
            ParamSpec::Int("seed", -1, -1, 2147483647,
                           "Random seed; -1 picks a new one each run. The same seed reproduces a run only on the same device and precision"),
            ParamSpec::String("prompt", "",
                              "Optional text prompt. The style image already carries the style, so this is for nudging content"),
            ParamSpec::String("negative_prompt", "",
                              "Optional text describing what to avoid"),
            ParamSpec::Enum("precision", "q8_0", {"q8_0", "f16", "f32"},
                            "Weight precision. q8_0 is the only one that fits alongside the style encoder on a 4 GB card; f16 is faster and more accurate where there is room for it; f32 is CPU-only in practice"),
            ParamSpec::Bool("vae_tiling", true,
                            "Decode the final image in tiles. Slower, but on a 4 GB card the full decode does not fit"),
            ParamSpec::Int("preview_interval", 5, 0, 50,
                           "Emit an intermediate preview every N steps (0 disables). Cheap, but not what the final image looks like"),
            ParamSpec::String("device", "",
                              "GPU to run on, as named by --version -v (e.g. Vulkan1). Empty picks the discrete card"),
        };
    }

    StyleInput style_input() const override { return StyleInput::Image; }
    bool supports_tiling() const override { return false; }   // D5: diffusion scales down, never tiles
    bool uses_gpu() const override { return true; }
    bool deterministic() const override { return false; }      // see D23

    std::string unavailable_reason(const RunOptions& opts) const override
    {
        if (!SdRuntime::instance().available()) return SdRuntime::instance().error();
        return missing_models(opts);
    }

    uint64_t estimate_vram(int w, int h, const Params& p, const RunOptions& opts) const override
    {
        (void)opts;
        const int gw = round_to_align(w);
        const int gh = round_to_align(h);
        const uint64_t pixels = static_cast<uint64_t>(gw) * static_cast<uint64_t>(gh);
        const std::string precision = str_or(p, "precision", "q8_0");
        return weight_bytes(precision) + overhead_bytes(precision) + kBytesPerPixel * pixels;
    }

    std::string preflight(int w, int h, const Params& p, const RunOptions& opts) const override
    {
        if (opts.backend == "cpu") return {};

        const std::string missing = missing_models(opts);
        if (!missing.empty()) return missing;

        const uint64_t need = estimate_vram(w, h, p, opts);
        const GpuMemoryInfo gpu = query_gpu_memory();
        if (!gpu.ok) return {};                 // cannot measure: do not block the user
        const uint64_t avail = gpu.available();
        if (need <= avail) return {};

        std::string msg = "not enough GPU memory on " + gpu.adapter + ": diffusion needs about " +
                          human_size(need) + " for " + std::to_string(w) + "x" + std::to_string(h) +
                          ", " + human_size(avail) + " is free (of " + human_size(gpu.budget) +
                          " budget).";

        // Dropping precision is the cheapest fix and costs no resolution, so
        // offer it before suggesting a smaller image (D23).
        const std::string precision = str_or(p, "precision", "q8_0");
        if (precision != "q8_0") {
            Params lighter = p;
            lighter.set_kv(params(), "precision=q8_0");
            if (estimate_vram(w, h, lighter, opts) <= avail) {
                msg += " Try -p precision=q8_0, which keeps the resolution.";
                return msg;
            }
        }

        const int suggest = suggested_size(w, h, p, opts, avail);
        if (suggest > 0) msg += " Scale the content down with --size " + std::to_string(suggest) + ".";
        else msg += " Even 256 px does not fit; run on the CPU with --backend cpu (minutes, not seconds).";
        return msg;
    }

    RunResult run(const Image& content, const Image* style, const std::string&, const Params& p,
                  const RunOptions& opts, Progress& progress) override
    {
        SdRuntime& sd = SdRuntime::instance();
        if (!sd.available()) return RunResult::fail(sd.error());
        if (!style || style->empty()) {
            return RunResult::fail("diffusion needs a style image as the <style> argument");
        }

        const std::string missing = missing_models(opts);
        if (!missing.empty()) return RunResult::fail(missing);

        const SdApi& api = sd.api();
        sd_set_logging(opts.verbose);

        // Device: an explicit -p device= wins, otherwise the discrete GPU, and
        // --backend cpu forces the CPU as it does for the ONNX algorithms.
        std::string device = p.get_str("device");
        if (opts.backend == "cpu") {
            device = "cpu";
        } else if (device.empty()) {
            device = sd.preferred_device();
        } else if (!sd.find_device(device)) {
            std::string known;
            for (const SdDevice& d : sd.devices()) known += (known.empty() ? "" : ", ") + d.name;
            return RunResult::fail("unknown device '" + device + "'; available: " + known);
        }

        if (opts.verbose) {
            std::string adapters;
            for (const GpuAdapterInfo& a : list_gpu_adapters()) {
                adapters += (adapters.empty() ? "" : "; ") + a.name + " " +
                            std::to_string(a.dedicated_total >> 20) + " MiB" +
                            (a.software ? " (software)" : "");
            }
            progress.log("adapters: " + adapters);
            const SdDevice* chosen = sd.find_device(device);
            progress.log("chose device " + device + " = " +
                         (chosen ? chosen->description : std::string("?")));
        }

        const int gen_w = round_to_align(content.width);
        const int gen_h = round_to_align(content.height);
        if (opts.verbose && (gen_w != content.width || gen_h != content.height)) {
            progress.log("generating at " + std::to_string(gen_w) + "x" + std::to_string(gen_h) +
                         " (both edges must be multiples of 64), result scaled back to " +
                         std::to_string(content.width) + "x" + std::to_string(content.height));
        }

        // The library takes non-const pixel pointers, so both inputs are copies
        // anyway; make them RGB and the right size while we are at it.
        Image init_image = to_rgb(resize(content, gen_w, gen_h));
        Image style_image = to_rgb(*style);

        const std::string precision = str_or(p, "precision", "q8_0");
        const int steps = p.get_int("steps", 20);
        const int preview_interval = p.get_int("preview_interval", 5);

        progress.report(0.f, "loading models");
        const GpuMemoryInfo gpu_before = opts.verbose ? query_gpu_memory() : GpuMemoryInfo{};

        CallbackScope callbacks(progress, api, preview_interval > 0, preview_interval, steps);

        const std::string checkpoint = path_join(opts.models_dir, kCheckpoint);
        const std::string ip_adapter = path_join(opts.models_dir, kIpAdapter);
        const std::string clip_vision = path_join(opts.models_dir, kClipVision);

        sd_ctx_params_t ctx_params;
        api.ctx_params_init(&ctx_params);
        ctx_params.model_path = checkpoint.c_str();
        ctx_params.ip_adapter_path = ip_adapter.c_str();
        ctx_params.clip_vision_path = clip_vision.c_str();
        ctx_params.wtype = api.str_to_type(precision.c_str());
        ctx_params.n_threads = opts.threads;
        ctx_params.backend = device.c_str();
        // The CLIP vision tower is 2.4 GB and runs exactly once, to encode the
        // style image, so its weights stay in system RAM. On a 4 GB card they
        // would otherwise crowd out the diffusion model and the run dies part
        // way through the UNet. Measured: with them resident, 512 px does not
        // fit at any precision.
        if (device != "cpu") ctx_params.params_backend = "clip=cpu";

        if (progress.cancelled()) return RunResult::aborted();

        sd_ctx_t* ctx = api.new_ctx(&ctx_params);
        if (!ctx) {
            return RunResult::fail("could not load the diffusion model from " + checkpoint +
                                   " (run with -v for the library's own diagnostics)");
        }
        callbacks.set_context(ctx);

        sd_img_gen_params_t gen;
        api.img_gen_params_init(&gen);
        gen.prompt = "";
        gen.negative_prompt = "";
        const std::string prompt = p.get_str("prompt");
        const std::string negative = p.get_str("negative_prompt");
        if (!prompt.empty()) gen.prompt = prompt.c_str();
        if (!negative.empty()) gen.negative_prompt = negative.c_str();

        gen.init_image = as_sd_image(init_image);
        gen.ip_adapter_image = as_sd_image(style_image);
        gen.ip_adapter_strength = static_cast<float>(p.get_float("ip_scale", 1.0));
        gen.strength = static_cast<float>(p.get_float("strength", 0.6));
        gen.width = gen_w;
        gen.height = gen_h;
        gen.seed = p.get_int("seed", -1);
        gen.batch_count = 1;
        gen.sample_params.sample_steps = steps;
        gen.sample_params.guidance.txt_cfg = static_cast<float>(p.get_float("cfg_scale", 7.0));
        gen.vae_tiling_params.enabled = p.get_bool("vae_tiling", true);

        progress.report(0.15f, "sampling");

        sd_image_t* results = nullptr;
        int result_count = 0;
        const bool ok = api.generate_image(ctx, &gen, &results, &result_count);

        RunResult out;
        if (callbacks.cancel_requested() || progress.cancelled()) {
            out = RunResult::aborted();
        } else if (!ok || result_count <= 0 || !results) {
            out = RunResult::fail("the diffusion run failed (use -v to see why)");
        } else {
            progress.report(0.95f, "finishing");
            Image image = from_sd_image(results[0]);
            // Undo the alignment rounding so the caller gets what it asked for.
            if (image.width != content.width || image.height != content.height) {
                image = resize(image, content.width, content.height);
            }
            out.image = std::move(image);
            // The positional name alone is misleading: ggml numbers Vulkan
            // devices in enumeration order, which is not stable between runs,
            // so Vulkan0 is the discrete card in one process and the iGPU in
            // the next. Report what was actually used.
            const SdDevice* used = sd.find_device(device);
            out.backend_used = used && !used->description.empty()
                                   ? device + " (" + used->description + ")"
                                   : device;
        }

        if (opts.verbose && device != "cpu") {
            const std::string line = gpu_usage_line(gpu_before, query_gpu_memory(),
                                                    estimate_vram(content.width, content.height, p, opts));
            if (!line.empty()) progress.log(line);
        }

        if (results) api.free_images(results, result_count);
        api.free_ctx(ctx);

        if (out.ok()) progress.report(1.f, "done");
        return out;
    }

private:
    // "" when everything is in place, otherwise a message naming the download.
    std::string missing_models(const RunOptions& opts) const
    {
        struct Needed { const char* path; const char* download; };
        const Needed needed[] = {
            {kCheckpoint, "sd15"},
            {kIpAdapter, "ip-adapter-sd15"},
            {kClipVision, "clip-vision"},
        };
        std::string names;
        for (const Needed& n : needed) {
            if (!file_exists(path_join(opts.models_dir, n.path))) {
                names += (names.empty() ? "" : " ") + std::string(n.download);
            }
        }
        if (names.empty()) return {};
        return "diffusion models are not downloaded yet; run: pastiche download " + names;
    }

    // Largest longer-side that fits, searched the same way the ONNX algorithms
    // do: try decreasing sizes rather than inverting the estimate, which is not
    // a straight line once alignment rounding is involved.
    int suggested_size(int w, int h, const Params& p, const RunOptions& opts, uint64_t avail) const
    {
        const int longest = std::max(w, h);
        for (int candidate = (longest - 64) / 64 * 64; candidate >= 256; candidate -= 64) {
            const double scale = static_cast<double>(candidate) / longest;
            const int cw = std::max(64, static_cast<int>(w * scale));
            const int ch = std::max(64, static_cast<int>(h * scale));
            if (estimate_vram(cw, ch, p, opts) <= avail) return candidate;
        }
        return 0;
    }
};

REGISTER_ALGORITHM(DiffusionAlgorithm)

} // namespace pastiche
