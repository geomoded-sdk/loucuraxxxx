// LoucuraXXXX GPU - driver.
//
// The driver is the layer between the virtual GPU and the operating system. It
// performs host detection, applies the configured resource reservation, selects
// and creates a backend, and connects the platform integration. It owns the
// memory budget so that every other module commits against one source of truth.
//
// It does not claim to be a kernel-mode driver. On platforms where that would
// be required (Windows WDDM, macOS), it stays in user space and says so through
// its reported status.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "backends/backend.hpp"
#include "backends/backend_registry.hpp"
#include "configurator/config.hpp"
#include "core/status.hpp"
#include "driver/platform_integration.hpp"
#include "hardware/hardware_info.hpp"
#include "memory/memory_budget.hpp"

namespace lx::driver {

// The full state the driver exposes after initialization.
struct DriverStatus {
    bool initialized = false;
    hardware::HardwareInfo hardware;
    backends::BackendInfo backend;
    memory::ReservationPlan reservation;
    IntegrationStatus integration;
    std::vector<std::string> warnings;
};

class Driver {
public:
    Driver() = default;
    ~Driver();

    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    // Detects the host, applies `config`, and brings up a backend. When the
    // preferred backend is unavailable and `config.allow_cpu_fallback` is set,
    // it falls back to the CPU backend and records why.
    Status initialize(const configurator::Config& config);

    // Releases the backend and integration. Idempotent.
    void shutdown();

    bool initialized() const { return initialized_; }
    const DriverStatus& status() const { return status_; }

    // The selected backend, or nullptr before initialize().
    backends::Backend* backend() { return backend_.get(); }
    const backends::Backend* backend() const { return backend_.get(); }

    // The shared memory budget every module commits against.
    memory::MemoryBudget& memory_budget() { return budget_; }
    const memory::MemoryBudget& memory_budget() const { return budget_; }

    const configurator::Config& config() const { return config_; }

    // Reports backend availability without creating a device. Useful for a
    // configuration UI.
    static std::vector<backends::AvailableBackend> probe_backends();

private:
    Status select_and_create_backend(const configurator::Config& config);

    bool initialized_ = false;
    configurator::Config config_{};
    DriverStatus status_{};
    memory::MemoryBudget budget_{};
    std::unique_ptr<backends::Backend> backend_;
    std::unique_ptr<PlatformIntegration> integration_;
};

}  // namespace lx::driver
