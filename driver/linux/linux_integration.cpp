// LoucuraXXXX GPU - Linux integration.
//
// Linux exposes graphics devices through DRM at /dev/dri. A normal process can
// open a render node and query it; it cannot create a new display adapter
// without a kernel module. This integration therefore reports UserSpaceOnly and
// explains exactly why, rather than claiming a virtual adapter it cannot make.
// It also records whether Mesa and Vulkan are present so the renderer knows
// which backends are viable.
#include "driver/platform_integration.hpp"

#if defined(__linux__)

#include <dirent.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "core/log.hpp"

namespace lx::driver {

namespace {

bool path_exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

// True when a render node (a DRM device without display master control) exists.
bool has_render_node() {
    DIR* dir = ::opendir("/dev/dri");
    if (!dir) return false;
    bool found = false;
    while (dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.rfind("renderD", 0) == 0) {
            found = true;
            break;
        }
    }
    ::closedir(dir);
    return found;
}

bool vulkan_loader_present() {
    return path_exists("/usr/lib/x86_64-linux-gnu/libvulkan.so.1") ||
           path_exists("/usr/lib/libvulkan.so.1") ||
           path_exists("/lib/x86_64-linux-gnu/libvulkan.so.1");
}

bool mesa_present() {
    return path_exists("/usr/lib/x86_64-linux-gnu/dri") ||
           path_exists("/usr/lib/dri");
}

}  // namespace

class LinuxIntegration : public PlatformIntegration {
public:
    const char* name() const override { return "linux"; }

    IntegrationStatus probe() const override {
        IntegrationStatus status;
        status.platform = "linux";
        status.api = "DRM / Mesa";

        if (has_render_node()) {
            status.level = IntegrationLevel::UserSpaceOnly;
            status.notes.emplace_back(
                "DRM render node present; the physical GPU can be used through "
                "its normal driver");
            // A render node lets this process submit work to the physical GPU, but
            // no zero-copy surface path is implemented yet, so the capability is
            // reported as absent rather than assumed from the node's presence.
            status.notes.emplace_back(
                "zero-copy surface sharing is not implemented; frames are copied");
        } else {
            status.level = IntegrationLevel::Unavailable;
            status.notes.emplace_back(
                "no /dev/dri/renderD* node; no DRM device is exposed to this process");
        }
        status.notes.emplace_back(mesa_present() ? "Mesa userspace drivers present"
                                                 : "Mesa userspace drivers not found");
        status.notes.emplace_back(vulkan_loader_present()
                                      ? "Vulkan loader present"
                                      : "Vulkan loader not found");
        // Exposing a *new* adapter requires a kernel module, which this process
        // will not install.
        status.can_expose_adapter = false;
        status.notes.emplace_back(
            "advertising a new adapter requires a kernel module and is out of "
            "scope for a user-space process");
        return status;
    }

    Status connect(const hardware::HardwareInfo&) override {
        status_ = probe();
        if (status_.level == IntegrationLevel::Unavailable) {
            return Status::error(ErrorCode::NotSupported,
                                 "Linux DRM integration unavailable: no render node");
        }
        connected_ = true;
        LX_INFO << "Linux integration connected (user-space, DRM)";
        return Status::success();
    }

    void disconnect() override { connected_ = false; }

private:
    IntegrationStatus status_{};
    bool connected_ = false;
};

std::unique_ptr<PlatformIntegration> create_platform_integration() {
    return std::unique_ptr<PlatformIntegration>(new LinuxIntegration());
}

}  // namespace lx::driver

#endif  // __linux__
