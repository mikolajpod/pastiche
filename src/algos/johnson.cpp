// Johnson et al. 2016, "Perceptual Losses for Real-Time Style Transfer":
// one feed-forward transformer network per style. Models are ONNX files
// models/johnson-<style>.onnx (see tools/prepare_johnson_onnx.py); the
// <style> argument is the preset name (or a path to an .onnx file).
//
// Network I/O: float32 NCHW RGB in 0..255, output in the same range. Two
// stride-2 convolutions followed by two 2x upsamplings mean the output size is
// only guaranteed to match for dimensions divisible by 4, so the input is
// edge-padded to a multiple of 4 and the result cropped back.
#include "backends/ort_session.hpp"
#include "core/algorithm.hpp"
#include "core/fs.hpp"
#include "core/gpu_info.hpp"
#include "core/tiling.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace pastiche {

namespace {

constexpr const char* kPrefix = "johnson-";
constexpr const char* kSuffix = ".onnx";
constexpr int kAlign = 4;
constexpr int kOverlap = 128;

// DirectML footprint (fp32, ORT 1.22) measured on a Quadro T2000 with -v:
//   512x384 -> 301 MiB, 1024x768 -> 1.06 GiB, 1536x1152 -> 2.08 GiB,
//   2048x1536 -> 4.11 GiB (over budget, spilled to system memory, 3x slower).
// Linear fit: about 45 MiB + 1390 B/px. Constants below sit on the high side.
constexpr uint64_t kFixedBytes = 100ull << 20;
constexpr uint64_t kBytesPerPixel = 1400;

} // namespace

class JohnsonAlgorithm final : public IStyleAlgorithm {
public:
    std::string id() const override { return "johnson"; }
    std::string description() const override
    {
        return "Fast feed-forward stylisation with a pre-trained style network (Johnson 2016)";
    }
    std::vector<ParamSpec> params() const override
    {
        return {
            ParamSpec::Float("blend", 1.0, 0.0, 1.0, "Mix between the original (0) and the stylised image (1)"),
            ParamSpec::Int("tile_size", 1024, 256, 4096,
                           "Tile edge in pixels when --tile is used (128 px overlap, soft blend). Instance "
                           "normalisation runs per tile, so flat areas may show tone shifts between tiles"),
        };
    }
    StyleInput style_input() const override { return StyleInput::Preset; }
    bool supports_tiling() const override { return true; }
    bool uses_gpu() const override { return true; }

    std::vector<std::string> presets(const RunOptions& opts) const override
    {
        std::vector<std::string> out;
        const size_t plen = std::strlen(kPrefix), slen = std::strlen(kSuffix);
        for (const std::string& f : list_dir(opts.models_dir)) {
            if (f.size() > plen + slen && f.compare(0, plen, kPrefix) == 0 &&
                f.compare(f.size() - slen, slen, kSuffix) == 0)
                out.push_back(f.substr(plen, f.size() - plen - slen));
        }
        return out;
    }

    uint64_t estimate_vram(int w, int h, const Params& p, const RunOptions& opts) const override
    {
        int pw = (w + kAlign - 1) / kAlign * kAlign;
        int ph = (h + kAlign - 1) / kAlign * kAlign;
        if (opts.tile) {
            const int t = std::max(kAlign, p.get_int("tile_size", 1024));
            pw = std::min(pw, t);
            ph = std::min(ph, t);
        }
        return kFixedBytes + kBytesPerPixel * static_cast<uint64_t>(pw) * static_cast<uint64_t>(ph);
    }

    std::string preflight(int w, int h, const Params& p, const RunOptions& opts) const override
    {
        return vram_preflight(*this, w, h, p, opts);
    }

    RunResult run(const Image& content, const Image*, const std::string& style_name, const Params& p,
                  const RunOptions& opts, Progress& progress) override
    {
        const std::string model_path = file_exists(style_name)
            ? style_name
            : path_join(opts.models_dir, std::string(kPrefix) + style_name + kSuffix);
        std::string backend;
        std::string err = OrtRuntime::instance().resolve_backend(opts.backend, backend);
        if (!err.empty()) return RunResult::fail(err);

        progress.report(0.f, "loading model " + path_basename(model_path));
        const GpuMemoryInfo gpu_before = opts.verbose ? query_gpu_memory() : GpuMemoryInfo{};
        OrtModel model;
        err = model.load(model_path, backend, opts.threads);
        if (!err.empty()) return RunResult::fail(err);
        if (progress.cancelled()) return RunResult::aborted();

        const Image rgb = to_rgb(content);
        const int tile = opts.tile ? std::max(kAlign, p.get_int("tile_size", 1024)) : std::max(rgb.width, rgb.height);
        const std::vector<TileRect> tiles = make_tiles(rgb.width, rgb.height, tile, kOverlap);
        TileBlender blender(rgb.width, rgb.height, kOverlap);
        Image stylised;
        for (size_t i = 0; i < tiles.size(); ++i) {
            if (progress.cancelled()) return RunResult::aborted();
            progress.report(0.05f + 0.9f * static_cast<float>(i) / tiles.size(),
                            tiles.size() == 1 ? "stylising on " + backend
                                              : "tile " + std::to_string(i + 1) + "/" + std::to_string(tiles.size()));
            const TileRect& r = tiles[i];
            Image piece;
            err = infer(model, tiles.size() == 1 ? rgb : crop(rgb, r.x, r.y, r.w, r.h), piece);
            if (!err.empty()) return RunResult::fail(err);
            if (tiles.size() == 1) stylised = std::move(piece);
            else blender.add(piece, r);
        }
        if (tiles.size() > 1) stylised = blender.finish();

        if (opts.verbose && backend != "cpu") {
            // Measured while the session is still alive, to calibrate estimate_vram().
            const std::string line = gpu_usage_line(gpu_before, query_gpu_memory(),
                                                    estimate_vram(content.width, content.height, p, opts));
            if (!line.empty()) progress.log(line);
        }

        const double blend = p.get_float("blend", 1.0);
        if (blend < 1.0) {
            progress.report(0.95f, "blending");
            const float b = static_cast<float>(blend);
            for (size_t i = 0; i < stylised.data.size(); ++i) {
                const float v = rgb.data[i] + (stylised.data[i] - rgb.data[i]) * b;
                stylised.data[i] = static_cast<uint8_t>(std::clamp(v + 0.5f, 0.f, 255.f));
            }
        }
        RunResult r;
        r.image = std::move(stylised);
        r.backend_used = model.backend();
        progress.report(1.f, "done");
        return r;
    }

private:
    // Runs the network on one RGB image (any size); output has the same size.
    static std::string infer(OrtModel& model, const Image& rgb, Image& out)
    {
        const Image padded = pad_to_multiple(rgb, kAlign);
        std::vector<float> in;
        image_to_chw(padded, in, 1.f);
        std::vector<float> outv;
        std::vector<int64_t> out_shape;
        const std::string err = model.run(in.data(), {1, 3, padded.height, padded.width}, outv, out_shape);
        if (!err.empty()) return err;
        if (out_shape.size() != 4 || out_shape[1] != 3 || out_shape[2] != padded.height || out_shape[3] != padded.width)
            return "unexpected output shape from the style network";
        Image full = chw_to_image(outv.data(), padded.width, padded.height, 1.f);
        out = (full.width == rgb.width && full.height == rgb.height) ? std::move(full) : crop(full, 0, 0, rgb.width, rgb.height);
        return {};
    }
};

REGISTER_ALGORITHM(JohnsonAlgorithm)

} // namespace pastiche
