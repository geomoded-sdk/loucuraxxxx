// Registers the CPU backend with the global registry.
//
// Registration is an explicit function rather than a file-scope initialiser.
// A static-library object that only self-registers is never referenced, so the
// linker drops it and the registration silently never happens; calling this
// function from the registry guarantees the object is kept.
#include "backends/backend_registry.hpp"
#include "backends/cpu/cpu_backend.hpp"

namespace lx::backends {

void register_cpu_backend() {
    static bool done = false;
    if (done) return;
    done = true;

    BackendRegistration registration;
    registration.kind = BackendKind::Cpu;
    registration.name = "cpu";
    registration.probe = &CpuBackend::probe;
    registration.factory = &CpuBackend::create;
    registration.maturity = Maturity::Implemented;
    register_backend(registration);
}

}  // namespace lx::backends
