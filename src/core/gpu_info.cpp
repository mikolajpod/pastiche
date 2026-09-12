#include "gpu_info.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>

#include "fs.hpp"

namespace pastiche {

namespace {

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Enumerates hardware adapters; returns the index of the one with the most
// dedicated memory, or -1.
int enumerate(IDXGIFactory1* factory, std::vector<GpuAdapterInfo>& out, std::vector<IDXGIAdapter1*>* keep)
{
    int best = -1;
    uint64_t best_mem = 0;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        GpuAdapterInfo info;
        info.name = wide_to_utf8(desc.Description);
        info.dedicated_total = desc.DedicatedVideoMemory;
        info.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        out.push_back(info);
        if (!info.software && info.dedicated_total > best_mem) { best_mem = info.dedicated_total; best = static_cast<int>(i); }
        if (keep) keep->push_back(adapter); else adapter->Release();
    }
    return best;
}

} // namespace

std::vector<GpuAdapterInfo> list_gpu_adapters()
{
    std::vector<GpuAdapterInfo> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void**>(&factory)))) return out;
    enumerate(factory.p, out, nullptr);
    return out;
}

GpuMemoryInfo query_gpu_memory()
{
    GpuMemoryInfo r;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void**>(&factory)))) {
        r.error = "CreateDXGIFactory1 failed";
        return r;
    }
    std::vector<GpuAdapterInfo> infos;
    std::vector<IDXGIAdapter1*> adapters;
    const int best = enumerate(factory.p, infos, &adapters);
    if (best < 0) {
        for (auto* a : adapters) a->Release();
        r.error = "no hardware GPU adapter found";
        return r;
    }
    ComPtr<IDXGIAdapter3> a3;
    const HRESULT hr = adapters[best]->QueryInterface(IID_IDXGIAdapter3, reinterpret_cast<void**>(&a3));
    for (auto* a : adapters) a->Release();
    if (FAILED(hr) || !a3) {
        r.error = "IDXGIAdapter3 not available (needs Windows 10)";
        return r;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO mem{};
    if (FAILED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mem))) {
        r.error = "QueryVideoMemoryInfo failed";
        return r;
    }
    r.ok = true;
    r.adapter = infos[best].name;
    r.dedicated_total = infos[best].dedicated_total;
    r.budget = mem.Budget;
    r.current_usage = mem.CurrentUsage;
    return r;
}

} // namespace pastiche

#else  // !_WIN32

namespace pastiche {

std::vector<GpuAdapterInfo> list_gpu_adapters() { return {}; }

GpuMemoryInfo query_gpu_memory()
{
    GpuMemoryInfo r;
    r.error = "GPU memory query not implemented on this platform";
    return r;
}

} // namespace pastiche

#endif
