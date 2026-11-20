// Registers the Direct3D 11 backend with the global registry (Windows only).
//
// Registration is an explicit function rather than a file-scope initialiser: a
// static-library object that only self-registers is never referenced, so the
// linker drops it and the registration silently never happens.
#include "backends/backend_registry.hpp"
#include "backends/d3d11/d3d11_backend.hpp"

namespace lx::backends {

void register_d3d11_backend() {
    static bool done = false;
    if (done) return;
    done = true;

    BackendRegistration registration;
    registration.kind = BackendKind::Direct3D11;
    registration.name = "d3d11";
    registration.probe = &D3D11Backend::probe;
    registration.factory = &D3D11Backend::create;
    // Experimental: built and tested under Wine on real D3D11, but new in 0.0.2
    // and not yet exercised across GPU vendors on Windows itself.
    registration.maturity = Maturity::Experimental;
    register_backend(registration);
}

}  // namespace lx::backends
