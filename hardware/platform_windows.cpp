// LoucuraXXXX GPU - Windows hardware detection.
//
// Compiled only on Windows. Uses the documented Win32 APIs (GlobalMemoryStatusEx,
// GetLogicalProcessorInformationEx, registry) and DXGI for adapter enumeration.
// This file is marked experimental: it is written against the documented APIs
// but has not been exercised on a Windows host in this repository's CI.
#if defined(_WIN32)

#include "hardware/platform.hpp"

#include <windows.h>
#include <dxgi.h>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

#include "backends/backend.hpp"

namespace lx::hardware::platform {

namespace {

std::string wide_to_utf8(const wchar_t* wide) {
    if (!wide) return {};
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    // len includes the terminating NUL, so the buffer must be len bytes to let
    // the API write that terminator; only then do we drop it.
    std::string out(static_cast<std::size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(len - 1));
    return out;
}

std::string read_registry_string(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    wchar_t buffer[512];
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    if (::RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, &type, buffer, &size) ==
        ERROR_SUCCESS) {
        return wide_to_utf8(buffer);
    }
    return {};
}

GpuKind classify_dxgi_adapter(DXGI_ADAPTER_DESC1& desc) {
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return GpuKind::Software;
    // Microsoft Basic Render Driver / WARP.
    if (desc.VendorId == 0x1414) return GpuKind::Software;
    // Wine builds this descriptor from its own card database while running the
    // calls on the host rasterizer, so the card it names is not present.
    if (backends::running_under_wine()) return GpuKind::Software;
    if (desc.VendorId == 0x1AF4 || desc.VendorId == 0x15AD || desc.VendorId == 0x1234) {
        return GpuKind::Virtual;
    }
    // A dedicated video memory pool larger than 256 MiB implies discrete.
    if (desc.DedicatedVideoMemory > 256ull * 1024 * 1024) return GpuKind::Discrete;
    return GpuKind::Integrated;
}

// Reads a CPUID leaf/subleaf into four 32-bit registers on both MSVC and GCC.
void cpuid_regs(int leaf, int subleaf, int out[4]) {
#if defined(_MSC_VER)
    __cpuidex(out, leaf, subleaf);
#else
    unsigned int a = 0, b = 0, c = 0, d = 0;
    __cpuid_count(leaf, subleaf, a, b, c, d);
    out[0] = static_cast<int>(a);
    out[1] = static_cast<int>(b);
    out[2] = static_cast<int>(c);
    out[3] = static_cast<int>(d);
#endif
}

// OSXSAVE + XGETBV check: AVX/AVX2/AVX-512 registers are only usable when the
// OS has enabled XSAVE for the corresponding state components. Reporting the
// CPUID feature bit without this check would claim support the OS may not give.
bool os_supports_ymm() {
    int regs[4];
    cpuid_regs(1, 0, regs);
    const bool osxsave = (regs[2] & (1 << 27)) != 0;
    if (!osxsave) return false;
#if defined(_MSC_VER)
    const unsigned long long xcr0 = _xgetbv(0);
#else
    unsigned int eax = 0, edx = 0;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    const unsigned long long xcr0 = (static_cast<unsigned long long>(edx) << 32) | eax;
#endif
    return (xcr0 & 0x6) == 0x6;  // XMM and YMM state enabled
}

std::string vendor_name_from_id(UINT vendor_id) {
    switch (vendor_id) {
        case 0x10DE: return "NVIDIA";
        case 0x1002:
        case 0x1022: return "AMD";
        case 0x8086: return "Intel";
        case 0x1414: return "Microsoft";
        case 0x1AF4: return "Red Hat (virtio)";
        case 0x15AD: return "VMware";
        case 0x1234: return "QEMU";
        default: return "unknown";
    }
}

}  // namespace

void detect_cpu(CpuInfo& out) {
    out.model = read_registry_string(
        HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString");
    out.vendor = read_registry_string(
        HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"VendorIdentifier");

    SYSTEM_INFO si {};
    ::GetNativeSystemInfo(&si);
    out.logical_cores = si.dwNumberOfProcessors;

    // Physical core count via the documented topology API.
    DWORD length = 0;
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length > 0) {
        std::vector<char> buffer(length);
        if (::GetLogicalProcessorInformationEx(
                RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
                &length)) {
            DWORD offset = 0;
            u32 physical = 0;
            while (offset < length) {
                auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                    buffer.data() + offset);
                if (entry->Relationship == RelationProcessorCore) ++physical;
                offset += entry->Size;
            }
            out.physical_cores = physical;
        }
    }
    if (out.physical_cores == 0) out.physical_cores = out.logical_cores;

    // Hybrid topology: RelationProcessorCore entries carry an EfficiencyClass.
    // A class of 0 denotes the highest performance tier on Windows.
    if (length > 0) {
        std::vector<char> buffer(length);
        if (::GetLogicalProcessorInformationEx(
                RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
                &length)) {
            DWORD offset = 0;
            while (offset < length) {
                auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                    buffer.data() + offset);
                if (entry->Relationship == RelationProcessorCore) {
                    if (entry->Processor.EfficiencyClass == 0) out.performance_cores++;
                    else out.efficiency_cores++;
                }
                offset += entry->Size;
            }
            if (out.performance_cores == out.physical_cores) {
                // Homogeneous CPU: do not report a hybrid split.
                out.performance_cores = 0;
                out.efficiency_cores = 0;
            }
        }
    }

    // ISA features via CPUID. Both MSVC (__cpuidex) and GCC (__cpuid_count) are
    // supported, so this is not gated on the compiler any more: a mingw build
    // used to report no AVX support at all because the check only compiled
    // under MSVC.
    int max_leaf = 0;
    {
        int regs[4];
        cpuid_regs(0, 0, regs);
        max_leaf = regs[0];
    }

    if (max_leaf >= 1) {
        int regs[4];
        cpuid_regs(1, 0, regs);
        if (regs[3] & (1 << 26)) out.isa_features.emplace_back("sse2");
        if (regs[2] & (1 << 19)) out.isa_features.emplace_back("sse4_2");
        // AVX is only real when the CPU reports it and the OS saves YMM state.
        if ((regs[2] & (1 << 28)) != 0 && os_supports_ymm()) {
            out.isa_features.emplace_back("avx");
        }
    }
    if (max_leaf >= 7) {
        int regs[4];
        cpuid_regs(7, 0, regs);
        const bool ymm_ok = os_supports_ymm();
        if ((regs[1] & (1 << 5)) != 0 && ymm_ok) {
            out.supports_avx2 = true;
            out.isa_features.emplace_back("avx2");
        }
        if ((regs[1] & (1 << 3)) != 0) out.isa_features.emplace_back("bmi1");
        if ((regs[1] & (1 << 8)) != 0) out.isa_features.emplace_back("bmi2");
        if ((regs[1] & (1 << 12)) != 0 && ymm_ok) out.isa_features.emplace_back("fma");
        // AVX-512 additionally needs the opmask/ZMM state enabled (XCR0 bits 5..7).
        if ((regs[1] & (1 << 16)) != 0 && ymm_ok) {
#if defined(_MSC_VER)
            const unsigned long long xcr0 = _xgetbv(0);
#else
            unsigned int eax = 0, edx = 0;
            __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
            const unsigned long long xcr0 =
                (static_cast<unsigned long long>(edx) << 32) | eax;
#endif
            if ((xcr0 & 0xE6) == 0xE6) {
                out.supports_avx512 = true;
                out.isa_features.emplace_back("avx512f");
            }
        }
    }

#if defined(_M_ARM64) || defined(__aarch64__)
    out.supports_neon = true;
    out.isa_features.emplace_back("neon");
#endif
}

void detect_memory(MemoryInfo& out) {
    MEMORYSTATUSEX status {};
    status.dwLength = sizeof(status);
    if (::GlobalMemoryStatusEx(&status)) {
        out.total_bytes = status.ullTotalPhys;
        out.available_bytes = status.ullAvailPhys;
        out.swap_total_bytes = status.ullTotalPageFile > status.ullTotalPhys
                                   ? status.ullTotalPageFile - status.ullTotalPhys
                                   : 0;
        out.has_swap = out.swap_total_bytes > 0;
    }
    SYSTEM_INFO si {};
    ::GetSystemInfo(&si);
    out.page_size_bytes = si.dwPageSize;
}

void detect_os(OperatingSystemInfo& out) {
    out.name = "Windows";
    out.kernel = read_registry_string(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        L"CurrentBuildNumber");
    out.version = read_registry_string(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        L"ProductName");

    SYSTEM_INFO si {};
    ::GetNativeSystemInfo(&si);
    out.is_64_bit = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
                    si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64;
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: out.architecture = "x86_64"; break;
        case PROCESSOR_ARCHITECTURE_ARM64: out.architecture = "aarch64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: out.architecture = "i386"; break;
        default: out.architecture = "unknown";
    }
}

std::vector<GpuDeviceInfo> detect_system_gpus() {
    std::vector<GpuDeviceInfo> gpus;

    // IDXGIFactory1 / IDXGIAdapter1 / DXGI_ADAPTER_DESC1: the Desc1 form is what
    // carries DXGI_ADAPTER_FLAG_SOFTWARE, which the classification below needs.
    // The plain IDXGIAdapter has no GetDesc1 and does not compile.
    IDXGIFactory1* factory = nullptr;
    if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(&factory))) ||
        factory == nullptr) {
        return gpus;
    }

    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (adapter == nullptr) break;

        DXGI_ADAPTER_DESC1 desc {};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            GpuDeviceInfo info;
            info.name = wide_to_utf8(desc.Description);
            info.vendor = vendor_name_from_id(desc.VendorId);
            info.vendor_id = desc.VendorId;
            info.device_id = desc.DeviceId;
            info.dedicated_vram_bytes = desc.DedicatedVideoMemory;
            info.shared_memory_bytes = desc.SharedSystemMemory;
            info.is_removable = (desc.Flags & DXGI_ADAPTER_FLAG_REMOTE) != 0;
            info.kind = classify_dxgi_adapter(desc);
            // The Windows backend talks to the adapter through Direct3D 11, so
            // that is the API the device is reported under.
            info.api = GraphicsApi::Direct3D11;
            // A UMA (integrated/virtual) adapter shares system memory with the
            // CPU; a discrete adapter with its own VRAM does not. DXGI reports
            // this through the adapter flags and the memory split.
            info.unified_memory = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                                  info.kind == GpuKind::Integrated &&
                                  desc.DedicatedVideoMemory == 0;
            gpus.push_back(std::move(info));
        }
        adapter->Release();
    }
    factory->Release();
    return gpus;
}

u32 logical_processor_count() {
    SYSTEM_INFO si {};
    ::GetSystemInfo(&si);
    return si.dwNumberOfProcessors;
}

}  // namespace lx::hardware::platform

#endif  // _WIN32
