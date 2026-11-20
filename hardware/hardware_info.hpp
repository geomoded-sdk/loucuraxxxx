// LoucuraXXXX GPU - host hardware description.
//
// All fields are filled from real operating-system queries. Nothing here is
// fabricated: when a value cannot be determined it is reported as unknown
// (zero / empty) rather than guessed.
#pragma once

#include <string>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

namespace lx::hardware {

// How a GPU was discovered. Kept explicit so callers never confuse a real
// discrete/integrated GPU with a software rasterizer.
enum class GpuKind {
    Unknown,
    Discrete,    // Dedicated card.
    Integrated,  // CPU-integrated graphics.
    Virtual,     // Paravirtualized adapter exposed by a hypervisor.
    Software,    // CPU rasterizer (e.g. Mesa lavapipe). Not real hardware.
};

inline const char* to_string(GpuKind kind) {
    switch (kind) {
        case GpuKind::Unknown: return "unknown";
        case GpuKind::Discrete: return "discrete";
        case GpuKind::Integrated: return "integrated";
        case GpuKind::Virtual: return "virtual";
        case GpuKind::Software: return "software";
    }
    return "unknown";
}

// Which graphics API a device was enumerated through.
enum class GraphicsApi { None, Vulkan, Metal, Direct3D11, Direct3D12, OpenGL, Software };

inline const char* to_string(GraphicsApi api) {
    switch (api) {
        case GraphicsApi::None: return "none";
        case GraphicsApi::Vulkan: return "vulkan";
        case GraphicsApi::Metal: return "metal";
        case GraphicsApi::Direct3D11: return "d3d11";
        case GraphicsApi::Direct3D12: return "d3d12";
        case GraphicsApi::OpenGL: return "opengl";
        case GraphicsApi::Software: return "software";
    }
    return "none";
}

struct GpuDeviceInfo {
    std::string name;
    std::string vendor;         // e.g. "NVIDIA", "AMD", "Intel", "Mesa"
    std::string driver;         // driver / ICD identifier when known
    u32 vendor_id = 0;
    u32 device_id = 0;
    u64 dedicated_vram_bytes = 0;  // 0 when unknown or shared
    u64 shared_memory_bytes = 0;
    bool is_removable = false;
    GpuKind kind = GpuKind::Unknown;
    GraphicsApi api = GraphicsApi::None;
    std::string api_version;    // reported API version string, when known

    // True when this device reads and writes the same physical memory the CPU
    // uses, so no host<->device copy is required. Apple silicon GPUs are
    // unified; so are most integrated GPUs. This is a *hardware* fact reported
    // by the OS, not a capability of any API on top of it.
    bool unified_memory = false;

    // Number of hardware queues the driver advertises, when known. Apple GPUs
    // expose multiple Metal command queues; integrated GPUs share one engine.
    u32 hardware_queue_count = 0;
};

// How the CPU and GPU address memory. This is the single fact that most changes
// how the layer should schedule and move data, so it is modelled explicitly
// rather than inferred at each call site.
enum class MemoryTopology {
    Unknown,   // not determined; treat conservatively as discrete
    Unified,   // CPU and GPU share one physical pool (Apple silicon, most iGPUs)
    Discrete,  // separate device memory; transfers are required
};

inline const char* to_string(MemoryTopology topology) {
    switch (topology) {
        case MemoryTopology::Unknown: return "unknown";
        case MemoryTopology::Unified: return "unified";
        case MemoryTopology::Discrete: return "discrete";
    }
    return "unknown";
}

struct CpuInfo {
    std::string model;
    std::string vendor;
    u32 logical_cores = 0;
    u32 physical_cores = 0;
    u32 performance_cores = 0;  // big cores on hybrid architectures, 0 if unknown
    u32 efficiency_cores = 0;   // little cores on hybrid architectures, 0 if unknown
    u64 l2_cache_bytes = 0;
    u64 l3_cache_bytes = 0;
    u64 max_frequency_hz = 0;
    bool supports_avx2 = false;
    bool supports_avx512 = false;
    bool supports_neon = false;
    std::vector<std::string> isa_features;
};

struct MemoryInfo {
    u64 total_bytes = 0;
    u64 available_bytes = 0;
    u64 page_size_bytes = 0;
    bool has_swap = false;
    u64 swap_total_bytes = 0;
};

struct OperatingSystemInfo {
    std::string name;        // "Linux", "Windows", "macOS"
    std::string version;
    std::string kernel;
    std::string architecture; // "x86_64", "aarch64", ...
    bool is_64_bit = true;
};

struct HardwareInfo {
    CpuInfo cpu;
    MemoryInfo memory;
    OperatingSystemInfo os;
    std::vector<GpuDeviceInfo> gpus;

    // Convenience: first device that is real hardware (not software).
    const GpuDeviceInfo* primary_gpu() const {
        for (const auto& gpu : gpus) {
            if (gpu.kind != GpuKind::Software) return &gpu;
        }
        return gpus.empty() ? nullptr : &gpus.front();
    }

    bool has_usable_gpu() const { return primary_gpu() != nullptr; }

    // How the CPU and the primary GPU address memory. Derived from the detected
    // devices: unified when the primary GPU shares the host pool, discrete when
    // it has its own, unknown when there is no usable GPU to ask.
    MemoryTopology memory_topology() const {
        const GpuDeviceInfo* gpu = primary_gpu();
        if (gpu == nullptr) return MemoryTopology::Unknown;
        if (gpu->unified_memory) return MemoryTopology::Unified;
        if (gpu->dedicated_vram_bytes > 0) return MemoryTopology::Discrete;
        // An integrated device with no dedicated VRAM is unified by definition.
        if (gpu->kind == GpuKind::Integrated) return MemoryTopology::Unified;
        return MemoryTopology::Unknown;
    }

    bool has_unified_memory() const {
        return memory_topology() == MemoryTopology::Unified;
    }

    // True when the CPU has distinguishable performance and efficiency cores.
    bool is_hybrid_cpu() const {
        return cpu.performance_cores > 0 && cpu.efficiency_cores > 0;
    }
};

// Detects CPU, memory and OS information from the running system.
Result<HardwareInfo> detect_hardware();

// Enumerates GPUs visible to the operating system at the driver level
// (DRM/sysfs on Linux, DXGI on Windows, Metal on macOS). This does not require
// a graphics API context to be created.
Result<std::vector<GpuDeviceInfo>> enumerate_system_gpus();

// Returns the number of logical processors reported by the OS.
u32 logical_processor_count();

}  // namespace lx::hardware
