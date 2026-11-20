// Registers the OpenGL 3.3 core backend with the global registry.
//
// Registration is an explicit function rather than a file-scope initialiser: a
// static-library object that only self-registers is never referenced, so the
// linker drops it and the registration silently never happens.
#include "backends/backend_registry.hpp"
#include "backends/opengl/gl_backend.hpp"

namespace lx::backends {

void register_opengl_backend() {
    static bool done = false;
    if (done) return;
    done = true;

    BackendRegistration registration;
    registration.kind = BackendKind::OpenGL;
    registration.name = "opengl";
    registration.probe = &GlBackend::probe;
    registration.factory = &GlBackend::create;
    // Experimental: the GL path is built and tested on real drivers, but it is
    // new in 0.0.2 and not yet exercised across the full range of GPU vendors.
    registration.maturity = Maturity::Experimental;
    register_backend(registration);
}

}  // namespace lx::backends
