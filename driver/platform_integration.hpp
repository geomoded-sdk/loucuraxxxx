// LoucuraXXXX GPU - OS integration interface for the driver.
//
// A platform integration connects the virtual GPU to the host operating system.
// Some connections require kernel privileges (a real display adapter, a DRM
// device node). Those are attempted when permitted and otherwise reported as
// unavailable; the driver always keeps a user-space path that works without
// them. No integration pretends to have succeeded.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"
#include "hardware/hardware_info.hpp"

namespace lx::driver {

// What a platform integration achieved. Reported to callers so the maturity of
// each connection is visible.
enum class IntegrationLevel {
    Unavailable,     // the OS feature is absent or refused
    UserSpaceOnly,   // works without kernel privileges (the safe default)
    KernelAssisted,  // a privileged connection was established
};

inline const char* to_string(IntegrationLevel level) {
    switch (level) {
        case IntegrationLevel::Unavailable: return "unavailable";
        case IntegrationLevel::UserSpaceOnly: return "user-space";
        case IntegrationLevel::KernelAssisted: return "kernel-assisted";
    }
    return "unknown";
}

struct IntegrationStatus {
    std::string platform;               // "linux", "windows", "macos"
    IntegrationLevel level = IntegrationLevel::Unavailable;
    std::string api;                    // "DRM/Mesa", "WDDM", "Metal"
    std::vector<std::string> notes;     // human-readable detail
    // Both flags describe what this integration *achieves*, not what the
    // platform would permit. They stay false until a code path implements the
    // capability, so a caller reading them is never told a feature exists when it
    // does not.
    bool can_expose_adapter = false;    // a virtual adapter is advertised
    bool can_share_surfaces = false;    // zero-copy surface sharing is implemented
};

// The interface each platform implements.
class PlatformIntegration {
public:
    virtual ~PlatformIntegration() = default;
    virtual const char* name() const = 0;

    // Probes what this platform permits without changing system state.
    virtual IntegrationStatus probe() const = 0;

    // Attempts to establish the integration. Must not install kernel drivers
    // or require root; it uses only interfaces available to a normal process.
    virtual Status connect(const hardware::HardwareInfo& hardware) = 0;

    // Releases anything connect() acquired.
    virtual void disconnect() = 0;
};

// Creates the integration for the current platform, or nullptr on platforms
// without one. The returned object is owned by the caller.
std::unique_ptr<PlatformIntegration> create_platform_integration();

}  // namespace lx::driver
