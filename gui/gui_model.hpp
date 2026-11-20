// LoucuraXXXX GPU - GUI model (shared, platform independent).
//
// The graphical front end is a thin shell over this model. Everything the GUI
// can do - detect the host, list backends and filters, validate a
// configuration, run a render - lives here so it can be unit tested without a
// display, and so the Win32 and X11 front ends contain only widget plumbing.
//
// The model never fabricates a result: every action either performs real work
// through the same library APIs the command line tool uses, or reports why it
// could not.
#pragma once

#include <string>
#include <vector>

#include "backends/backend.hpp"
#include "configurator/config.hpp"
#include "core/status.hpp"
#include "hardware/hardware_info.hpp"

namespace lx::gui {

// One log line shown in the GUI's output pane. `is_error` lets the front end
// colour failures without parsing the text.
struct LogLine {
    std::string text;
    bool is_error = false;
};

// A backend the GUI can offer in the "backend" selector.
struct BackendChoice {
    std::string name;      // e.g. "CPU reference backend"
    std::string kind;      // e.g. "cpu", "vulkan"
    bool is_hardware = false;
};

// A filter the GUI can offer, with its configuration knobs. `enabled` and
// `intensity` mirror the matching entry in GuiState::config.filters so the front
// end can draw a checkbox per filter without walking the config itself.
struct FilterChoice {
    std::string name;
    std::string description;
    float default_intensity = 1.0f;
    bool enabled = false;
    std::string scope = "any";  // "any" / "game" / "system"
};

// The full, serialisable state the GUI presents. The front ends read this to
// draw and write to it through the setters below; they never touch the library
// directly.
struct GuiState {
    // Host summary (filled by refresh_host()).
    std::string cpu_model;
    u32 logical_cores = 0;
    u32 physical_cores = 0;
    u32 performance_cores = 0;
    u32 efficiency_cores = 0;
    bool hybrid_cpu = false;
    bool unified_memory = false;
    std::string os_summary;
    u64 total_memory_bytes = 0;
    u64 available_memory_bytes = 0;
    std::string memory_topology;  // "unified" / "discrete" / "unknown"

    std::vector<std::string> gpus;         // one human-readable line per GPU
    std::vector<BackendChoice> backends;   // probed, hardware first
    std::vector<FilterChoice> filters;     // registered filters
    // Index of the filter the front end has selected for editing (the toggles
    // act on this one). Clamped to a valid index by sync_filter_choices().
    std::size_t selected_filter = 0;

    // Safe limits derived from the host; the sliders are clamped to these.
    u64 max_ram_bytes = 0;
    u32 max_cpu_cores = 1;

    // The configuration the user is editing.
    configurator::Config config;

    // Results of the last validate / render action.
    bool validated = false;
    bool validation_ok = false;
    std::vector<std::string> validation_adjustments;
    std::vector<std::string> validation_errors;

    // The log pane.
    std::vector<LogLine> log;
};

class GuiModel {
public:
    // Detects the host, probes backends, and lists filters. Safe to call again
    // to re-detect. Returns a Status so a front end can surface a hard failure.
    Status refresh_host();

    // Appends a message to the log pane.
    void log(std::string text, bool is_error = false);
    void clear_log() { state_.log.clear(); }

    // Validates the current configuration against the host, storing the
    // effective config and the adjustment/error lists in the state.
    Status validate();

    // Persists the active configuration to the default per-user path.
    Status save_config();

    // Loads and validates the configuration from the default per-user path.
    Status load_config();

    // Turns a filter on or off in the editing config, creating a default entry
    // the first time it is enabled. `name` is the registered filter name; an
    // unknown name is rejected. These are the toggles the Configurator shows.
    Status set_filter_enabled(const std::string& name, bool enabled);

    // Sets a filter's content scope ("any" / "game" / "system"). Unknown values
    // are rejected so the config never holds a scope the chain cannot parse.
    Status set_filter_scope(const std::string& name, const std::string& scope);

    // Sets a filter's intensity (clamped to 0..1). The filter must already be
    // present in the config (i.e. enabled at least once).
    Status set_filter_intensity(const std::string& name, f32 intensity);

    // Mirrors config.filters into state().filters so a front end can draw the
    // per-filter checkbox and scope without walking the config. Called after
    // any change to the filter list.
    void sync_filter_choices();

    // Runs `frames` frames at the configured size through the render pipeline
    // with the enabled filters, and logs the measured timings. Uses the same
    // renderer as the CLI; the numbers are measured, never invented.
    Status render_frames(u32 width, u32 height, u32 frames);

    GuiState& state() { return state_; }
    const GuiState& state() const { return state_; }

    // Maps the configured backend name to a backend kind. "auto" and anything
    // unknown resolve to the CPU backend, the only one guaranteed present.
    backends::BackendKind requested_backend_kind() const;

private:
    GuiState state_{};
};

}  // namespace lx::gui
