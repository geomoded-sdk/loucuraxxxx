// LoucuraXXXX GPU - Linux hardware detection via /proc and /sys.
#include "hardware/platform.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <vector>

namespace lx::hardware::platform {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    return s.substr(start);
}

std::string read_first_line(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return trim(line);
}

u64 read_u64(const std::string& path) {
    std::ifstream in(path);
    u64 value = 0;
    in >> value;
    return value;
}

// Parses the value of a "key : value" line from /proc/cpuinfo. The separator is
// the first colon, and the key is trimmed: /proc/cpuinfo pads keys with spaces
// so the colon is not at a fixed offset.
std::string cpuinfo_field(const std::string& text, const std::string& key) {
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (trim(line.substr(0, colon)) == key) {
            return trim(line.substr(colon + 1));
        }
    }
    return {};
}

u32 count_unique_physical_cores(const std::string& cpuinfo) {
    std::istringstream stream(cpuinfo);
    std::string line;
    std::vector<std::pair<int, int>> ids;
    int phys = -1, core = -1;
    auto flush = [&]() {
        if (phys >= 0 && core >= 0) {
            auto key = std::make_pair(phys, core);
            if (std::find(ids.begin(), ids.end(), key) == ids.end()) ids.push_back(key);
        }
        phys = core = -1;
    };
    while (std::getline(stream, line)) {
        if (line.empty()) { flush(); continue; }
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trim(line.substr(0, colon));
        const std::string value = trim(line.substr(colon + 1));
        if (key == "physical id") phys = std::atoi(value.c_str());
        else if (key == "core id") core = std::atoi(value.c_str());
    }
    flush();
    return static_cast<u32>(ids.size());
}

void collect_isa_features(CpuInfo& cpu, const std::string& cpuinfo) {
    static const char* features[] = {"sse2", "sse4_2", "avx",  "avx2", "avx512f",
                                     "fma",  "neon",   "asimd"};
    for (const char* f : features) {
        if (cpuinfo.find(f) != std::string::npos) cpu.isa_features.emplace_back(f);
    }
    auto has = [&](const char* f) {
        return std::find(cpu.isa_features.begin(), cpu.isa_features.end(), f) !=
               cpu.isa_features.end();
    };
    cpu.supports_avx2 = has("avx2");
    cpu.supports_avx512 = has("avx512f");
    cpu.supports_neon = has("neon") || has("asimd");
}

// Maps a vendor id (from /sys/class/drm/cardN/device/vendor) to a name.
std::string pci_vendor_name(u32 vendor_id) {
    switch (vendor_id) {
        case 0x10DE: return "NVIDIA";
        case 0x1002:
        case 0x1022: return "AMD";
        case 0x8086: return "Intel";
        case 0x1AF4: return "Red Hat (virtio)";
        case 0x15AD: return "VMware";
        case 0x1234: return "QEMU";
        case 0x106B: return "Apple";
        default: return "unknown";
    }
}

GpuKind classify_linux_gpu(const std::string& driver, u32 vendor_id,
                           bool has_dedicated_vram) {
    if (driver.find("virtio") != std::string::npos || vendor_id == 0x1AF4 ||
        vendor_id == 0x15AD || vendor_id == 0x1234) {
        return GpuKind::Virtual;
    }
    if (driver.find("simpledrm") != std::string::npos ||
        driver.find("vgem") != std::string::npos) {
        return GpuKind::Software;
    }
    if (has_dedicated_vram) return GpuKind::Discrete;
    return GpuKind::Integrated;
}

}  // namespace

void detect_cpu(CpuInfo& out) {
    std::ifstream in("/proc/cpuinfo");
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    out.model = cpuinfo_field(text, "model name");
    if (out.model.empty()) out.model = cpuinfo_field(text, "Processor");
    out.vendor = cpuinfo_field(text, "vendor_id");
    if (out.vendor.empty()) out.vendor = cpuinfo_field(text, "CPU implementer");

    out.logical_cores = logical_processor_count();
    out.physical_cores = count_unique_physical_cores(text);
    if (out.physical_cores == 0) out.physical_cores = out.logical_cores;

    const u64 l2 = read_u64("/sys/devices/system/cpu/cpu0/cache/index2/size");
    const u64 l3 = read_u64("/sys/devices/system/cpu/cpu0/cache/index3/size");
    out.l2_cache_bytes = l2 * 1024ull;  // sysfs reports KiB
    out.l3_cache_bytes = l3 * 1024ull;

    const u64 khz = read_u64("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    if (khz > 0) out.max_frequency_hz = khz * 1000ull;

    collect_isa_features(out, text);

    // Hybrid topology: Linux exposes per-core capacity when available. We only
    // report big/little counts when the kernel actually provides the data.
    u32 with_capacity = 0;
    for (u32 i = 0; i < out.logical_cores; ++i) {
        const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) +
                                 "/cpu_capacity";
        if (read_u64(path) > 0) ++with_capacity;
    }
    if (with_capacity == out.logical_cores && with_capacity > 0) {
        std::vector<u64> caps;
        for (u32 i = 0; i < out.logical_cores; ++i) {
            caps.push_back(read_u64("/sys/devices/system/cpu/cpu" + std::to_string(i) +
                                    "/cpu_capacity"));
        }
        const u64 max_cap = *std::max_element(caps.begin(), caps.end());
        const u64 min_cap = *std::min_element(caps.begin(), caps.end());
        if (max_cap != min_cap) {
            for (u64 c : caps) {
                if (c == max_cap) out.performance_cores++;
                else out.efficiency_cores++;
            }
        }
    }
}

void detect_memory(MemoryInfo& out) {
    struct sysinfo si {};
    if (::sysinfo(&si) == 0) {
        out.total_bytes = si.totalram * si.mem_unit;
        // freeram excludes reclaimable cache; available = free + buffers + cache.
        out.available_bytes =
            (static_cast<u64>(si.freeram) + si.bufferram + si.sharedram) * si.mem_unit;
        out.swap_total_bytes = si.totalswap * si.mem_unit;
        out.has_swap = out.swap_total_bytes > 0;
    }
    out.page_size_bytes = static_cast<u64>(::sysconf(_SC_PAGESIZE));

    // Prefer MemAvailable from /proc/meminfo when present: it is the kernel's
    // own estimate of memory available without swapping.
    std::ifstream in("/proc/meminfo");
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 14, "MemAvailable: ") == 0) {
            const u64 kb = std::strtoull(line.c_str() + 14, nullptr, 10);
            out.available_bytes = kb * 1024ull;
            break;
        }
    }
}

void detect_os(OperatingSystemInfo& out) {
    out.name = "Linux";
    out.kernel = read_first_line("/proc/sys/kernel/osrelease");

    std::ifstream in("/etc/os-release");
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 13, "PRETTY_NAME=\"") == 0) {
            out.version = line.substr(13, line.size() - 14);
            break;
        }
    }
    if (out.version.empty()) out.version = read_first_line("/etc/issue");

#if defined(__x86_64__)
    out.architecture = "x86_64";
#elif defined(__aarch64__)
    out.architecture = "aarch64";
#elif defined(__i386__)
    out.architecture = "i386";
#else
    out.architecture = "unknown";
#endif
    out.is_64_bit = sizeof(void*) == 8;
}

std::vector<GpuDeviceInfo> detect_system_gpus() {
    std::vector<GpuDeviceInfo> gpus;

    DIR* dir = ::opendir("/sys/class/drm");
    if (!dir) return gpus;

    std::vector<std::string> cards;
    while (dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        // Accept cardN but not cardN-DP-1 connectors.
        if (name.rfind("card", 0) == 0 && name.find('-') == std::string::npos &&
            name.size() > 4) {
            cards.push_back(name);
        }
    }
    ::closedir(dir);
    std::sort(cards.begin(), cards.end());

    for (const auto& card : cards) {
        const std::string base = "/sys/class/drm/" + card;
        const std::string device = base + "/device";

        GpuDeviceInfo info;
        std::ifstream vendor_file(device + "/vendor");
        if (vendor_file) {
            std::string v;
            vendor_file >> v;
            info.vendor_id = static_cast<u32>(std::strtoul(v.c_str(), nullptr, 16));
            info.vendor = pci_vendor_name(info.vendor_id);
        }
        std::ifstream device_file(device + "/device");
        if (device_file) {
            std::string d;
            device_file >> d;
            info.device_id = static_cast<u32>(std::strtoul(d.c_str(), nullptr, 16));
        }

        // Resolve the bound kernel driver from the sysfs symlink.
        std::string driver;
        const std::string driver_link = device + "/driver";
        char resolved[512];
        const ssize_t n = ::readlink(driver_link.c_str(), resolved, sizeof(resolved) - 1);
        if (n > 0) {
            resolved[n] = '\0';
            const std::string path(resolved);
            const auto slash = path.find_last_of('/');
            driver = slash == std::string::npos ? path : path.substr(slash + 1);
        }
        info.driver = driver;

        // Dedicated VRAM is exposed per-memory-region by amdgpu/nouveau. When
        // absent we leave it at zero rather than guessing.
        const std::string vram = device + "/mem_info_vram_total";
        if (std::ifstream vr(vram); vr) {
            u64 bytes = 0;
            vr >> bytes;
            info.dedicated_vram_bytes = bytes;
        }

        info.name = card + (driver.empty() ? "" : " (" + driver + ")");
        info.kind = classify_linux_gpu(driver, info.vendor_id,
                                       info.dedicated_vram_bytes > 0);
        info.api = GraphicsApi::None;

        // Unified memory: an integrated/virtual device with no separate VRAM
        // region reads and writes the same pool the CPU uses. A device with its
        // own VRAM is discrete. This is the same distinction the Apple path
        // makes, expressed through Linux's sysfs.
        info.unified_memory =
            (info.kind == GpuKind::Integrated || info.kind == GpuKind::Virtual) &&
            info.dedicated_vram_bytes == 0;
        if (info.unified_memory) {
            info.shared_memory_bytes = 0;  // bounded only by the system budget
        }

        gpus.push_back(std::move(info));
    }
    return gpus;
}

u32 logical_processor_count() {
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<u32>(n) : 0;
}

}  // namespace lx::hardware::platform
