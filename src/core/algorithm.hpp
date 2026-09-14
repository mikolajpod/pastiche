#pragma once

#include "image.hpp"
#include "params.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pastiche {

// What the <style> argument means for a given algorithm.
enum class StyleInput {
    Image,   // path to a style image (AdaIN, diffusion)
    Preset,  // name of a bundled/trained style, see presets() (Johnson)
    None     // ignored (identity)
};

// Options shared by all algorithms, set by the CLI flags / GUI, not by -p.
struct RunOptions {
    bool tile = false;          // explicit tiling (feed-forward only), never default
    std::string backend;        // "" = auto, or "cpu", "dml", "cuda"
    std::string models_dir;     // resolved models directory
    int threads = 0;            // 0 = library default
    bool verbose = false;
};

// Progress / cancellation channel. The worker calls report() between steps or
// tiles and polls cancelled(); the CLI prints, the GUI updates a shared state.
class Progress {
public:
    virtual ~Progress() = default;
    virtual void report(float fraction, const std::string& stage) { (void)fraction; (void)stage; }
    virtual bool cancelled() const { return false; }
    // Intermediate result (diffusion only); may be ignored.
    virtual void preview(const Image& img) { (void)img; }
    // Diagnostic line (verbose mode); the CLI prints it without breaking the
    // progress display, the GUI may append it to a log.
    virtual void log(const std::string& line) { (void)line; }
};

struct RunResult {
    Image image;
    std::string error;          // non-empty means failure
    std::string backend_used;   // e.g. "cpu", "dml"; empty when not applicable
    bool cancelled = false;
    bool ok() const { return error.empty() && !cancelled; }

    static RunResult fail(std::string msg) { RunResult r; r.error = std::move(msg); return r; }
    static RunResult aborted() { RunResult r; r.cancelled = true; return r; }
};

// One style-transfer algorithm. Implementations live in src/algos/<id>.cpp and
// register themselves with REGISTER_ALGORITHM. Constructors must be cheap: the
// registry instantiates every algorithm to list ids and parameters; models are
// loaded inside run().
class IStyleAlgorithm {
public:
    virtual ~IStyleAlgorithm() = default;

    virtual std::string id() const = 0;
    virtual std::string description() const = 0;
    virtual std::vector<ParamSpec> params() const = 0;

    virtual StyleInput style_input() const { return StyleInput::Image; }
    // Preset names when style_input() == Preset (may scan opts.models_dir).
    virtual std::vector<std::string> presets(const RunOptions& opts) const { (void)opts; return {}; }
    virtual bool supports_tiling() const { return false; }
    virtual bool uses_gpu() const { return true; }

    // False when the same inputs cannot be expected to give the same pixels on
    // two backends. Diffusion sampling is iterative and chaotic, so a rounding
    // difference well below the precision of the weights grows into a different
    // but equally valid image; measured, the same model at the same precision
    // on CPU versus Vulkan differs more than f32 versus q8_0 does (D23).
    // --selftest uses this to decide whether comparing against a CPU reference
    // proves anything.
    virtual bool deterministic() const { return true; }

    // Non-empty when the algorithm cannot run at all right now and why, for
    // instance because its weights have not been downloaded. Callers report it
    // and move on instead of treating it as a failure.
    virtual std::string unavailable_reason(const RunOptions& opts) const
    {
        (void)opts;
        return {};
    }

    // GPU memory in bytes needed to process a w x h content image with these
    // parameters (activations + weights). 0 = nothing / unknown.
    virtual uint64_t estimate_vram(int w, int h, const Params& p, const RunOptions& opts) const = 0;

    // Called before run() by the CLI and GUI. Returns "" when the run may go
    // ahead, otherwise a user-facing reason to refuse (typically: the VRAM
    // estimate exceeds the free GPU memory, with a suggested --size).
    virtual std::string preflight(int w, int h, const Params& p, const RunOptions& opts) const
    {
        (void)w; (void)h; (void)p; (void)opts;
        return {};
    }

    // style is non-null only for StyleInput::Image; style_name is the raw
    // <style> argument (preset name or path) for the other kinds.
    virtual RunResult run(const Image& content, const Image* style, const std::string& style_name,
                          const Params& p, const RunOptions& opts, Progress& progress) = 0;
};

using AlgorithmFactory = std::unique_ptr<IStyleAlgorithm> (*)();

class Registry {
public:
    static Registry& instance();
    void add(AlgorithmFactory factory);
    std::vector<std::string> ids() const;                       // sorted
    std::unique_ptr<IStyleAlgorithm> create(const std::string& id) const;  // nullptr when unknown
private:
    struct Entry { std::string id; AlgorithmFactory factory; };
    std::vector<Entry> entries_;
};

// Place at file scope in the algorithm's .cpp. The algorithm sources are
// compiled into an OBJECT library so the registrar is never dropped by the
// static-library linker.
#define REGISTER_ALGORITHM(Class)                                                     \
    namespace {                                                                       \
    struct Class##Registrar {                                                         \
        Class##Registrar()                                                            \
        {                                                                             \
            ::pastiche::Registry::instance().add(                                     \
                []() -> std::unique_ptr<::pastiche::IStyleAlgorithm> {                \
                    return std::make_unique<Class>();                                 \
                });                                                                   \
        }                                                                             \
    } Class##RegistrarInstance;                                                       \
    }

} // namespace pastiche
