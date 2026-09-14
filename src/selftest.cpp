#include "selftest.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pastiche {

namespace {

// Swallows progress, but not diagnostics: with -v an algorithm's log lines are
// the only window into what it actually did, and a self-test that hides them is
// a self-test you cannot debug.
class QuietProgress final : public Progress {
public:
    explicit QuietProgress(bool verbose) : verbose_(verbose) {}

    void log(const std::string& line) override
    {
        if (verbose_) std::fprintf(stderr, "               %s\n", line.c_str());
    }

private:
    bool verbose_ = false;
};

// Picks the <style> for an algorithm in a test: a synthetic image, the first
// preset, or nothing. Returns false when the algorithm cannot run here.
bool pick_style(const IStyleAlgorithm& algo, const RunOptions& opts, const Image& style_img,
                const Image*& style, std::string& style_name, std::string& why)
{
    style = nullptr;
    style_name.clear();
    switch (algo.style_input()) {
        case StyleInput::Image: style = &style_img; return true;
        case StyleInput::None: return true;
        case StyleInput::Preset: {
            const std::vector<std::string> presets = algo.presets(opts);
            if (presets.empty()) { why = "no presets found in models dir '" + opts.models_dir + "'"; return false; }
            style_name = presets.front();
            return true;
        }
    }
    return false;
}

} // namespace

int run_selftest(const RunOptions& opts)
{
    const Image content = make_test_image(256, 192, 1);
    const Image style_img = make_test_image(192, 256, 2);
    int failed = 0, skipped = 0, passed = 0;
    std::printf("%-14s %-6s %s\n", "algorithm", "result", "details");
    for (const std::string& id : Registry::instance().ids()) {
        std::unique_ptr<IStyleAlgorithm> algo = Registry::instance().create(id);
        const Image* style = nullptr;
        std::string style_name, why;
        if (!pick_style(*algo, opts, style_img, style, style_name, why)) {
            std::printf("%-14s %-6s %s\n", id.c_str(), "SKIP", why.c_str());
            ++skipped;
            continue;
        }
        if (const std::string reason = algo->unavailable_reason(opts); !reason.empty()) {
            std::printf("%-14s %-6s %s\n", id.c_str(), "SKIP", reason.c_str());
            ++skipped;
            continue;
        }

        const Params params = Params::defaults(algo->params());
        QuietProgress progress(opts.verbose);
        const auto t0 = std::chrono::steady_clock::now();
        RunResult r = algo->run(content, style, style_name, params, opts, progress);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::string problem;
        if (!r.ok()) problem = r.cancelled ? "cancelled" : r.error;
        else if (r.image.width != content.width || r.image.height != content.height)
            problem = "size " + std::to_string(r.image.width) + "x" + std::to_string(r.image.height) +
                      " != " + std::to_string(content.width) + "x" + std::to_string(content.height);
        else if (is_uniform(r.image)) problem = "output is a uniform colour";
        std::string extra;
        if (problem.empty() && !algo->deterministic()) {
            // Nothing to compare against: see IStyleAlgorithm::deterministic.
            // The checks above (right size, not a uniform colour, no error) are
            // what this algorithm can be held to (D23).
            extra = ", not compared (stochastic)";
        } else if (problem.empty() && !r.backend_used.empty() && r.backend_used != "cpu") {
            // Reference run on the CPU execution provider; GPU fp32 should agree closely.
            RunOptions cpu_opts = opts;
            cpu_opts.backend = "cpu";
            RunResult ref = algo->run(content, style, style_name, params, cpu_opts, progress);
            if (!ref.ok()) problem = "cpu reference failed: " + ref.error;
            else if (ref.image.data.size() != r.image.data.size()) problem = "cpu reference has a different size";
            else {
                double sum = 0;
                int maxd = 0;
                for (size_t i = 0; i < r.image.data.size(); ++i) {
                    const int d = std::abs(int(r.image.data[i]) - int(ref.image.data[i]));
                    sum += d;
                    maxd = std::max(maxd, d);
                }
                const double mean = sum / r.image.data.size();
                char buf[96];
                std::snprintf(buf, sizeof buf, ", vs cpu: mean diff %.2f, max %d", mean, maxd);
                extra = buf;
                if (mean > 2.0) problem = std::string("differs from cpu reference too much") + buf;
            }
        }
        if (problem.empty()) {
            std::printf("%-14s %-6s %.1f ms, hash %016llx%s%s%s\n", id.c_str(), "PASS", ms,
                        static_cast<unsigned long long>(image_hash(r.image)),
                        r.backend_used.empty() ? "" : ", backend ", r.backend_used.c_str(), extra.c_str());
            ++passed;
        } else {
            std::printf("%-14s %-6s %s\n", id.c_str(), "FAIL", problem.c_str());
            ++failed;
        }
    }
    std::printf("\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed == 0 ? 0 : 1;
}

int run_benchmark(const RunOptions& opts)
{
    const int sizes[] = {256, 512, 1024};
    int failures = 0;
    std::printf("%-14s %6s %10s %10s %s\n", "algorithm", "size", "first ms", "second ms", "notes");
    for (const std::string& id : Registry::instance().ids()) {
        std::unique_ptr<IStyleAlgorithm> algo = Registry::instance().create(id);
        for (int s : sizes) {
            const Image content = make_test_image(s, s * 3 / 4, 1);
            const Image style_img = make_test_image(s, s, 2);
            const Image* style = nullptr;
            std::string style_name, why;
            if (!pick_style(*algo, opts, style_img, style, style_name, why)) {
                std::printf("%-14s %6d %10s %10s %s\n", id.c_str(), s, "-", "-", why.c_str());
                break;
            }
            const Params params = Params::defaults(algo->params());
            QuietProgress progress(opts.verbose);
            double ms[2] = {0, 0};
            std::string note;
            for (int rep = 0; rep < 2; ++rep) {
                const auto t0 = std::chrono::steady_clock::now();
                RunResult r = algo->run(content, style, style_name, params, opts, progress);
                ms[rep] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (!r.ok()) { note = r.error.empty() ? "cancelled" : r.error; ++failures; break; }
                if (rep == 0 && !r.backend_used.empty()) note = "backend " + r.backend_used;
            }
            std::printf("%-14s %6d %10.1f %10.1f %s\n", id.c_str(), s, ms[0], ms[1], note.c_str());
            if (!note.empty() && note.rfind("backend", 0) != 0) break;
        }
    }
    return failures == 0 ? 0 : 1;
}

} // namespace pastiche
