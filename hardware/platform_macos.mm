// LoucuraXXXX GPU - macOS hardware detection.
//
// Compiled only on macOS. Uses sysctl (sysctlbyname) and Metal for adapter
// enumeration. Marked experimental: written against documented APIs but not
// exercised on a macOS host in this repository's CI.
#if defined(__APPLE__)

#include "hardware/platform.hpp"

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstring>
#include <string>
#include <vector>

namespace lx::hardware::platform {

namespace {

std::string sysctl_string(const char* name) {
    std::size_t size = 0;
    if (::sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return {};
    std::string value(size, '\0');
    if (::sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) return {};
    if (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

u64 sysctl_u64(const char* name) {
    u64 value = 0;
    std::size_t size = sizeof(value);
    ::sysctlbyname(name, &value, &size, nullptr, 0);
    return value;
}

u32 sysctl_u32(const char* name) {
    u32 value = 0;
    std::size_t size = sizeof(value);
    ::sysctlbyname(name, &value, &size, nullptr, 0);
    return value;
}

GpuKind classify_apple_gpu(bool unified_memory) {
    // Unified memory means the GPU draws from the system pool, which is the
    // defining property of an integrated device; separate memory means a
    // discrete card. This holds for Apple silicon and for Intel Macs alike.
    return unified_memory ? GpuKind::Integrated : GpuKind::Discrete;
}

}  // namespace

void detect_cpu(CpuInfo& out) {
    out.model = sysctl_string("machdep.cpu.brand_string");
    if (out.model.empty()) out.model = sysctl_string("hw.model");
    out.vendor = sysctl_string("machdep.cpu.vendor");
    out.logical_cores = sysctl_u32("hw.logicalcpu");
    out.physical_cores = sysctl_u32("hw.physicalcpu");
    out.l2_cache_bytes = sysctl_u64("hw.l2cachesize");
    out.l3_cache_bytes = sysctl_u64("hw.l3cachesize");
    out.max_frequency_hz = sysctl_u64("hw.cpufrequency_max");

    // Apple silicon: performance vs efficiency core counts are published.
    const u32 perf = sysctl_u32("hw.perflevel0.physicalcpu");
    const u32 eff = sysctl_u32("hw.perflevel1.physicalcpu");
    if (perf > 0 && eff > 0) {
        out.performance_cores = perf;
        out.efficiency_cores = eff;
    }

    // Intel Macs publish the feature string; Apple silicon exposes NEON as an
    // integer sysctl (which always reads 1 on arm64). Only the string sysctl is
    // parsed as text.
    const std::string features = sysctl_string("machdep.cpu.features");
    if (features.find("AVX2") != std::string::npos) {
        out.supports_avx2 = true;
        out.isa_features.emplace_back("avx2");
    }
#if defined(__aarch64__)
    out.supports_neon = true;
    out.isa_features.emplace_back("neon");
#else
    if (sysctl_u32("hw.optional.arm.FEAT_NEON") != 0) {
        out.supports_neon = true;
        out.isa_features.emplace_back("neon");
    }
#endif
}

void detect_memory(MemoryInfo& out) {
    out.total_bytes = sysctl_u64("hw.memsize");
    out.page_size_bytes = static_cast<u64>(::getpagesize());

    // Available memory is not a sysctl; ask the kernel directly. vm_statistics64
    // gives the free/inactive/... page counts and macOS reports reclaimable
    // memory (free + inactive + purgeable) as what a new reservation can take
    // without forcing the system to swap. Leaving this at total_bytes would let
    // the reservation policy believe the whole machine is free.
    vm_statistics64_data_t stats {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (::host_statistics64(::mach_host_self(), HOST_VM_INFO64,
                            reinterpret_cast<host_info64_t>(&stats), &count) == KERN_SUCCESS) {
        const u64 page = out.page_size_bytes != 0 ? out.page_size_bytes : 4096;
        const u64 reclaimable =
            (static_cast<u64>(stats.free_count) + stats.inactive_count +
             stats.purgeable_count) *
            page;
        out.available_bytes = reclaimable;
        out.swap_total_bytes = 0;  // macOS does not expose a fixed swap size
        out.has_swap = stats.swapins > 0 || stats.swapouts > 0;
    } else {
        // Fall back to a conservative half of physical memory rather than
        // claiming all of it is available.
        out.available_bytes = out.total_bytes / 2;
    }
    if (out.available_bytes > out.total_bytes) out.available_bytes = out.total_bytes;
}

void detect_os(OperatingSystemInfo& out) {
    out.name = "macOS";
    out.kernel = sysctl_string("kern.osrelease");
    out.version = sysctl_string("kern.osproductversion");
    out.architecture = sysctl_string("hw.machine");
    out.is_64_bit = sizeof(void*) == 8;
}

std::vector<GpuDeviceInfo> detect_system_gpus() {
    std::vector<GpuDeviceInfo> gpus;

    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    if (devices == nil) {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device != nil) devices = @[ device ];
    }

    for (id<MTLDevice> device in devices) {
        GpuDeviceInfo info;
        info.name = [device.name UTF8String];
        info.vendor = "Apple";
        info.driver = "Metal";
        // Ask the device itself whether it shares the CPU's memory pool. This is
        // the authoritative answer and it differs between an Apple silicon Mac
        // (true) and an Intel Mac with a discrete GPU (false), so it must not be
        // inferred from the CPU architecture.
        const bool unified = [device hasUnifiedMemory];
        info.unified_memory = unified;
        info.dedicated_vram_bytes = unified ? 0 : [device recommendedMaxWorkingSetSize];
        info.shared_memory_bytes = unified ? [device recommendedMaxWorkingSetSize] : 0;
        info.kind = classify_apple_gpu(unified);
        info.api = GraphicsApi::Metal;
        // Metal does not expose how many independent command queues the hardware
        // has, so this is left unknown (0) rather than guessed at a fixed number.
        info.hardware_queue_count = 0;
        gpus.push_back(std::move(info));
    }
    return gpus;
}

u32 logical_processor_count() {
    return sysctl_u32("hw.logicalcpu");
}

}  // namespace lx::hardware::platform

#endif  // __APPLE__
