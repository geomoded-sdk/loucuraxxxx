#include "backends/backend.hpp"

#include "backends/backend_registry.hpp"
#include "core/log.hpp"

namespace lx::backends {

Result<std::unique_ptr<Backend>> create_backend(const BackendCreateInfo& info) {
    const BackendRegistration* registration = find_backend(info.preferred);
    if (registration == nullptr) {
        return Status::error(ErrorCode::BackendUnavailable,
                             std::string("backend '") + to_string(info.preferred) +
                                 "' is not compiled into this build");
    }
    if (registration->probe) {
        const BackendProbeResult probe = registration->probe();
        if (!probe.available) {
            return Status::error(ErrorCode::BackendUnavailable,
                                 std::string("backend '") + registration->name +
                                     "' is not usable: " + probe.reason);
        }
    }
    if (registration->factory == nullptr) {
        return Status::error(ErrorCode::BackendUnavailable,
                             std::string("backend '") + registration->name +
                                 "' has no factory");
    }
    return registration->factory(info);
}

std::vector<BackendKind> available_backend_kinds() {
    std::vector<BackendKind> kinds;
    for (const auto& reg : registered_backends()) kinds.push_back(reg.kind);
    return kinds;
}

}  // namespace lx::backends
