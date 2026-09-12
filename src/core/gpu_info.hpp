#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pastiche {

struct GpuMemoryInfo {
    bool ok = false;
    std::string adapter;            // e.g. "NVIDIA Quadro T2000"
    uint64_t dedicated_total = 0;   // bytes of dedicated video memory
    uint64_t budget = 0;            // OS-granted budget for this process (Windows) or total
    uint64_t current_usage = 0;     // bytes currently used by this process
    std::string error;

    uint64_t available() const { return budget > current_usage ? budget - current_usage : 0; }
};

struct GpuAdapterInfo {
    std::string name;
    uint64_t dedicated_total = 0;
    bool software = false;
};

// Free GPU memory of the adapter the GPU backends will use: the non-software
// adapter with the most dedicated memory (matches the DirectML "high
// performance" preference on laptops with an integrated + discrete GPU).
// Windows: DXGI QueryVideoMemoryInfo. Elsewhere: ok == false for now.
GpuMemoryInfo query_gpu_memory();

std::vector<GpuAdapterInfo> list_gpu_adapters();

} // namespace pastiche
