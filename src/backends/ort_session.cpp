#include "ort_session.hpp"

#include "core/fs.hpp"
#include "core/gpu_info.hpp"

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifndef PASTICHE_ORT_DIR
#define PASTICHE_ORT_DIR ""
#endif

namespace pastiche {

namespace {

// Minimal mirror of OrtDmlApi from dml_provider_factory.h. Declared here to
// avoid pulling in d3d12.h / DirectML.h; only the two entry points we use are
// typed, the rest are placeholders that keep the layout.
enum DmlDeviceFilter : uint32_t { DML_FILTER_ANY = 0xffffffff, DML_FILTER_GPU = 1u << 0, DML_FILTER_NPU = 1u << 1 };
enum DmlPerformancePreference : uint32_t { DML_PREF_DEFAULT = 0, DML_PREF_HIGH_PERFORMANCE = 1, DML_PREF_MINIMUM_POWER = 2 };
struct DmlDeviceOptions { DmlPerformancePreference preference; DmlDeviceFilter filter; };
struct DmlApiMirror {
    OrtStatus*(ORT_API_CALL* AppendDML)(OrtSessionOptions*, int device_id);
    void* AppendDML1;
    void* CreateGPUAllocationFromD3DResource;
    void* FreeGPUAllocation;
    void* GetD3D12ResourceFromAllocation;
    OrtStatus*(ORT_API_CALL* AppendDML2)(OrtSessionOptions*, DmlDeviceOptions*);
};

std::string status_message(const OrtApi* api, OrtStatus* st)
{
    if (!st) return {};
    std::string msg = api->GetErrorMessage(st);
    api->ReleaseStatus(st);
    return msg;
}

#define ORT_CHECK(expr)                                              \
    do {                                                             \
        if (OrtStatus* st__ = (expr)) {                              \
            return std::string(#expr) + ": " + status_message(api, st__); \
        }                                                            \
    } while (0)

void* open_library(const std::string& path, std::string& err)
{
#ifdef _WIN32
    HMODULE h = LoadLibraryExW(utf8_to_wide(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "error %lu", static_cast<unsigned long>(GetLastError()));
        err = buf;
    }
    return reinterpret_cast<void*>(h);
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) err = dlerror() ? dlerror() : "dlopen failed";
    return h;
#endif
}

void* find_symbol(void* handle, const char* name)
{
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

[[maybe_unused]] void close_library(void* handle)
{
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

std::vector<std::string> candidate_library_paths()
{
#ifdef _WIN32
    const char* name = "onnxruntime.dll";
#elif defined(__APPLE__)
    const char* name = "libonnxruntime.dylib";
#else
    const char* name = "libonnxruntime.so";
#endif
    std::vector<std::string> out;
    const std::string env = getenv_utf8("PASTICHE_ORT_DIR");
    if (!env.empty()) out.push_back(path_join(env, name));
    out.push_back(path_join(exe_dir(), name));
    const std::string compiled = PASTICHE_ORT_DIR;
    if (!compiled.empty()) out.push_back(path_join(compiled, name));
    out.push_back(name);  // system search path
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// OrtRuntime
// ---------------------------------------------------------------------------
OrtRuntime& OrtRuntime::instance()
{
    static OrtRuntime rt;
    return rt;
}

OrtRuntime::OrtRuntime()
{
    std::string errors;
    for (const std::string& path : candidate_library_paths()) {
        if (path.find('/') != std::string::npos || path.find('\\') != std::string::npos) {
            if (!file_exists(path)) continue;
        }
        std::string err;
        handle_ = open_library(path, err);
        if (handle_) { library_path_ = path; break; }
        errors += "\n  " + path + ": " + err;
    }
    if (!handle_) {
        error_ = "ONNX Runtime library not found. Looked for:";
        for (const std::string& p : candidate_library_paths()) error_ += "\n  " + p;
        if (!errors.empty()) error_ += "\nload errors:" + errors;
        return;
    }
    using GetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();
    auto get_base = reinterpret_cast<GetApiBaseFn>(find_symbol(handle_, "OrtGetApiBase"));
    if (!get_base) { error_ = "OrtGetApiBase not exported by " + library_path_; return; }
    const OrtApiBase* base = get_base();
    api_ = base ? base->GetApi(ORT_API_VERSION) : nullptr;
    if (!api_) {
        error_ = "ONNX Runtime " + std::string(base && base->GetVersionString ? base->GetVersionString() : "?") +
                 " does not provide API version " + std::to_string(ORT_API_VERSION);
        return;
    }
    if (OrtStatus* st = api_->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "pastiche", &env_)) {
        error_ = "CreateEnv: " + status_message(api_, st);
        api_ = nullptr;
        return;
    }
    // DML EP is present only in the DirectML build of onnxruntime.dll.
    const void* dml = nullptr;
    if (OrtStatus* st = api_->GetExecutionProviderApi("DML", ORT_API_VERSION, &dml)) {
        api_->ReleaseStatus(st);
        dml = nullptr;
    }
    dml_api_ = const_cast<void*>(dml);
}

OrtRuntime::~OrtRuntime()
{
    // Intentionally leak env and library: static destruction order vs. DML
    // device teardown is not worth the risk at process exit.
}

std::string OrtRuntime::version() const
{
    if (!handle_) return {};
    using GetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();
    auto get_base = reinterpret_cast<GetApiBaseFn>(find_symbol(handle_, "OrtGetApiBase"));
    if (!get_base) return {};
    const OrtApiBase* base = get_base();
    return base && base->GetVersionString ? base->GetVersionString() : "";
}

std::vector<std::string> OrtRuntime::available_backends() const
{
    std::vector<std::string> v;
    if (!available()) return v;
    if (dml_available()) v.push_back("dml");
    v.push_back("cpu");
    return v;
}

std::string OrtRuntime::resolve_backend(const std::string& requested, std::string& resolved) const
{
    if (!available()) return error_;
    std::string want = requested;
    for (char& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (want.empty() || want == "auto") {
        resolved = dml_available() ? "dml" : "cpu";
        return {};
    }
    if (want == "cpu") { resolved = "cpu"; return {}; }
    if (want == "dml" || want == "directml") {
        if (!dml_available()) return "DirectML backend requested but " + library_path_ + " has no DML execution provider";
        resolved = "dml";
        return {};
    }
    if (want == "cuda") return "CUDA backend is not available in this build yet (planned as an optional runtime package)";
    return "unknown backend '" + requested + "' (use cpu, dml or cuda)";
}

// ---------------------------------------------------------------------------
// OrtModel
// ---------------------------------------------------------------------------
const char* const kCancelled = "__pastiche_cancelled__";

namespace {

// Polls Progress::cancelled() while a session runs and terminates it. Does
// nothing (and starts no thread) when there is no progress sink.
class CancelWatchdog {
public:
    CancelWatchdog(OrtModel& model, Progress* progress)
    {
        if (!progress) return;
        thread_ = std::thread([this, &model, progress]() {
            while (!stop_.load(std::memory_order_relaxed)) {
                if (progress->cancelled()) {
                    fired_.store(true, std::memory_order_relaxed);
                    model.terminate();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }
    ~CancelWatchdog()
    {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) thread_.join();
    }
    bool fired() const { return fired_.load(std::memory_order_relaxed); }

private:
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> fired_{false};
};

} // namespace

OrtModel::OrtModel() = default;

OrtModel::~OrtModel()
{
    const OrtApi* api = OrtRuntime::instance().api();
    if (!api) return;
    if (run_options_) api->ReleaseRunOptions(run_options_);
    if (session_) api->ReleaseSession(session_);
}

std::string OrtModel::load(const std::string& model_path, const std::string& backend, int threads)
{
    OrtRuntime& rt = OrtRuntime::instance();
    if (!rt.available()) return rt.error();
    const OrtApi* api = rt.api();
    if (!file_exists(model_path)) return "model file not found: " + model_path;

    OrtSessionOptions* so = nullptr;
    ORT_CHECK(api->CreateSessionOptions(&so));
    struct SoGuard { const OrtApi* api; OrtSessionOptions* so; ~SoGuard() { if (so) api->ReleaseSessionOptions(so); } } so_guard{api, so};

    ORT_CHECK(api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL));
    ORT_CHECK(api->SetSessionLogSeverityLevel(so, 3));
    if (threads > 0) ORT_CHECK(api->SetIntraOpNumThreads(so, threads));

    if (backend == "dml") {
        if (!rt.dml_available()) return "DML execution provider not available";
        auto* dml = static_cast<DmlApiMirror*>(rt.dml_api());
        // Required by the DML EP.
        ORT_CHECK(api->DisableMemPattern(so));
        ORT_CHECK(api->SetSessionExecutionMode(so, ORT_SEQUENTIAL));
        OrtStatus* st = nullptr;
        if (dml->AppendDML2) {
            DmlDeviceOptions dev{DML_PREF_HIGH_PERFORMANCE, DML_FILTER_GPU};
            st = dml->AppendDML2(so, &dev);
        } else {
            st = dml->AppendDML(so, 0);
        }
        if (st) return "DML execution provider: " + status_message(api, st);
    } else if (backend != "cpu") {
        return "unsupported backend '" + backend + "'";
    }

#ifdef _WIN32
    const std::wstring wpath = utf8_to_wide(model_path);
    OrtStatus* st = api->CreateSession(rt.env(), wpath.c_str(), so, &session_);
#else
    OrtStatus* st = api->CreateSession(rt.env(), model_path.c_str(), so, &session_);
#endif
    if (st) { session_ = nullptr; return "CreateSession(" + path_basename(model_path) + "): " + status_message(api, st); }
    backend_ = backend;

    OrtAllocator* alloc = nullptr;
    ORT_CHECK(api->GetAllocatorWithDefaultOptions(&alloc));
    size_t n_in = 0, n_out = 0;
    ORT_CHECK(api->SessionGetInputCount(session_, &n_in));
    ORT_CHECK(api->SessionGetOutputCount(session_, &n_out));
    inputs_.clear();
    outputs_.clear();
    for (size_t i = 0; i < n_in; ++i) {
        char* name = nullptr;
        ORT_CHECK(api->SessionGetInputName(session_, i, alloc, &name));
        inputs_.emplace_back(name);
        api->AllocatorFree(alloc, name);
    }
    for (size_t i = 0; i < n_out; ++i) {
        char* name = nullptr;
        ORT_CHECK(api->SessionGetOutputName(session_, i, alloc, &name));
        outputs_.emplace_back(name);
        api->AllocatorFree(alloc, name);
    }
    ORT_CHECK(api->CreateRunOptions(&run_options_));
    return {};
}

std::string OrtModel::run(const std::vector<std::string>& in_names,
                          const std::vector<const float*>& in_data,
                          const std::vector<std::vector<int64_t>>& in_shapes,
                          const std::vector<std::string>& out_names,
                          std::vector<std::vector<float>>& out_data,
                          std::vector<std::vector<int64_t>>& out_shapes,
                          Progress* progress)
{
    const OrtApi* api = OrtRuntime::instance().api();
    if (!api || !session_) return "model not loaded";
    if (in_names.size() != in_data.size() || in_names.size() != in_shapes.size()) return "run: input count mismatch";
    if (progress && progress->cancelled()) return kCancelled;

    // A single Run() on a large image takes seconds and cannot be polled from
    // the inside, so a watchdog thread flips the ORT terminate flag when the
    // user cancels (D9). The flag is cleared again before every run.
    api->RunOptionsUnsetTerminate(run_options_);
    CancelWatchdog watchdog(*this, progress);

    OrtMemoryInfo* mem = nullptr;
    ORT_CHECK(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem));
    struct MemGuard { const OrtApi* api; OrtMemoryInfo* m; ~MemGuard() { api->ReleaseMemoryInfo(m); } } mem_guard{api, mem};

    std::vector<OrtValue*> inputs(in_names.size(), nullptr);
    std::vector<OrtValue*> outputs(out_names.size(), nullptr);
    struct ValGuard {
        const OrtApi* api; std::vector<OrtValue*>& a; std::vector<OrtValue*>& b;
        ~ValGuard() { for (auto* v : a) if (v) api->ReleaseValue(v); for (auto* v : b) if (v) api->ReleaseValue(v); }
    } val_guard{api, inputs, outputs};

    std::vector<const char*> in_c, out_c;
    for (size_t i = 0; i < in_names.size(); ++i) {
        size_t count = 1;
        for (int64_t d : in_shapes[i]) count *= static_cast<size_t>(std::max<int64_t>(d, 0));
        ORT_CHECK(api->CreateTensorWithDataAsOrtValue(mem, const_cast<float*>(in_data[i]), count * sizeof(float),
                                                      in_shapes[i].data(), in_shapes[i].size(),
                                                      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[i]));
        in_c.push_back(in_names[i].c_str());
    }
    for (const std::string& n : out_names) out_c.push_back(n.c_str());

    OrtStatus* st = api->Run(session_, run_options_, in_c.data(), inputs.data(), inputs.size(),
                             out_c.data(), out_c.size(), outputs.data());
    if (st) {
        const std::string msg = status_message(api, st);
        if (watchdog.fired()) return kCancelled;   // terminated on purpose
        return "Run: " + msg;
    }

    out_data.assign(out_names.size(), {});
    out_shapes.assign(out_names.size(), {});
    for (size_t i = 0; i < outputs.size(); ++i) {
        OrtTensorTypeAndShapeInfo* info = nullptr;
        ORT_CHECK(api->GetTensorTypeAndShape(outputs[i], &info));
        size_t ndim = 0;
        api->GetDimensionsCount(info, &ndim);
        out_shapes[i].resize(ndim);
        api->GetDimensions(info, out_shapes[i].data(), ndim);
        ONNXTensorElementDataType type;
        api->GetTensorElementType(info, &type);
        api->ReleaseTensorTypeAndShapeInfo(info);
        if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) return "output '" + out_names[i] + "' is not float32";
        size_t count = 1;
        for (int64_t d : out_shapes[i]) count *= static_cast<size_t>(std::max<int64_t>(d, 0));
        void* ptr = nullptr;
        ORT_CHECK(api->GetTensorMutableData(outputs[i], &ptr));
        out_data[i].assign(static_cast<float*>(ptr), static_cast<float*>(ptr) + count);
    }
    return {};
}

std::string OrtModel::run(const float* input, const std::vector<int64_t>& in_shape,
                          std::vector<float>& output, std::vector<int64_t>& out_shape,
                          Progress* progress)
{
    if (inputs_.size() != 1 || outputs_.size() != 1)
        return "model has " + std::to_string(inputs_.size()) + " inputs and " + std::to_string(outputs_.size()) +
               " outputs; expected 1 and 1";
    std::vector<std::vector<float>> outs;
    std::vector<std::vector<int64_t>> shapes;
    const std::string err = run({inputs_[0]}, {input}, {in_shape}, {outputs_[0]}, outs, shapes, progress);
    if (!err.empty()) return err;
    output = std::move(outs[0]);
    out_shape = std::move(shapes[0]);
    return {};
}

void OrtModel::terminate()
{
    const OrtApi* api = OrtRuntime::instance().api();
    if (api && run_options_) api->RunOptionsSetTerminate(run_options_);
}

// ---------------------------------------------------------------------------
// VRAM preflight
// ---------------------------------------------------------------------------
std::string human_size(uint64_t b)
{
    char buf[64];
    if (b >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.2f GiB", b / double(1ull << 30));
    else std::snprintf(buf, sizeof buf, "%.0f MiB", b / double(1ull << 20));
    return buf;
}

std::string gpu_usage_line(const GpuMemoryInfo& before, const GpuMemoryInfo& after, uint64_t estimate)
{
    if (!before.ok || !after.ok) return {};
    const uint64_t delta = after.current_usage > before.current_usage ? after.current_usage - before.current_usage : 0;
    return "gpu " + after.adapter + ": usage " + human_size(before.current_usage) + " -> " +
           human_size(after.current_usage) + " (delta " + human_size(delta) + ", estimate " + human_size(estimate) +
           ", budget " + human_size(after.budget) + ")";
}

int vram_suggest_size(const IStyleAlgorithm& algo, int w, int h, const Params& p, const RunOptions& opts,
                      uint64_t available)
{
    // Activations scale with area, so walk the longer side down in 64 px steps.
    const int longest = std::max(w, h);
    for (int n = (longest / 64) * 64 - 64; n >= 256; n -= 64) {
        const double s = static_cast<double>(n) / longest;
        const int sw = std::max(1, static_cast<int>(std::lround(w * s)));
        const int sh = std::max(1, static_cast<int>(std::lround(h * s)));
        if (algo.estimate_vram(sw, sh, p, opts) <= available) return n;
    }
    return 0;
}

std::string vram_preflight(const IStyleAlgorithm& algo, int w, int h, const Params& p, const RunOptions& opts)
{
    std::string backend;
    const std::string err = OrtRuntime::instance().resolve_backend(opts.backend, backend);
    if (!err.empty()) return err;
    if (backend == "cpu") return {};

    const uint64_t need = algo.estimate_vram(w, h, p, opts);
    if (need == 0) return {};
    const GpuMemoryInfo gpu = query_gpu_memory();
    if (!gpu.ok) return {};  // cannot measure: do not block the user
    const uint64_t avail = gpu.available();
    if (need <= avail) return {};

    const int suggest = vram_suggest_size(algo, w, h, p, opts, avail);
    std::string msg = "not enough GPU memory on " + gpu.adapter + ": " + algo.id() + " needs about " + human_size(need) +
                      " for " + std::to_string(w) + "x" + std::to_string(h) + ", " + human_size(avail) + " is free (of " +
                      human_size(gpu.budget) + " budget).";
    if (suggest > 0) msg += " Scale the content down with --size " + std::to_string(suggest) + ".";
    else msg += " Scale the content down with --size (even 256 px does not fit).";
    if (algo.supports_tiling() && !opts.tile) msg += " Alternatively process in tiles with --tile (may show seams).";
    msg += " Or run on the CPU with --backend cpu.";
    return msg;
}

} // namespace pastiche
