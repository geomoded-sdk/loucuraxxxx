// LoucuraXXXX GPU - configuration model.
//
// The configuration is plain data with no logic. Validation and clamping live
// in the Configurator so that a config loaded from disk can always be checked
// against the host it will run on.
#pragma once

#include <string>
#include <vector>

#include "core/types.hpp"

namespace lx::configurator {

// Processing priority requested by the user. This maps onto the OS scheduler
// and onto how aggressively the manager admits work.
enum class Priority { Low, Normal, High };

inline const char* to_string(Priority p) {
    switch (p) {
        case Priority::Low: return "low";
        case Priority::Normal: return "normal";
        case Priority::High: return "high";
    }
    return "normal";
}

// Configuration for a single filter instance. Filters are identified by name
// and configured with a small set of typed parameters.
struct FilterConfig {
    std::string name;        // matches IFilter::name()
    bool enabled = true;
    f32 intensity = 1.0f;    // 0..1 blend with the original image
    u32 passes = 1;          // number of iterations; 0 means "filter default"
    std::string quality = "balanced";  // "fast", "balanced", "quality"
    // Which content this filter applies to: "any" (default), "game" or
    // "system". A game filter is skipped on system frames and vice versa, so the
    // desktop UI is never run through a filter tuned for a game.
    std::string scope = "any";
};

// What the user asks for. All values are requests: the Configurator may clamp
// them to what the machine can safely provide.
struct Config {
    // --- Resource reservation ---
    u64 reserved_ram_bytes = 2 * kGiB;
    u32 reserved_cpu_cores = 2;
    Priority priority = Priority::Normal;

    // --- Backend / device selection ---
    std::string preferred_backend = "auto";  // "auto", "vulkan", "metal", "cpu"
    std::string preferred_gpu;               // empty => auto-select

    // --- Filters ---
    std::vector<FilterConfig> filters;
    bool filters_enabled = true;

    // --- Renderer ---
    bool enable_validation = false;  // backend validation layers when available
    u32 target_fps = 0;              // 0 => uncapped
    // Not honourable today: the renderer owns no window or swapchain, so there is
    // nothing to synchronise against. The Configurator reports the request and
    // leaves it off; use target_fps to cap the frame rate.
    bool vsync = false;

    // --- Manager ---
    bool allow_cpu_gpu_split = true;
    bool allow_cpu_fallback = true;  // run on CPU when no usable GPU exists
    u32 max_in_flight_frames = 2;

    // --- Diagnostics ---
    u32 telemetry_interval_ms = 1000;
};

// The effective configuration after validation, plus a record of every change
// the Configurator made and why. Nothing is silently altered.
struct ValidationReport {
    Config effective;
    std::vector<std::string> adjustments;  // human-readable list of clamps
    std::vector<std::string> errors;       // non-recoverable problems

    bool ok() const { return errors.empty(); }
};

}  // namespace lx::configurator
