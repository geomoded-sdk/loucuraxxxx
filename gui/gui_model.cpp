#include "gui/gui_model.hpp"

#include "core/clock.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <vector>

#include "backends/backend_registry.hpp"
#include "configurator/configurator.hpp"
#include "core/files.hpp"
#include "filters/filter_registry.hpp"
#include "hardware/hardware_info.hpp"
#include "renderer/renderer.hpp"

namespace lx::gui {

namespace {

std::string format_mib(u64 bytes) {
    std::ostringstream ss;
    ss << (bytes / kMiB) << " MiB";
    return ss.str();
}

}  // namespace

backends::BackendKind GuiModel::requested_backend_kind() const {
    const std::string& name = state_.config.preferred_backend;
    if (name == "vulkan") return backends::BackendKind::Vulkan;
    if (name == "d3d11" || name == "dx11" || name == "direct3d11") {
        return backends::BackendKind::Direct3D11;
    }
    if (name == "opengl" || name == "gl" || name == "gl3") {
        return backends::BackendKind::OpenGL;
    }
    if (name == "metal") return backends::BackendKind::Metal;
    if (name == "cpu") return backends::BackendKind::Cpu;
    // "auto" means the best backend the probe actually finds, which is the same
    // rule the CLI uses: real hardware, then the CPU path, then a software
    // rasterizer. Falling back to CPU unconditionally here would make the GUI
    // silently ignore a working GPU.
    const auto available = backends::probe_available_backends();
    if (!available.empty()) return available.front().kind;
    return backends::BackendKind::Cpu;
}

Status GuiModel::refresh_host() {
    auto hardware = hardware::detect_hardware();
    if (!hardware.ok()) {
        log("hardware detection failed: " + hardware.status().message(), true);
        return hardware.status();
    }
    const hardware::HardwareInfo& hw = hardware.value();

    state_.cpu_model = hw.cpu.model;
    state_.logical_cores = hw.cpu.logical_cores;
    state_.physical_cores = hw.cpu.physical_cores;
    state_.performance_cores = hw.cpu.performance_cores;
    state_.efficiency_cores = hw.cpu.efficiency_cores;
    state_.hybrid_cpu = hw.is_hybrid_cpu();
    state_.unified_memory = hw.has_unified_memory();
    state_.os_summary = hw.os.name + " " + hw.os.version + " (" + hw.os.architecture + ")";
    state_.total_memory_bytes = hw.memory.total_bytes;
    state_.available_memory_bytes = hw.memory.available_bytes;
    state_.memory_topology = hardware::to_string(hw.memory_topology());

    state_.gpus.clear();
    for (const auto& gpu : hw.gpus) {
        std::string line = gpu.name + " [" + hardware::to_string(gpu.kind) + ", " +
                           hardware::to_string(gpu.api);
        if (gpu.unified_memory) line += ", unified memory";
        line += "]";
        state_.gpus.push_back(std::move(line));
    }

    state_.backends.clear();
    for (const auto& reg : backends::probe_available_backends()) {
        BackendChoice choice;
        choice.name = reg.name;
        choice.kind = backends::to_string(reg.kind);
        // Comes from the probe, not from the backend kind: an API present only
        // through a software rasterizer must not be shown as acceleration.
        choice.is_hardware = reg.is_hardware;
        state_.backends.push_back(std::move(choice));
    }

    state_.filters.clear();
    for (const auto& reg : filters::registered_filters()) {
        FilterChoice choice;
        choice.name = reg.info.name;
        choice.description = reg.info.description;
        state_.filters.push_back(std::move(choice));
    }
    // Reflect whatever the editing config already enables so the checkboxes
    // start in the right state.
    sync_filter_choices();

    // Recompute the safe limits from the host so the front end can clamp its
    // controls to what the machine can actually provide.
    configurator::Configurator configurator;
    configurator.attach_host(hw);
    state_.max_ram_bytes = configurator.limits().max_ram_bytes;
    state_.max_cpu_cores = configurator.limits().max_cpu_cores;

    // Clamp the editing config to the new limits so the UI never shows an
    // unsafe value.
    if (state_.config.reserved_ram_bytes > state_.max_ram_bytes) {
        state_.config.reserved_ram_bytes = state_.max_ram_bytes;
    }
    if (state_.config.reserved_cpu_cores > state_.max_cpu_cores) {
        state_.config.reserved_cpu_cores = state_.max_cpu_cores;
    }

    log("host detected: " + state_.cpu_model + ", " +
        format_mib(state_.total_memory_bytes) + " total");
    log("backends available: " + std::to_string(state_.backends.size()) + ", filters: " +
        std::to_string(state_.filters.size()));
    return Status::success();
}

void GuiModel::log(std::string text, bool is_error) {
    state_.log.push_back(LogLine{std::move(text), is_error});
}

Status GuiModel::validate() {
    auto hardware = hardware::detect_hardware();
    if (!hardware.ok()) {
        log("validate: hardware detection failed", true);
        return hardware.status();
    }
    configurator::Configurator configurator;
    configurator.attach_host(hardware.value());

    // Re-detect limits and store them, so the value the front end displays is
    // the same one validate() clamps against. Available memory can shift between
    // refresh_host() and validate(); keeping a stale ceiling here would let the
    // effective config sit above the displayed limit.
    state_.max_ram_bytes = configurator.limits().max_ram_bytes;
    state_.max_cpu_cores = configurator.limits().max_cpu_cores;

    configurator::ValidationReport report = configurator.validate(state_.config);
    state_.validated = true;
    state_.validation_ok = report.ok();
    state_.validation_adjustments = report.adjustments;
    state_.validation_errors = report.errors;

    if (!report.ok()) {
        for (const auto& error : report.errors) log("validation error: " + error, true);
        return Status::error(ErrorCode::InvalidArgument, "configuration is not valid");
    }
    // Adopt the clamped configuration so subsequent renders use safe values.
    state_.config = report.effective;
    log("configuration valid: RAM=" + format_mib(report.effective.reserved_ram_bytes) +
        ", cores=" + std::to_string(report.effective.reserved_cpu_cores) +
        ", backend=" + report.effective.preferred_backend);
    for (const auto& adjustment : report.adjustments) {
        log("adjusted: " + adjustment);
    }
    return Status::success();
}

Status GuiModel::save_config() {
    auto path = configurator::Configurator::default_config_path();
    if (!path.ok()) return path.status();

    // Round-trip through a Configurator so the file always holds a validated
    // configuration, never a raw request.
    auto hardware = hardware::detect_hardware();
    if (!hardware.ok()) return hardware.status();
    configurator::Configurator configurator;
    configurator.attach_host(hardware.value());
    auto applied = configurator.apply(state_.config);
    if (!applied.ok()) {
        log("cannot save an invalid configuration", true);
        return applied.status();
    }
    Status status = configurator.save(path.value());
    if (!status.ok()) {
        log("save failed: " + status.message(), true);
        return status;
    }
    log("configuration saved to " + path.value());
    return Status::success();
}

Status GuiModel::load_config() {
    auto path = configurator::Configurator::default_config_path();
    if (!path.ok()) return path.status();
    if (!file_exists(path.value())) {
        log("no saved configuration at " + path.value());
        return Status::success();
    }
    auto hardware = hardware::detect_hardware();
    if (!hardware.ok()) return hardware.status();
    configurator::Configurator configurator;
    configurator.attach_host(hardware.value());
    auto report = configurator.load(path.value());
    if (!report.ok()) {
        log("load failed: " + report.status().message(), true);
        return report.status();
    }
    state_.config = configurator.active();
    state_.validated = true;
    state_.validation_ok = true;
    state_.validation_adjustments = report.value().adjustments;
    state_.validation_errors.clear();
    sync_filter_choices();
    log("configuration loaded from " + path.value());
    return Status::success();
}

namespace {
// Finds the config entry for `name`, or nullptr when the filter is not in the
// list. Kept local because the three setters below all need it.
configurator::FilterConfig* find_filter_config(configurator::Config& config,
                                               const std::string& name) {
    for (auto& fc : config.filters) {
        if (fc.name == name) return &fc;
    }
    return nullptr;
}
}  // namespace

Status GuiModel::set_filter_enabled(const std::string& name, bool enabled) {
    if (filters::create_filter(name).status().code() == ErrorCode::NotImplemented) {
        return Status::error(ErrorCode::InvalidArgument, "unknown filter '" + name + "'");
    }
    configurator::FilterConfig* fc = find_filter_config(state_.config, name);
    if (enabled) {
        if (fc == nullptr) {
            configurator::FilterConfig fresh;
            fresh.name = name;
            fresh.enabled = true;
            state_.config.filters.push_back(std::move(fresh));
        } else {
            fc->enabled = true;
        }
    } else if (fc != nullptr) {
        // Disabling keeps the entry (and its intensity/scope) so the user's
        // tuning survives a toggle off and on; the chain already skips
        // !enabled entries.
        fc->enabled = false;
    }
    sync_filter_choices();
    return Status::success();
}

Status GuiModel::set_filter_scope(const std::string& name, const std::string& scope) {
    if (scope != "any" && scope != "game" && scope != "system") {
        return Status::error(ErrorCode::InvalidArgument, "unknown scope '" + scope + "'");
    }
    configurator::FilterConfig* fc = find_filter_config(state_.config, name);
    if (fc == nullptr) {
        return Status::error(ErrorCode::InvalidArgument,
                             "filter '" + name + "' is not enabled");
    }
    fc->scope = scope;
    sync_filter_choices();
    return Status::success();
}

Status GuiModel::set_filter_intensity(const std::string& name, f32 intensity) {
    configurator::FilterConfig* fc = find_filter_config(state_.config, name);
    if (fc == nullptr) {
        return Status::error(ErrorCode::InvalidArgument,
                             "filter '" + name + "' is not enabled");
    }
    fc->intensity = std::min(std::max(intensity, 0.0f), 1.0f);
    sync_filter_choices();
    return Status::success();
}

void GuiModel::sync_filter_choices() {
    for (auto& choice : state_.filters) {
        const configurator::FilterConfig* fc = find_filter_config(state_.config, choice.name);
        choice.enabled = fc != nullptr && fc->enabled;
        choice.scope = fc != nullptr ? fc->scope : std::string("any");
        if (fc != nullptr) choice.default_intensity = fc->intensity;
    }
    if (state_.filters.empty()) {
        state_.selected_filter = 0;
    } else if (state_.selected_filter >= state_.filters.size()) {
        state_.selected_filter = state_.filters.size() - 1;
    }
}

Status GuiModel::render_frames(u32 width, u32 height, u32 frames) {
    if (width == 0 || height == 0 || frames == 0) {
        log("render: width, height and frame count must be non-zero", true);
        return Status::error(ErrorCode::InvalidArgument, "invalid render dimensions");
    }

    renderer::RendererDesc desc;
    desc.width = width;
    desc.height = height;
    desc.backend.preferred = requested_backend_kind();
    desc.shader_asset_root = "shaders";
    desc.enable_filters = state_.config.filters_enabled;
    // The reserved cores are what the virtual GPU may spend on filter work; the
    // renderer owns a pool of that size and splits row-splittable filters.
    desc.cpu_worker_count = state_.config.reserved_cpu_cores;
    desc.target_fps = state_.config.target_fps;
    desc.max_in_flight_frames = state_.config.max_in_flight_frames;
    desc.backend.allow_validation = state_.config.enable_validation;
    desc.performance_cores = state_.performance_cores;
    desc.efficiency_cores = state_.efficiency_cores;

    renderer::Renderer renderer;
    Status status = renderer.initialize(desc);
    if (!status.ok() && desc.backend.preferred != backends::BackendKind::Cpu) {
        // The requested hardware backend is not usable here. Say so, then fall
        // back to the CPU backend the same way the driver does, so the GUI
        // still produces a frame - but it never reports the GPU it did not use.
        log("backend '" + state_.config.preferred_backend + "' unavailable: " +
                status.message() + "; falling back to CPU",
            true);
        desc.backend.preferred = backends::BackendKind::Cpu;
        status = renderer.initialize(desc);
    }
    if (!status.ok()) {
        log("renderer init failed: " + status.message(), true);
        return status;
    }
    log("rendering on backend: " + renderer.backend_info().device_name);

    std::vector<std::pair<std::string, filters::FilterParams>> chain;
    if (state_.config.filters_enabled) {
        for (const auto& fc : state_.config.filters) {
            if (!fc.enabled) continue;
            filters::FilterParams params;
            params.intensity = fc.intensity;
            params.passes = fc.passes;
            params.quality = fc.quality;
            params.scope = filters::content_scope_from_string(fc.scope);
            chain.emplace_back(fc.name, params);
        }
    }
    status = renderer.configure_filters(chain);
    if (!status.ok()) {
        log("filter setup failed: " + status.message(), true);
        return status;
    }

    // Guard the pixel-count multiply: absurd dimensions would wrap the size_t
    // and the fill loop below would write past the (small) buffer.
    if (static_cast<u64>(width) * height > std::numeric_limits<std::size_t>::max() / 4) {
        log("render: dimensions are absurd", true);
        return Status::error(ErrorCode::InvalidArgument, "invalid render dimensions");
    }
    std::vector<u8> pixels(static_cast<std::size_t>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * width + x) * 4;
            pixels[o + 0] = static_cast<u8>(x * 255 / std::max(1u, width - 1));
            pixels[o + 1] = static_cast<u8>(y * 255 / std::max(1u, height - 1));
            pixels[o + 2] = 128;
            pixels[o + 3] = 255;
        }
    }
    renderer::FrameInput input;
    input.pixels = pixels.data();
    input.bytes = pixels.size();

    // Per-frame logging is only useful at low frame counts; a real render loop
    // would flood the log. telemetry_interval_ms decides how often a progress
    // line is emitted: 0 means every frame, otherwise a line at most that often.
    // The final summary below always reports the measured totals.
    const u64 interval_nanos =
        static_cast<u64>(state_.config.telemetry_interval_ms) * 1'000'000ull;
    u64 next_report = 0;
    for (u32 frame = 0; frame < frames; ++frame) {
        renderer::FrameStats stats;
        status = renderer.render_frame(input, &stats);
        if (!status.ok()) {
            log("render failed: " + status.message(), true);
            return status;
        }
        const u64 now = now_nanos();
        if (frame == 0 || interval_nanos == 0 || now >= next_report) {
            std::ostringstream ss;
            ss << "frame " << frame << ": cpu=" << stats.cpu_time_micros()
               << "us filter=" << stats.filter_time_micros() << "us";
            log(ss.str());
            next_report = now + interval_nanos;
        }
    }
    std::ostringstream ss;
    ss << "average fps (measured over " << renderer.stats().sample_count()
       << " frames): " << renderer.stats().average_fps();
    log(ss.str());
    return Status::success();
}

}  // namespace lx::gui
