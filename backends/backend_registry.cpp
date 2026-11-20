#include "backends/backend_registry.hpp"

#include <algorithm>
#include <mutex>

#include "core/log.hpp"

namespace lx::backends {

namespace {
std::mutex g_mutex;
std::vector<BackendRegistration>& registry() {
    static std::vector<BackendRegistration> instance;
    return instance;
}
}  // namespace

std::optional<BackendKind> backend_kind_from_string(const std::string& name) {
    if (name == "cpu") return BackendKind::Cpu;
    if (name == "vulkan") return BackendKind::Vulkan;
    if (name == "d3d11") return BackendKind::Direct3D11;
    if (name == "opengl") return BackendKind::OpenGL;
    if (name == "metal") return BackendKind::Metal;
    return std::nullopt;
}

void register_backend(const BackendRegistration& registration) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& reg = registry();
    for (auto& existing : reg) {
        if (existing.kind == registration.kind) {
            existing = registration;  // last registration wins
            return;
        }
    }
    reg.push_back(registration);
}

void ensure_builtin_backends_registered() {
    static bool done = false;
    if (done) return;
    done = true;
    // Order matters for the "auto" selection ladder: probe_available_backends()
    // keeps hardware backends ahead of the CPU fallback, and within the
    // hardware set registration order is the preference order. So register in
    // descending preference: Vulkan when present, then Direct3D 11 on Windows,
    // then OpenGL 3.3 as the universal fallback.
#if defined(LX_HAS_VULKAN)
    register_vulkan_backend();
#endif
#if defined(LX_HAS_METAL)
    register_metal_backend();
#endif
#if defined(LX_HAS_D3D11)
    register_d3d11_backend();
#endif
#if defined(LX_HAS_OPENGL)
    register_opengl_backend();
#endif
    register_cpu_backend();
}

const std::vector<BackendRegistration>& registered_backends() {
    ensure_builtin_backends_registered();
    return registry();
}

const BackendRegistration* find_backend(BackendKind kind) {
    for (const auto& reg : registered_backends()) {
        if (reg.kind == kind) return &reg;
    }
    return nullptr;
}

std::vector<AvailableBackend> probe_available_backends() {
    // The preference ladder, highest first: a real, broadly-capable GPU API is
    // preferred over the CPU reference path. Vulkan is opt-in and rarely
    // present; Direct3D 11 is the Windows real-GPU path; OpenGL 3.3 core is the
    // universal fallback that works almost everywhere; the CPU is last. The rank
    // is explicit so the order does not depend on registration order.
    auto rank = [](BackendKind kind) {
        switch (kind) {
            case BackendKind::Vulkan: return 0;
            case BackendKind::Metal: return 1;
            case BackendKind::Direct3D11: return 2;
            case BackendKind::OpenGL: return 3;
            case BackendKind::Cpu: return 4;
        }
        return 5;
    };

    std::vector<AvailableBackend> available;
    for (const auto& reg : registered_backends()) {
        BackendProbeResult probe = reg.probe ? reg.probe() : BackendProbeResult{};
        if (!probe.available) {
            LX_DEBUG << "backend '" << reg.name << "' unavailable: " << probe.reason;
            continue;
        }
        AvailableBackend entry;
        entry.kind = reg.kind;
        entry.name = reg.name;
        entry.device_name = probe.device_name;
        entry.reason = probe.reason;
        entry.maturity = reg.maturity;
        entry.is_hardware = probe.is_hardware;
        available.push_back(std::move(entry));
    }
    // Order by capability tier first, then by the preference ladder. The tiers
    // are: real GPU hardware, then the CPU reference path, then software
    // rasterizers. Ranking by API alone would put a software Vulkan or OpenGL
    // driver ahead of the CPU, which is strictly worse: a software rasterizer
    // does the same work on the same cores with driver overhead on top, so
    // selecting it as the default would look like acceleration while being
    // slower than the plain CPU path.
    auto tier = [](const AvailableBackend& b) {
        if (b.is_hardware) return 0;
        if (b.kind == BackendKind::Cpu) return 1;
        return 2;
    };
    std::stable_sort(available.begin(), available.end(),
                     [&](const AvailableBackend& a, const AvailableBackend& b) {
                         const int ta = tier(a), tb = tier(b);
                         if (ta != tb) return ta < tb;
                         return rank(a.kind) < rank(b.kind);
                     });
    return available;
}

}  // namespace lx::backends
