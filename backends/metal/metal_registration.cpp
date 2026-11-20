// Registers the Metal backend with the global registry.
//
// Same rationale as the CPU backend: an explicit function keeps the object file
// in the static library and makes registration order visible, instead of relying
// on a file-scope initialiser the linker is free to drop.
#include "backends/backend_registry.hpp"
#include "backends/metal/metal_backend.hpp"

namespace lx::backends {

void register_metal_backend() {
    static bool done = false;
    if (done) return;
    done = true;

    BackendRegistration registration;
    registration.kind = BackendKind::Metal;
    registration.name = "metal";
    registration.probe = &MetalBackend::probe;
    registration.factory = &MetalBackend::create;
    registration.maturity = Maturity::Experimental;
    register_backend(registration);
}

}  // namespace lx::backends
