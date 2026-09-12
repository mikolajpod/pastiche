// Huang & Belongie 2017, "Arbitrary Style Transfer in Real-time with Adaptive
// Instance Normalization". Two ONNX models (see tools/prepare_adain_onnx.py):
//   models/adain-encoder.onnx  image [1,3,H,W] in 0..1  -> VGG relu4_1 features [1,512,H/8,W/8]
//   models/adain-decoder.onnx  features [1,512,h,w]     -> image [1,3,8h,8w] in 0..1
// The AdaIN step itself runs here in C++: per-channel mean/std of the content
// features are replaced by those of the style features. Statistics are always
// computed over the whole content image, so --tile only splits the network
// passes and tiles stay consistent with each other (D5).
#include "backends/ort_session.hpp"
#include "core/algorithm.hpp"
#include "core/fs.hpp"
#include "core/gpu_info.hpp"
#include "core/tiling.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pastiche {

namespace {

constexpr const char* kEncoder = "adain-encoder.onnx";
constexpr const char* kDecoder = "adain-decoder.onnx";
constexpr int kAlign = 8;        // three 2x2 max-pools / three 2x upsamplings
constexpr int kChannels = 512;
constexpr int kOverlap = 128;
constexpr double kEps = 1e-5;    // as in function.py calc_mean_std

// DirectML footprint (fp32, ORT 1.22) measured on a Quadro T2000 with -v:
//   512x384 -> 710 MiB, 768x576 -> 1.06 GiB, 1024x768 -> 2.05 GiB,
//   768x768 tiles -> 2.04 GiB. The DML allocator rounds buffers up to powers
// of two, hence the jumps; the constants below envelope the measurements.
constexpr uint64_t kFixedBytes = 200ull << 20;
constexpr uint64_t kBytesPerPixel = 2700;

struct Stats {
    std::vector<double> mean, stdev;
};

// Per-channel mean and standard deviation (unbiased, + eps, like torch.var).
Stats stats_of(const std::vector<double>& sum, const std::vector<double>& sumsq, double n)
{
    Stats s;
    s.mean.resize(sum.size());
    s.stdev.resize(sum.size());
    for (size_t c = 0; c < sum.size(); ++c) {
        const double mean = sum[c] / n;
        double var = (sumsq[c] - n * mean * mean) / std::max(1.0, n - 1.0);
        if (var < 0) var = 0;
        s.mean[c] = mean;
        s.stdev[c] = std::sqrt(var + kEps);
    }
    return s;
}

void accumulate(const std::vector<float>& feat, int plane, std::vector<double>& sum, std::vector<double>& sumsq)
{
    for (int c = 0; c < kChannels; ++c) {
        const float* f = feat.data() + static_cast<size_t>(c) * plane;
        double s = 0, q = 0;
        for (int i = 0; i < plane; ++i) { s += f[i]; q += static_cast<double>(f[i]) * f[i]; }
        sum[c] += s;
        sumsq[c] += q;
    }
}

} // namespace

class AdainAlgorithm final : public IStyleAlgorithm {
public:
    std::string id() const override { return "adain"; }
    std::string description() const override
    {
        return "Arbitrary style from any image via adaptive instance normalisation (Huang 2017)";
    }
    std::vector<ParamSpec> params() const override
    {
        return {
            ParamSpec::Float("alpha", 1.0, 0.0, 1.0, "Style strength: 0 keeps the content features, 1 replaces their statistics fully"),
            ParamSpec::Int("style_size", 512, 0, 2048,
                           "Longer side the style image is scaled to before encoding (0 = as is); sets the scale of the style's texture"),
            ParamSpec::Int("tile_size", 1024, 256, 4096,
                           "Tile edge in pixels when --tile is used (128 px overlap). Statistics are global, so tiles match"),
        };
    }
    StyleInput style_input() const override { return StyleInput::Image; }
    bool supports_tiling() const override { return true; }
    bool uses_gpu() const override { return true; }

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

    RunResult run(const Image& content, const Image* style, const std::string&, const Params& p,
                  const RunOptions& opts, Progress& progress) override
    {
        if (!style || style->empty()) return RunResult::fail("adain needs a style image");
        std::string backend;
        std::string err = OrtRuntime::instance().resolve_backend(opts.backend, backend);
        if (!err.empty()) return RunResult::fail(err);

        progress.report(0.f, "loading models");
        const GpuMemoryInfo gpu_before = opts.verbose ? query_gpu_memory() : GpuMemoryInfo{};
        OrtModel encoder, decoder;
        err = encoder.load(path_join(opts.models_dir, kEncoder), backend, opts.threads);
        if (!err.empty()) return RunResult::fail(err);
        err = decoder.load(path_join(opts.models_dir, kDecoder), backend, opts.threads);
        if (!err.empty()) return RunResult::fail(err);
        if (progress.cancelled()) return RunResult::aborted();

        // Style statistics.
        progress.report(0.05f, "encoding style");
        Image style_rgb = to_rgb(*style);
        const int style_size = p.get_int("style_size", 512);
        if (style_size > 0) style_rgb = fit_longest_side(style_rgb, style_size, true);
        std::vector<float> style_feat;
        int sfw = 0, sfh = 0;
        err = encode(encoder, style_rgb, style_feat, sfw, sfh);
        if (!err.empty()) return RunResult::fail(err);
        std::vector<double> ssum(kChannels, 0.0), ssq(kChannels, 0.0);
        accumulate(style_feat, sfw * sfh, ssum, ssq);
        const Stats style_stats = stats_of(ssum, ssq, static_cast<double>(sfw) * sfh);
        style_feat.clear();
        style_feat.shrink_to_fit();

        // Pass 1: encode content tiles, accumulate global statistics.
        const Image rgb = to_rgb(content);
        const int tile = opts.tile ? std::max(kAlign, p.get_int("tile_size", 1024)) : std::max(rgb.width, rgb.height);
        const std::vector<TileRect> tiles = make_tiles(rgb.width, rgb.height, tile, kOverlap);
        struct TileFeat { std::vector<float> feat; int fw = 0, fh = 0; };
        std::vector<TileFeat> feats(tiles.size());
        std::vector<double> csum(kChannels, 0.0), csq(kChannels, 0.0);
        double count = 0;
        for (size_t i = 0; i < tiles.size(); ++i) {
            if (progress.cancelled()) return RunResult::aborted();
            progress.report(0.1f + 0.4f * static_cast<float>(i) / tiles.size(),
                            tiles.size() == 1 ? "encoding content on " + backend
                                              : "encoding tile " + std::to_string(i + 1) + "/" + std::to_string(tiles.size()));
            const TileRect& r = tiles[i];
            err = encode(encoder, tiles.size() == 1 ? rgb : crop(rgb, r.x, r.y, r.w, r.h), feats[i].feat, feats[i].fw, feats[i].fh);
            if (!err.empty()) return RunResult::fail(err);
            accumulate(feats[i].feat, feats[i].fw * feats[i].fh, csum, csq);
            count += static_cast<double>(feats[i].fw) * feats[i].fh;
        }
        const Stats content_stats = stats_of(csum, csq, count);

        // Pass 2: AdaIN with global statistics, decode, blend tiles.
        const float alpha = static_cast<float>(p.get_float("alpha", 1.0));
        TileBlender blender(rgb.width, rgb.height, kOverlap);
        Image result;
        for (size_t i = 0; i < tiles.size(); ++i) {
            if (progress.cancelled()) return RunResult::aborted();
            progress.report(0.5f + 0.45f * static_cast<float>(i) / tiles.size(),
                            tiles.size() == 1 ? "decoding on " + backend
                                              : "decoding tile " + std::to_string(i + 1) + "/" + std::to_string(tiles.size()));
            TileFeat& tf = feats[i];
            const int plane = tf.fw * tf.fh;
            for (int c = 0; c < kChannels; ++c) {
                const float scale = static_cast<float>(style_stats.stdev[c] / content_stats.stdev[c]);
                const float shift = static_cast<float>(style_stats.mean[c] - content_stats.mean[c] * scale);
                float* f = tf.feat.data() + static_cast<size_t>(c) * plane;
                for (int k = 0; k < plane; ++k) {
                    const float t = f[k] * scale + shift;
                    f[k] = alpha * t + (1.f - alpha) * f[k];
                }
            }
            const TileRect& r = tiles[i];
            Image piece;
            err = decode(decoder, tf.feat, tf.fw, tf.fh, r.w, r.h, piece);
            if (!err.empty()) return RunResult::fail(err);
            tf.feat.clear();
            tf.feat.shrink_to_fit();
            if (tiles.size() == 1) result = std::move(piece);
            else blender.add(piece, r);
        }
        if (tiles.size() > 1) result = blender.finish();

        if (opts.verbose && backend != "cpu") {
            // Measured while both sessions are still alive, to calibrate estimate_vram().
            const std::string line = gpu_usage_line(gpu_before, query_gpu_memory(),
                                                    estimate_vram(content.width, content.height, p, opts));
            if (!line.empty()) progress.log(line);
        }

        RunResult out;
        out.image = std::move(result);
        out.backend_used = encoder.backend();
        progress.report(1.f, "done");
        return out;
    }

private:
    // RGB image -> relu4_1 features of the image padded to a multiple of 8.
    static std::string encode(OrtModel& encoder, const Image& rgb, std::vector<float>& feat, int& fw, int& fh)
    {
        const Image padded = pad_to_multiple(rgb, kAlign);
        std::vector<float> in;
        image_to_chw(padded, in, 1.f / 255.f);
        std::vector<int64_t> shape;
        const std::string err = encoder.run(in.data(), {1, 3, padded.height, padded.width}, feat, shape);
        if (!err.empty()) return "encoder: " + err;
        if (shape.size() != 4 || shape[1] != kChannels || shape[2] != padded.height / kAlign || shape[3] != padded.width / kAlign)
            return "encoder returned an unexpected feature shape";
        fh = static_cast<int>(shape[2]);
        fw = static_cast<int>(shape[3]);
        return {};
    }

    // Features -> RGB image cropped to w x h.
    static std::string decode(OrtModel& decoder, const std::vector<float>& feat, int fw, int fh, int w, int h, Image& out)
    {
        std::vector<float> img;
        std::vector<int64_t> shape;
        const std::string err = decoder.run(feat.data(), {1, kChannels, fh, fw}, img, shape);
        if (!err.empty()) return "decoder: " + err;
        if (shape.size() != 4 || shape[1] != 3 || shape[2] != fh * kAlign || shape[3] != fw * kAlign)
            return "decoder returned an unexpected image shape";
        Image full = chw_to_image(img.data(), fw * kAlign, fh * kAlign, 255.f);
        out = (full.width == w && full.height == h) ? std::move(full) : crop(full, 0, 0, w, h);
        return {};
    }
};

REGISTER_ALGORITHM(AdainAlgorithm)

} // namespace pastiche
