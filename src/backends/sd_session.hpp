#pragma once

// Thin wrapper over the stable-diffusion.cpp C API. The library is loaded at
// run time (LoadLibrary / dlopen) and never linked (D20), so the executables
// build and start without it; the diffusion algorithm reports a clear error
// instead of the process failing to start. Build the library with
// tools/build_sdcpp.sh (D21).
#include <cstddef>
#include <string>
#include <vector>

namespace pastiche {

// One entry of sd_list_devices(), which returns "name<TAB>description" lines.
struct SdDevice {
    std::string name;         // backend spec name, e.g. "vulkan0", "cpu"
    std::string description;  // human readable, e.g. "Quadro T2000"
    bool is_cpu = false;
};

// Routes stable-diffusion.cpp / ggml log output to stderr. Off by default:
// merely enumerating devices makes ggml print a multi-line Vulkan banner, which
// would corrupt the output of things like --version. Call this before the first
// use of SdRuntime, the library is only touched on first access.
void sd_set_logging(bool enabled);

class SdRuntime {
public:
    static SdRuntime& instance();

    bool available() const { return handle_ != nullptr; }
    const std::string& error() const { return error_; }        // why loading failed
    const std::string& library_path() const { return library_path_; }
    std::string version() const;                               // "master-859 (7f410a3)"

    const std::vector<SdDevice>& devices() const { return devices_; }

    // The device the diffusion algorithm uses unless the user overrides it.
    // ggml orders devices as the driver reports them, which on a hybrid laptop
    // puts the integrated GPU first - picking devices[0] would quietly run on
    // the Intel iGPU instead of the discrete card. So we match against the
    // adapter gpu_info already selected for the VRAM preflight (the biggest
    // non-software one), which is the same choice DirectML's HighPerformance
    // filter makes for the ORT path. Falls back to the first non-CPU device,
    // then to "cpu". Empty only when the library is unavailable.
    std::string preferred_device() const;

    // Looks up a device by its backend spec name. Returns nullptr if absent,
    // so callers can reject a bad --device with the list of valid names.
    const SdDevice* find_device(const std::string& name) const;

private:
    SdRuntime();
    ~SdRuntime();
    SdRuntime(const SdRuntime&) = delete;
    SdRuntime& operator=(const SdRuntime&) = delete;

    void query_devices();

    void* handle_ = nullptr;
    std::string error_;
    std::string library_path_;
    std::vector<SdDevice> devices_;

    // Entry points resolved from the library. More will join them when the
    // diffusion algorithm lands; these are the ones needed to report what is
    // available without loading a model.
    const char* (*fn_version_)() = nullptr;
    const char* (*fn_commit_)() = nullptr;
    std::size_t (*fn_list_devices_)(char*, std::size_t) = nullptr;
    void (*fn_set_log_callback_)(void (*)(int, const char*, void*), void*) = nullptr;
};

} // namespace pastiche
