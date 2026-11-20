// LoucuraXXXX GPU - macOS integration.
//
// macOS deliberately prevents third-party kernel graphics drivers; all GPU
// access goes through Metal. This integration therefore never attempts to
// install a driver. It confirms that Metal is available and reports
// UserSpaceOnly, using Metal as the only sanctioned path.
//
// Marked experimental: written against documented Metal APIs but not exercised
// on a macOS host in this repository's CI.
#include "driver/platform_integration.hpp"

#if defined(__APPLE__)

#import <Metal/Metal.h>

#include "core/log.hpp"

namespace lx::driver {

class MacOsIntegration : public PlatformIntegration {
public:
    const char* name() const override { return "macos"; }

    IntegrationStatus probe() const override {
        IntegrationStatus status;
        status.platform = "macos";
        status.api = "Metal";

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device != nil) {
            status.level = IntegrationLevel::UserSpaceOnly;
            // Metal would allow shared storage, but no IOSurface/Metal shared
            // texture path exists in this code yet, so the flag stays false.
            status.notes.emplace_back(std::string("Metal device available: ") +
                                      [device.name UTF8String]);
            status.notes.emplace_back(
                "zero-copy surface sharing is not implemented; frames are copied");
        } else {
            status.level = IntegrationLevel::Unavailable;
            status.notes.emplace_back("no Metal device available");
        }
        status.can_expose_adapter = false;
        status.notes.emplace_back(
            "macOS does not permit third-party kernel graphics drivers; Metal is "
            "the only supported path and no driver installation is attempted");
        return status;
    }

    Status connect(const hardware::HardwareInfo&) override {
        status_ = probe();
        if (status_.level == IntegrationLevel::Unavailable) {
            return Status::error(ErrorCode::NotSupported, "no Metal device available");
        }
        connected_ = true;
        LX_INFO << "macOS integration connected (user-space, Metal)";
        return Status::success();
    }

    void disconnect() override { connected_ = false; }

private:
    IntegrationStatus status_{};
    bool connected_ = false;
};

std::unique_ptr<PlatformIntegration> create_platform_integration() {
    return std::unique_ptr<PlatformIntegration>(new MacOsIntegration());
}

}  // namespace lx::driver

#endif  // __APPLE__
