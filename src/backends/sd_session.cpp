#include "sd_session.hpp"

#include "core/fs.hpp"
#include "core/gpu_info.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef PASTICHE_SD_DIR
#define PASTICHE_SD_DIR ""
#endif

namespace pastiche {

namespace {

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

void close_library(void* handle)
{
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

// Same search order as the ONNX Runtime loader (D16): explicit environment
// override, next to the executable, the path baked in at compile time, then
// the system search path. MinGW names the shared library libstable-diffusion,
// but accept the MSVC-style name too so a library from elsewhere still works.
std::vector<std::string> candidate_library_paths()
{
#ifdef _WIN32
    const char* names[] = {"libstable-diffusion.dll", "stable-diffusion.dll"};
#elif defined(__APPLE__)
    const char* names[] = {"libstable-diffusion.dylib"};
#else
    const char* names[] = {"libstable-diffusion.so"};
#endif
    std::vector<std::string> out;
    const std::string env = getenv_utf8("PASTICHE_SD_DIR");
    const std::string compiled = PASTICHE_SD_DIR;
    for (const char* name : names) {
        if (!env.empty()) out.push_back(path_join(env, name));
        out.push_back(path_join(exe_dir(), name));
        if (!compiled.empty()) out.push_back(path_join(compiled, name));
        out.push_back(name);  // system search path
    }
    return out;
}

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> tokenize(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : to_lower(s)) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            cur += c;
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// How well a ggml device description matches a DXGI adapter name. Vendor
// prefixes differ between the two APIs ("NVIDIA Quadro T2000" vs "Quadro
// T2000"), so compare shared tokens rather than requiring equality. Tokens
// shorter than three characters are skipped, they match too eagerly.
// Whether library log output reaches stderr. Read from the log callback, which
// ggml may call from its worker threads.
std::atomic<bool> g_logging{false};

// Mirrors sd_log_level_t; only used to label forwarded lines.
const char* log_level_name(int level)
{
    switch (level) {
    case 0:  return "debug";
    case 1:  return "info";
    case 2:  return "warn";
    default: return "error";
    }
}

void log_callback(int level, const char* text, void*)
{
    if (!g_logging.load(std::memory_order_relaxed) || !text) return;
    std::fprintf(stderr, "[sd %s] %s", log_level_name(level), text);
}

// Redirects stderr to the null device for its lifetime.
//
// Enumerating devices initialises the ggml backend registry, and ggml-vulkan
// writes a multi-line "Found N Vulkan devices" banner straight to stderr. It
// cannot be intercepted: sd.cpp only routes ggml's log into its own callback
// from inside new_sd_ctx (diffusion_engine.cpp), which has not run yet, and
// ggml_log_set is not among the symbols the shared library exports. Once a
// context exists the banner problem goes away, so this covers exactly the
// enumeration call and nothing else.
class StderrSilencer {
public:
    explicit StderrSilencer(bool active)
    {
        if (!active) return;
        std::fflush(stderr);
#ifdef _WIN32
        const int fd = _fileno(stderr);
        saved_ = _dup(fd);
        const int null_fd = _open("NUL", _O_WRONLY);
        if (saved_ >= 0 && null_fd >= 0) { _dup2(null_fd, fd); }
        if (null_fd >= 0) _close(null_fd);
#else
        const int fd = fileno(stderr);
        saved_ = dup(fd);
        const int null_fd = open("/dev/null", O_WRONLY);
        if (saved_ >= 0 && null_fd >= 0) { dup2(null_fd, fd); }
        if (null_fd >= 0) close(null_fd);
#endif
    }

    ~StderrSilencer()
    {
        if (saved_ < 0) return;
        std::fflush(stderr);
#ifdef _WIN32
        _dup2(saved_, _fileno(stderr));
        _close(saved_);
#else
        dup2(saved_, fileno(stderr));
        close(saved_);
#endif
    }

    StderrSilencer(const StderrSilencer&) = delete;
    StderrSilencer& operator=(const StderrSilencer&) = delete;

private:
    int saved_ = -1;
};

int match_score(const std::string& a, const std::string& b)
{
    const std::vector<std::string> ta = tokenize(a);
    const std::vector<std::string> tb = tokenize(b);
    int score = 0;
    for (const std::string& x : ta) {
        if (x.size() < 3) continue;
        if (std::find(tb.begin(), tb.end(), x) != tb.end()) ++score;
    }
    return score;
}

} // namespace

void sd_set_logging(bool enabled)
{
    g_logging.store(enabled, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// SdRuntime
// ---------------------------------------------------------------------------
SdRuntime& SdRuntime::instance()
{
    static SdRuntime rt;
    return rt;
}

SdRuntime::SdRuntime()
{
    const std::vector<std::string> candidates = candidate_library_paths();
    std::string errors;
    for (const std::string& path : candidates) {
        if (path.find('/') != std::string::npos || path.find('\\') != std::string::npos) {
            if (!file_exists(path)) continue;
        }
        std::string err;
        handle_ = open_library(path, err);
        if (handle_) { library_path_ = path; break; }
        errors += "\n  " + path + ": " + err;
    }
    if (!handle_) {
        error_ = "stable-diffusion library not found (build it with tools/build_sdcpp.sh). Looked for:";
        for (const std::string& p : candidates) error_ += "\n  " + p;
        if (!errors.empty()) error_ += "\nload errors:" + errors;
        return;
    }

    // Install the log sink first: querying devices already initialises the ggml
    // backends, which print a Vulkan banner unless the callback swallows it.
    fn_set_log_callback_ = reinterpret_cast<void (*)(void (*)(int, const char*, void*), void*)>(
        find_symbol(handle_, "sd_set_log_callback"));
    if (fn_set_log_callback_) fn_set_log_callback_(&log_callback, nullptr);

    fn_version_ = reinterpret_cast<const char* (*)()>(find_symbol(handle_, "sd_version"));
    fn_commit_ = reinterpret_cast<const char* (*)()>(find_symbol(handle_, "sd_commit"));
    fn_list_devices_ =
        reinterpret_cast<std::size_t (*)(char*, std::size_t)>(find_symbol(handle_, "sd_list_devices"));

    if (!fn_list_devices_) {
        error_ = "sd_list_devices not exported by " + library_path_ +
                 " - the library is too old or not stable-diffusion.cpp";
        close_library(handle_);
        handle_ = nullptr;
        return;
    }

    query_devices();
}

SdRuntime::~SdRuntime()
{
    // Deliberately not unloading: ggml registers backends in static storage and
    // spins up worker threads, so freeing the library at exit races with them.
    // The ORT loader leaks its handle for the same reason.
}

std::string SdRuntime::version() const
{
    if (!handle_) return {};
    const char* ver = fn_version_ ? fn_version_() : nullptr;
    const char* commit = fn_commit_ ? fn_commit_() : nullptr;
    std::string out = ver ? ver : "unknown";
    if (commit && *commit) out += " (" + std::string(commit) + ")";
    return out;
}

void SdRuntime::query_devices()
{
    std::string buf;
    {
        const StderrSilencer quiet(!g_logging.load(std::memory_order_relaxed));
        const std::size_t needed = fn_list_devices_(nullptr, 0);
        if (needed == 0) return;
        buf.assign(needed + 1, '\0');
        const std::size_t written = fn_list_devices_(&buf[0], buf.size());
        buf.resize(std::min(written, needed));
    }

    std::size_t pos = 0;
    while (pos < buf.size()) {
        std::size_t eol = buf.find('\n', pos);
        if (eol == std::string::npos) eol = buf.size();
        const std::string line = buf.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;

        SdDevice dev;
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            dev.name = line;
        } else {
            dev.name = line.substr(0, tab);
            dev.description = line.substr(tab + 1);
        }
        if (!dev.name.empty() && dev.name.back() == '\r') dev.name.pop_back();
        if (!dev.description.empty() && dev.description.back() == '\r') dev.description.pop_back();
        if (dev.name.empty()) continue;
        dev.is_cpu = to_lower(dev.name) == "cpu";
        devices_.push_back(std::move(dev));
    }
}

const SdDevice* SdRuntime::find_device(const std::string& name) const
{
    const std::string wanted = to_lower(name);
    for (const SdDevice& d : devices_) {
        if (to_lower(d.name) == wanted) return &d;
    }
    return nullptr;
}

std::string SdRuntime::preferred_device() const
{
    if (devices_.empty()) return {};

    const GpuMemoryInfo gpu = query_gpu_memory();
    if (gpu.ok && !gpu.adapter.empty()) {
        const SdDevice* best = nullptr;
        int best_score = 0;
        for (const SdDevice& d : devices_) {
            if (d.is_cpu) continue;
            const int score = match_score(gpu.adapter, d.description);
            if (score > best_score) {
                best_score = score;
                best = &d;
            }
        }
        if (best) return best->name;
    }

    for (const SdDevice& d : devices_) {
        if (!d.is_cpu) return d.name;
    }
    return devices_.front().name;
}

} // namespace pastiche
