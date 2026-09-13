#pragma once

// Thin wrapper over the ONNX Runtime C API. The runtime DLL is loaded at run
// time (LoadLibrary / dlopen), so the executables build and start without it;
// algorithms that need it report a clear error instead.
#include "core/algorithm.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct OrtApi;
struct OrtEnv;
struct OrtSession;
struct OrtSessionOptions;
struct OrtRunOptions;

namespace pastiche {

class OrtRuntime {
public:
    static OrtRuntime& instance();

    bool available() const { return api_ != nullptr; }
    const std::string& error() const { return error_; }          // why loading failed
    const std::string& library_path() const { return library_path_; }
    std::string version() const;                                // ORT version string
    const OrtApi* api() const { return api_; }
    OrtEnv* env() const { return env_; }
    bool dml_available() const { return dml_api_ != nullptr; }
    void* dml_api() const { return dml_api_; }                  // OrtDmlApi*, opaque here

    // Maps the requested backend ("", "auto", "cpu", "dml", "cuda") onto one
    // that is available here. Returns "" and sets `resolved`, or an error.
    std::string resolve_backend(const std::string& requested, std::string& resolved) const;
    std::vector<std::string> available_backends() const;

private:
    OrtRuntime();
    ~OrtRuntime();
    OrtRuntime(const OrtRuntime&) = delete;
    OrtRuntime& operator=(const OrtRuntime&) = delete;

    void* handle_ = nullptr;
    const OrtApi* api_ = nullptr;
    OrtEnv* env_ = nullptr;
    void* dml_api_ = nullptr;
    std::string error_;
    std::string library_path_;
};

// One loaded model. Not thread-safe; one session per worker.
// Sentinel returned by OrtModel::run() when the run was terminated through the
// Progress cancellation flag. Algorithms turn it into RunResult::aborted().
extern const char* const kCancelled;

class OrtModel {
public:
    OrtModel();
    ~OrtModel();
    OrtModel(const OrtModel&) = delete;
    OrtModel& operator=(const OrtModel&) = delete;

    // backend: "cpu" or "dml" (already resolved). Returns "" or an error.
    std::string load(const std::string& model_path, const std::string& backend, int threads);
    bool loaded() const { return session_ != nullptr; }
    const std::string& backend() const { return backend_; }
    const std::vector<std::string>& input_names() const { return inputs_; }
    const std::vector<std::string>& output_names() const { return outputs_; }

    // Float tensors in, float tensors out. Shapes are NCHW-style int64 dims.
    // When `progress` is given, a watchdog polls progress->cancelled() while the
    // session runs and terminates it; run() then returns kCancelled.
    std::string run(const std::vector<std::string>& in_names,
                    const std::vector<const float*>& in_data,
                    const std::vector<std::vector<int64_t>>& in_shapes,
                    const std::vector<std::string>& out_names,
                    std::vector<std::vector<float>>& out_data,
                    std::vector<std::vector<int64_t>>& out_shapes,
                    Progress* progress = nullptr);

    // Convenience for single-input single-output models.
    std::string run(const float* input, const std::vector<int64_t>& in_shape,
                    std::vector<float>& output, std::vector<int64_t>& out_shape,
                    Progress* progress = nullptr);

    // May be called from another thread to abort a running run() early.
    void terminate();

private:
    OrtSession* session_ = nullptr;
    OrtRunOptions* run_options_ = nullptr;
    std::string backend_;
    std::vector<std::string> inputs_, outputs_;
};

// GPU-memory preflight shared by the ORT algorithms: resolves the backend, and
// when it is a GPU backend compares algo.estimate_vram() with the free memory
// reported by the OS. Returns "" when the run may proceed, otherwise a
// user-facing refusal that names the amounts and a suggested --size (and
// --tile when the algorithm supports it).
std::string vram_preflight(const IStyleAlgorithm& algo, int w, int h, const Params& p, const RunOptions& opts);

// Largest longer side (multiple of 64, >= 256) at which algo.estimate_vram()
// fits into `available` bytes for an image with the aspect of w x h; 0 when
// even 256 px does not fit.
int vram_suggest_size(const IStyleAlgorithm& algo, int w, int h, const Params& p, const RunOptions& opts,
                      uint64_t available);

std::string human_size(uint64_t bytes);

struct GpuMemoryInfo;
// One-line "gpu <adapter>: usage A -> B (delta, estimate, budget)" report for
// verbose mode; empty when either measurement failed.
std::string gpu_usage_line(const GpuMemoryInfo& before, const GpuMemoryInfo& after, uint64_t estimate);

} // namespace pastiche
