// Identity algorithm: returns the content image unchanged (optionally inverted
// or gamma-adjusted). Exists to exercise the whole pipeline - CLI parsing,
// image I/O, parameter validation, progress, sidecar JSON - without a model.
#include "core/algorithm.hpp"

#include <algorithm>
#include <cmath>

namespace pastiche {

class IdentityAlgorithm final : public IStyleAlgorithm {
public:
    std::string id() const override { return "identity"; }
    std::string description() const override
    {
        return "Copies the content image (pipeline test, no model, no GPU)";
    }
    std::vector<ParamSpec> params() const override
    {
        return {
            ParamSpec::Bool("invert", false, "Invert colours of the output"),
            ParamSpec::Float("gamma", 1.0, 0.1, 5.0, "Gamma applied to the output (1 = unchanged)"),
        };
    }
    StyleInput style_input() const override { return StyleInput::None; }
    bool uses_gpu() const override { return false; }
    uint64_t estimate_vram(int, int, const Params&, const RunOptions&) const override { return 0; }

    RunResult run(const Image& content, const Image*, const std::string&, const Params& p,
                  const RunOptions&, Progress& progress) override
    {
        progress.report(0.f, "copying");
        RunResult r;
        r.image = content;
        const bool invert = p.get_bool("invert", false);
        const double gamma = p.get_float("gamma", 1.0);
        if (invert || gamma != 1.0) {
            uint8_t lut[256];
            for (int i = 0; i < 256; ++i) {
                double v = i / 255.0;
                if (gamma != 1.0) v = std::pow(v, 1.0 / gamma);
                if (invert) v = 1.0 - v;
                lut[i] = static_cast<uint8_t>(std::clamp(std::lround(v * 255.0), 0L, 255L));
            }
            const int c = r.image.channels;
            for (int y = 0; y < r.image.height; ++y) {
                if (progress.cancelled()) return RunResult::aborted();
                uint8_t* row = r.image.row(y);
                for (int x = 0; x < r.image.width; ++x)
                    for (int k = 0; k < 3; ++k) row[x * c + k] = lut[row[x * c + k]];
                if ((y & 63) == 0) progress.report(static_cast<float>(y) / r.image.height, "adjusting");
            }
        }
        progress.report(1.f, "done");
        return r;
    }
};

REGISTER_ALGORITHM(IdentityAlgorithm)

} // namespace pastiche
