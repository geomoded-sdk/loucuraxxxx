// LoucuraXXXX GPU - backend registration table.
//
// Backends self-register at load time so the core never needs to know which
// ones were compiled in. Each backend exposes a probe that reports whether it
// is usable on the current host without creating a device.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "backends/backend.hpp"
#include "core/status.hpp"

namespace lx::backends {

// Maps a configuration backend name ("vulkan", "d3d11", "opengl", "metal",
// "cpu") to its kind. Returns nullopt for an unknown name, which callers use to
// reject a request rather than defer the failure to device creation.
std::optional<BackendKind> backend_kind_from_string(const std::string& name);

// Probes the host for whether a backend could be created. Cheap: must not
// create a device or allocate significant memory.
struct BackendProbeResult {
    bool available = false;
    std::string reason;      // why it is unavailable, when it is not
    std::string device_name; // best device found, when available

    // False when the API is present only through a software rasterizer, so the
    // caller can say "usable but not accelerated" instead of implying hardware.
    bool is_hardware = false;
};

using BackendProbe = BackendProbeResult (*)();
using BackendFactory = Result<std::unique_ptr<Backend>> (*)(const BackendCreateInfo&);

struct BackendRegistration {
    BackendKind kind;
    const char* name;
    BackendProbe probe;
    BackendFactory factory;
    Maturity maturity;
};

// Registers a backend. Called once by ensure_builtin_backends_registered().
void register_backend(const BackendRegistration& registration);

// Registers the backends compiled into this build, in a fixed order, exactly
// once. Called lazily on first registry access.
void ensure_builtin_backends_registered();

// Explicit registration entry points.
//
// A backend that only registers itself from a file-scope initialiser has no
// symbol the rest of the program references, so the linker may drop its object
// file out of the static library and the registration silently never runs.
// Each backend therefore exposes a registration function that the registry
// calls once, which both keeps the object and makes the ordering explicit.
void register_cpu_backend();
#if defined(LX_HAS_OPENGL)
void register_opengl_backend();
#endif
#if defined(LX_HAS_D3D11)
void register_d3d11_backend();
#endif
#if defined(LX_HAS_VULKAN)
void register_vulkan_backend();
#endif
#if defined(LX_HAS_METAL)
void register_metal_backend();
#endif

// Returns every registered backend, in registration order.
const std::vector<BackendRegistration>& registered_backends();

// Finds a registration by kind. Returns nullptr when not compiled in.
const BackendRegistration* find_backend(BackendKind kind);

// A backend that probed as usable on this host, with what the probe learned.
// `is_hardware` is false for the CPU path and for an API that is only present
// through a software rasterizer, which is the difference between "this machine
// can run Vulkan" and "this machine has a GPU that runs Vulkan".
struct AvailableBackend {
    BackendKind kind = BackendKind::Cpu;
    std::string name;
    std::string device_name;
    std::string reason;
    Maturity maturity = Maturity::Implemented;
    bool is_hardware = false;
};

// Probes all registered backends and returns the ones that are usable, ordered
// so that accelerated paths come first: real hardware before software
// rasterizers, and within each group the preference ladder (Vulkan, Metal,
// Direct3D 11, OpenGL) before the CPU fallback.
std::vector<AvailableBackend> probe_available_backends();

}  // namespace lx::backends
