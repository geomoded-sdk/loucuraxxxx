#include "configurator/configurator.hpp"

#include <algorithm>
#include <sstream>

#include "backends/backend_registry.hpp"
#include "core/files.hpp"
#include "core/json.hpp"
#include "core/log.hpp"
#include "core/version.hpp"
#include "filters/filter_registry.hpp"

namespace lx::configurator {

namespace {

u32 parse_priority(const std::string& text) {
    if (text == "low") return static_cast<u32>(Priority::Low);
    if (text == "high") return static_cast<u32>(Priority::High);
    return static_cast<u32>(Priority::Normal);
}

// Converts a JSON number to u32, clamping to the valid range. A negative or
// oversized value would otherwise wrap through the cast into a huge count.
u32 as_u32_clamped(const Json& value, long long fallback) {
    const long long v = value.as_int(fallback);
    if (v < 0) return 0;
    if (v > 0xFFFFFFFFll) return 0xFFFFFFFFu;
    return static_cast<u32>(v);
}

}  // namespace

void Configurator::attach_host(const hardware::HardwareInfo& hardware) {
    hardware_ = hardware;
    host_attached_ = true;

    limits_ = HostLimits{};
    limits_.system_total_bytes = hardware.memory.total_bytes;
    limits_.system_available_bytes = hardware.memory.available_bytes;

    // CPU: the OS needs at least one core for itself and other processes. We
    // also keep one spare beyond that on machines with more than two cores so a
    // game or the desktop stays responsive.
    const u32 logical = std::max(1u, hardware.cpu.logical_cores);
    u32 reserved = 1;                      // always leave one for the OS
    if (logical >= 4) reserved = 2;        // leave a second on roomier machines
    limits_.max_cpu_cores = logical > reserved ? logical - reserved : 1;

    // RAM: the memory policy is the single source of truth for what is safe.
    memory::MemoryPolicy policy;
    const memory::ReservationPlan plan =
        policy.plan(hardware.memory, hardware.memory.total_bytes);
    limits_.max_ram_bytes = plan.granted_bytes;

    // Hybrid CPU topology and unified memory: both are properties of the host
    // that change how work should be placed, so record them here once.
    limits_.hybrid_cpu = hardware.is_hybrid_cpu();
    limits_.performance_cores = hardware.cpu.performance_cores;
    limits_.efficiency_cores = hardware.cpu.efficiency_cores;
    limits_.unified_memory = hardware.has_unified_memory();

    // GPU/backend hint.
    const hardware::GpuDeviceInfo* gpu = hardware.primary_gpu();
    limits_.has_usable_gpu = gpu != nullptr;
    if (gpu != nullptr) {
        switch (gpu->api) {
            case hardware::GraphicsApi::Vulkan: limits_.backend_hint = "vulkan"; break;
            case hardware::GraphicsApi::Metal: limits_.backend_hint = "metal"; break;
            default: limits_.backend_hint = "auto"; break;
        }
    } else {
        limits_.backend_hint = "cpu";
    }
    if (limits_.backend_hint == "auto") {
        // Choose from the platform when the driver-level probe was silent.
#if defined(__APPLE__)
        limits_.backend_hint = "metal";
#elif defined(_WIN32) || defined(__linux__)
        limits_.backend_hint = "vulkan";
#else
        limits_.backend_hint = "cpu";
#endif
    }
}

ValidationReport Configurator::validate(const Config& requested) const {
    ValidationReport report;
    if (!host_attached_) {
        report.errors.emplace_back("configurator has no host attached");
        report.effective = requested;
        return report;
    }
    Config effective = requested;

    // --- RAM reservation ---
    if (effective.reserved_ram_bytes == 0) {
        effective.reserved_ram_bytes = 512 * kMiB;
        report.adjustments.emplace_back(
            "reserved_ram_bytes was 0; raised to 512 MiB minimum");
    }
    if (effective.reserved_ram_bytes > limits_.max_ram_bytes) {
        std::ostringstream msg;
        msg << "reserved_ram_bytes " << (effective.reserved_ram_bytes / kMiB)
            << " MiB exceeds safe limit " << (limits_.max_ram_bytes / kMiB)
            << " MiB (system total " << (limits_.system_total_bytes / kMiB)
            << " MiB); clamped";
        report.adjustments.push_back(msg.str());
        effective.reserved_ram_bytes = limits_.max_ram_bytes;
    }

    // --- CPU cores ---
    if (effective.reserved_cpu_cores == 0) {
        effective.reserved_cpu_cores = 1;
        report.adjustments.emplace_back("reserved_cpu_cores was 0; raised to 1");
    }
    if (effective.reserved_cpu_cores > limits_.max_cpu_cores) {
        std::ostringstream msg;
        msg << "reserved_cpu_cores " << effective.reserved_cpu_cores
            << " exceeds safe limit " << limits_.max_cpu_cores << " of "
            << hardware_.cpu.logical_cores << " logical cores; clamped";
        report.adjustments.push_back(msg.str());
        effective.reserved_cpu_cores = limits_.max_cpu_cores;
    }

    // --- Backend ---
    if (effective.preferred_backend != "auto" && effective.preferred_backend != "cpu" &&
        !limits_.has_usable_gpu) {
        report.adjustments.emplace_back(
            "no usable physical GPU detected; falling back to the CPU backend");
        effective.preferred_backend = "cpu";
    }
    // A name that matches no registered backend is a request the rest of the
    // system cannot honour. Letting it through would defer the failure to driver
    // init, where it is harder to attribute, so it is resolved here. The registry
    // is consulted rather than the probe: validating a config must not create a
    // device.
    if (effective.preferred_backend != "auto" && effective.preferred_backend != "cpu") {
        const auto kind = backends::backend_kind_from_string(effective.preferred_backend);
        if (!kind.has_value() || backends::find_backend(*kind) == nullptr) {
            report.adjustments.emplace_back("preferred_backend '" +
                                            effective.preferred_backend +
                                            "' is not available; using 'auto'");
            effective.preferred_backend = "auto";
        }
    }

    // --- Filters ---
    for (auto& filter : effective.filters) {
        // A filter name that is not registered cannot be built later, so the
        // chain would fail after the config was accepted. Report it here, where
        // the message can name the filter and reach the user.
        if (!filter.name.empty() && filters::find_filter(filter.name) == nullptr) {
            report.errors.push_back("unknown filter '" + filter.name + "'");
        }
        if (filter.intensity < 0.0f) {
            filter.intensity = 0.0f;
            report.adjustments.emplace_back("filter '" + filter.name +
                                            "' intensity < 0; clamped to 0");
        }
        if (filter.intensity > 1.0f) {
            filter.intensity = 1.0f;
            report.adjustments.emplace_back("filter '" + filter.name +
                                            "' intensity > 1; clamped to 1");
        }
        if (filter.quality != "fast" && filter.quality != "balanced" &&
            filter.quality != "quality") {
            report.adjustments.emplace_back("filter '" + filter.name +
                                            "' unknown quality; set to balanced");
            filter.quality = "balanced";
        }
        if (filter.scope != "any" && filter.scope != "game" && filter.scope != "system") {
            report.adjustments.emplace_back("filter '" + filter.name +
                                            "' unknown scope; set to any");
            filter.scope = "any";
        }
    }

    // vsync needs a present surface to synchronise against, and this project has
    // none: the renderer draws into a texture and never owns a window or
    // swapchain. Storing the flag and ignoring it would let a caller believe
    // frames were being tear-free. It is reported and cleared instead.
    if (effective.vsync) {
        effective.vsync = false;
        report.adjustments.emplace_back(
            "vsync requested but there is no present surface to synchronise with; "
            "left off (use target_fps to cap the frame rate)");
    }

    if (effective.max_in_flight_frames == 0) {
        effective.max_in_flight_frames = 1;
        report.adjustments.emplace_back("max_in_flight_frames was 0; raised to 1");
    }
    if (effective.max_in_flight_frames > 4) {
        effective.max_in_flight_frames = 4;
        report.adjustments.emplace_back("max_in_flight_frames > 4 adds latency; clamped to 4");
    }

    report.effective = effective;
    return report;
}

Result<ValidationReport> Configurator::apply(const Config& requested) {
    ValidationReport report = validate(requested);
    if (!report.ok()) return report;  // errors, not adjustments
    active_ = report.effective;
    has_active_ = true;
    for (const auto& adjustment : report.adjustments) {
        LX_WARN << "config adjusted: " << adjustment;
    }
    return report;
}

Result<std::string> Configurator::default_config_path() {
    auto dir = lx::config_directory();
    if (!dir.ok()) return dir.status();
    return dir.value() + "/config.json";
}

Config Configurator::parse_json(const std::string& text,
                                std::vector<std::string>& errors) const {
    Config config;
    auto parsed = lx::Json::parse(text);
    if (!parsed.ok()) {
        errors.push_back("config parse error: " + parsed.status().message());
        return config;
    }
    const lx::Json& root = parsed.value();
    if (!root.is_object()) {
        errors.push_back("config root must be a JSON object");
        return config;
    }

    if (root.has("reserved_ram_mib")) {
        const long long mib = root.at("reserved_ram_mib").as_int(-1);
        // Bound before scaling: mib * kMiB would overflow u64 for a huge value
        // and wrap to a small reservation that skips the safety clamp.
        if (mib < 0) {
            errors.push_back("reserved_ram_mib must be >= 0");
        } else if (static_cast<unsigned long long>(mib) > (~0ull) / kMiB) {
            errors.push_back("reserved_ram_mib is too large");
        } else {
            config.reserved_ram_bytes = static_cast<u64>(mib) * kMiB;
        }
    }
    if (root.has("reserved_cpu_cores")) {
        const long long cores = root.at("reserved_cpu_cores").as_int(-1);
        if (cores < 0) errors.push_back("reserved_cpu_cores must be >= 0");
        else if (cores > 0xFFFFFFFFll) errors.push_back("reserved_cpu_cores is too large");
        else config.reserved_cpu_cores = static_cast<u32>(cores);
    }
    if (root.has("priority")) {
        config.priority = static_cast<Priority>(parse_priority(root.at("priority").as_string()));
    }
    if (root.has("preferred_backend")) {
        config.preferred_backend = root.at("preferred_backend").as_string("auto");
    }
    if (root.has("preferred_gpu")) {
        config.preferred_gpu = root.at("preferred_gpu").as_string();
    }
    if (root.has("filters_enabled")) {
        config.filters_enabled = root.at("filters_enabled").as_bool(true);
    }
    if (root.has("enable_validation")) {
        config.enable_validation = root.at("enable_validation").as_bool(false);
    }
    if (root.has("target_fps")) {
        config.target_fps = as_u32_clamped(root.at("target_fps"), 0);
    }
    if (root.has("vsync")) config.vsync = root.at("vsync").as_bool(false);
    if (root.has("allow_cpu_gpu_split")) {
        config.allow_cpu_gpu_split = root.at("allow_cpu_gpu_split").as_bool(true);
    }
    if (root.has("allow_cpu_fallback")) {
        config.allow_cpu_fallback = root.at("allow_cpu_fallback").as_bool(true);
    }
    if (root.has("max_in_flight_frames")) {
        config.max_in_flight_frames = as_u32_clamped(root.at("max_in_flight_frames"), 2);
    }
    if (root.has("telemetry_interval_ms")) {
        config.telemetry_interval_ms =
            as_u32_clamped(root.at("telemetry_interval_ms"), 1000);
    }

    if (root.has("filters")) {
        const lx::Json& filters = root.at("filters");
        if (!filters.is_array()) {
            errors.push_back("'filters' must be an array");
        } else {
            for (const lx::Json& item : filters.items()) {
                if (!item.is_object()) {
                    errors.push_back("each filter entry must be an object");
                    continue;
                }
                FilterConfig fc;
                fc.name = item.at("name").as_string();
                if (fc.name.empty()) {
                    errors.push_back("filter entry missing 'name'");
                    continue;
                }
                fc.enabled = item.at("enabled").as_bool(true);
                fc.intensity = static_cast<f32>(item.at("intensity").as_number(1.0));
                const long long passes = item.at("passes").as_int(1);
                fc.passes = (passes < 0 || passes > 0xFFFFFFFFll)
                                ? 1u
                                : static_cast<u32>(passes);
                fc.quality = item.at("quality").as_string("balanced");
                fc.scope = item.at("scope").as_string("any");
                config.filters.push_back(std::move(fc));
            }
        }
    }
    return config;
}

std::string Configurator::serialize_json(const Config& config) const {
    lx::Json root = lx::Json::object();
    root["version"] = lx::version_string();
    root["reserved_ram_mib"] = static_cast<unsigned long long>(config.reserved_ram_bytes / kMiB);
    root["reserved_cpu_cores"] = config.reserved_cpu_cores;
    root["priority"] = to_string(config.priority);
    root["preferred_backend"] = config.preferred_backend;
    root["preferred_gpu"] = config.preferred_gpu;
    root["filters_enabled"] = config.filters_enabled;
    root["enable_validation"] = config.enable_validation;
    root["target_fps"] = config.target_fps;
    root["vsync"] = config.vsync;
    root["allow_cpu_gpu_split"] = config.allow_cpu_gpu_split;
    root["allow_cpu_fallback"] = config.allow_cpu_fallback;
    root["max_in_flight_frames"] = config.max_in_flight_frames;
    root["telemetry_interval_ms"] = config.telemetry_interval_ms;

    lx::Json filters = lx::Json::array();
    for (const auto& fc : config.filters) {
        lx::Json item = lx::Json::object();
        item["name"] = fc.name;
        item["enabled"] = fc.enabled;
        item["intensity"] = static_cast<double>(fc.intensity);
        item["passes"] = fc.passes;
        item["quality"] = fc.quality;
        item["scope"] = fc.scope;
        filters.push_back(std::move(item));
    }
    root["filters"] = std::move(filters);
    return root.dump(2);
}

Result<ValidationReport> Configurator::load(const std::string& path) {
    auto text = lx::read_file(path);
    if (!text.ok()) return text.status();

    std::vector<std::string> errors;
    Config parsed = parse_json(text.value(), errors);
    if (!errors.empty()) {
        std::ostringstream msg;
        for (const auto& e : errors) msg << e << "; ";
        return Status::error(ErrorCode::ParseError, msg.str());
    }
    return apply(parsed);
}

Status Configurator::save(const std::string& path) const {
    if (!has_active_) {
        return Status::error(ErrorCode::InvalidArgument, "no active configuration to save");
    }
    return lx::write_file(path, serialize_json(active_));
}

}  // namespace lx::configurator
