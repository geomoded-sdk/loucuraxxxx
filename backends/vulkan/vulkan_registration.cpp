// Registers the Vulkan backend with the global registry (opt-in build).
#include "backends/backend_registry.hpp"
#include "backends/vulkan/vulkan_backend.hpp"

namespace lx::backends {

void register_vulkan_backend() {
    static bool done = false;
    if (done) return;
    done = true;

    BackendRegistration registration;
    registration.kind = BackendKind::Vulkan;
    registration.name = "vulkan";
    registration.probe = &VulkanBackend::probe;
    registration.factory = &VulkanBackend::create;
    // Experimental: the backend is real and tested against a Vulkan device, but
    // it is opt-in and new in 0.0.2, so it is not yet exercised across the range
    // of Vulkan drivers.
    registration.maturity = Maturity::Experimental;
    register_backend(registration);
}

}  // namespace lx::backends
