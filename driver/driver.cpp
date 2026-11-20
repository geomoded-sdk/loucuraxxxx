#include "driver/driver.hpp"

#include "backends/backend_registry.hpp"
#include "core/log.hpp"
#include "core/version.hpp"
#include "memory/memory_budget.hpp"

namespace lx::driver {

namespace {

backends::BackendKind backend_kind_from_name(const std::string& name) {
    if (name == "vulkan") return backends::BackendKind::Vulkan;
    if (name == "d3d11" || name == "dx11" || name == "direct3d11") {
        return backends::BackendKind::Direct3D11;
    }
    if (name == "opengl" || name == "gl" || name == "gl3") {
        return backends::BackendKind::OpenGL;
    }
    if (name == "metal") return backends::BackendKind::Metal;
    // "cpu" and anything unrecognised select the CPU reference backend, which is
    // the only one that is always present.
    return backends::BackendKind::Cpu;
}

}  // namespace

Driver::~Driver() { shutdown(); }

Status Driver::initialize(const configurator::Config& config) {
    if (initialized_) {
        return Status::error(ErrorCode::ResourceBusy, "driver already initialized");
    }
    config_ = config;
    // A previous cycle's status must not leak into this one. Without this, every
    // warning from an earlier initialize()/shutdown() pair was still in the list,
    // so a caller that reinitialized saw the same warning twice and could not tell
    // which session it belonged to.
    status_ = DriverStatus{};

    // 1. Detect the host. Everything downstream depends on these real values.
    auto hardware = hardware::detect_hardware();
    if (!hardware.ok()) return hardware.status();
    status_.hardware = hardware.value();

    // 2. Compute and apply the safe reservation from the request.
    memory::MemoryPolicy policy;
    status_.reservation = policy.plan(status_.hardware.memory, config.reserved_ram_bytes);
    budget_.configure(status_.reservation);
    if (status_.reservation.clamped) {
        status_.warnings.push_back(
            "RAM reservation clamped from " +
            std::to_string(status_.reservation.requested_bytes / kMiB) + " MiB to " +
            std::to_string(status_.reservation.granted_bytes / kMiB) + " MiB (" +
            status_.reservation.reason + ")");
        LX_WARN << status_.warnings.back();
    }

    // 3. Select and create a backend.
    LX_TRY(select_and_create_backend(config));
    status_.backend = backend_->info();

    // 4. Connect the platform integration. Failure here is not fatal: the
    //    user-space backend path works without it.
    integration_ = create_platform_integration();
    if (integration_) {
        const Status connected = integration_->connect(status_.hardware);
        if (connected.ok()) {
            status_.integration = integration_->probe();
        } else {
            status_.integration = integration_->probe();
            status_.warnings.push_back(std::string("platform integration: ") +
                                       connected.message());
            LX_WARN << status_.warnings.back();
        }
    }

    // 5. Record the granted CPU budget for callers.
    if (config.reserved_cpu_cores == 0) {
        status_.warnings.emplace_back("no CPU cores reserved for the virtual GPU");
    }

    status_.initialized = true;
    initialized_ = true;
    LX_INFO << version_string() << " driver initialized: backend="
            << backends::to_string(status_.backend.kind) << ", reserved RAM="
            << (status_.reservation.granted_bytes / kMiB) << " MiB, reserved cores="
            << config.reserved_cpu_cores << ", integration="
            << to_string(status_.integration.level);
    return Status::success();
}

Status Driver::select_and_create_backend(const configurator::Config& config) {
    backends::BackendCreateInfo create_info;
    create_info.allow_validation = config.enable_validation;
    create_info.preferred_device = config.preferred_gpu;
    create_info.memory_budget_bytes = status_.reservation.granted_bytes;

    const std::string& preferred = config.preferred_backend;

    // "auto": probe and take the best available, hardware first.
    if (preferred == "auto") {
        const auto available = backends::probe_available_backends();
        if (available.empty()) {
            return Status::error(ErrorCode::BackendUnavailable, "no backend available");
        }
        create_info.preferred = available.front().kind;
        auto backend = backends::create_backend(create_info);
        if (!backend.ok()) {
            // Fall through to the CPU backend if permitted.
            if (!config.allow_cpu_fallback) return backend.status();
            create_info.preferred = backends::BackendKind::Cpu;
            status_.warnings.push_back(
                std::string("preferred backend failed, using CPU: ") +
                backend.status().message());
            LX_WARN << status_.warnings.back();
            auto cpu = backends::create_backend(create_info);
            if (!cpu.ok()) return cpu.status();
            backend_ = std::move(cpu.value());
            return Status::success();
        }
        backend_ = std::move(backend.value());
        return Status::success();
    }

    create_info.preferred = backend_kind_from_name(preferred);
    auto backend = backends::create_backend(create_info);
    if (backend.ok()) {
        backend_ = std::move(backend.value());
        return Status::success();
    }

    if (!config.allow_cpu_fallback) return backend.status();

    status_.warnings.push_back(std::string("backend '") + preferred +
                               "' unavailable, falling back to CPU: " +
                               backend.status().message());
    LX_WARN << status_.warnings.back();
    create_info.preferred = backends::BackendKind::Cpu;
    auto cpu = backends::create_backend(create_info);
    if (!cpu.ok()) return cpu.status();
    backend_ = std::move(cpu.value());
    return Status::success();
}

void Driver::shutdown() {
    if (integration_) {
        integration_->disconnect();
        integration_.reset();
    }
    backend_.reset();
    // Drop the whole status rather than only flipping the flag: the hardware
    // sample, backend info, reservation and warnings all describe a session that
    // has ended. Keeping them let status() describe a device that no longer
    // exists.
    status_ = DriverStatus{};
    initialized_ = false;
}

std::vector<backends::AvailableBackend> Driver::probe_backends() {
    return backends::probe_available_backends();
}

// Fallback for platforms without a dedicated integration. It reports clearly
// that no OS-specific connection is available, so callers never assume one.
#if !defined(__linux__) && !defined(_WIN32) && !defined(__APPLE__)
namespace {
class NullIntegration : public PlatformIntegration {
public:
    const char* name() const override { return "none"; }
    IntegrationStatus probe() const override {
        IntegrationStatus status;
        status.platform = "unknown";
        status.level = IntegrationLevel::Unavailable;
        status.notes.emplace_back("no platform integration for this operating system");
        return status;
    }
    Status connect(const hardware::HardwareInfo&) override {
        return Status::error(ErrorCode::NotSupported,
                             "no platform integration available");
    }
    void disconnect() override {}
};
}  // namespace

std::unique_ptr<PlatformIntegration> create_platform_integration() {
    return std::unique_ptr<PlatformIntegration>(new NullIntegration());
}
#endif

}  // namespace lx::driver
