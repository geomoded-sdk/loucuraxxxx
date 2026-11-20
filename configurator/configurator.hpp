// LoucuraXXXX GPU - Configurator.
//
// Owns the user-facing configuration: it validates a request against the real
// host, clamps anything unsafe, and persists the result. It is the only module
// that reads or writes the on-disk configuration file.
#pragma once

#include <string>
#include <vector>

#include "configurator/config.hpp"
#include "core/status.hpp"
#include "hardware/hardware_info.hpp"
#include "memory/memory_budget.hpp"

namespace lx::configurator {

// Resolved, host-aware limits. Computed once from HardwareInfo and reused.
struct HostLimits {
    u32 max_cpu_cores = 1;         // cores the virtual GPU may use
    u64 max_ram_bytes = 0;         // bytes the virtual GPU may reserve
    u64 system_total_bytes = 0;
    u64 system_available_bytes = 0;
    bool has_usable_gpu = false;
    std::string backend_hint;      // "vulkan", "metal", "cpu"

    // Hybrid CPU topology, when the host reports it. On Apple silicon this is
    // the P-core / E-core split, which the manager uses to place latency-bound
    // work on performance cores and background work on efficiency cores.
    bool hybrid_cpu = false;
    u32 performance_cores = 0;
    u32 efficiency_cores = 0;

    // Whether CPU and GPU share one memory pool (Apple silicon, integrated
    // GPUs). When true, no host<->device transfer is needed.
    bool unified_memory = false;
};

class Configurator {
public:
    Configurator() = default;

    // Binds the configurator to a host. Must be called before validate().
    void attach_host(const hardware::HardwareInfo& hardware);

    const HostLimits& limits() const { return limits_; }
    const hardware::HardwareInfo& hardware() const { return hardware_; }

    // Validates and clamps a requested configuration. Never throws; problems are
    // reported in the returned report.
    ValidationReport validate(const Config& requested) const;

    // Validates a request and, if valid, stores it as the active configuration.
    Result<ValidationReport> apply(const Config& requested);

    const Config& active() const { return active_; }
    bool has_active_config() const { return has_active_; }

    // Loads a configuration from a JSON file, validates it against the host, and
    // makes it active. Returns the report describing any clamping.
    Result<ValidationReport> load(const std::string& path);

    // Saves the active configuration to a JSON file.
    Status save(const std::string& path) const;

    // Returns the default configuration file path for this user.
    static Result<std::string> default_config_path();

private:
    Config parse_json(const std::string& text, std::vector<std::string>& errors) const;
    std::string serialize_json(const Config& config) const;

    hardware::HardwareInfo hardware_{};
    HostLimits limits_{};
    Config active_{};
    bool has_active_ = false;
    bool host_attached_ = false;
};

}  // namespace lx::configurator
