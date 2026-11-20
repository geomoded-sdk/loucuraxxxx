// Tests for the GUI model: the shared, display-free logic behind the front
// ends. Everything the window can do must be exercisable without a window.
#include "gui/gui_model.hpp"

#include "backends/backend_registry.hpp"
#include "test_framework.hpp"

using namespace lx;

LX_TEST(gui, refresh_host_populates_real_fields) {
    gui::GuiModel model;
    LX_CHECK_OK(model.refresh_host());
    const gui::GuiState& s = model.state();
    // A real machine always reports at least one logical core and some memory.
    LX_CHECK(s.logical_cores >= 1);
    LX_CHECK(s.total_memory_bytes > 0);
    LX_CHECK(!s.os_summary.empty());
    LX_CHECK(!s.backends.empty());  // the CPU backend is always present
    LX_CHECK(!s.filters.empty());   // built-in filters are always registered
}

LX_TEST(gui, safe_limits_are_positive_and_clamp_config) {
    gui::GuiModel model;
    LX_CHECK_OK(model.refresh_host());
    LX_CHECK(model.state().max_ram_bytes > 0);
    LX_CHECK(model.state().max_cpu_cores >= 1);
    // The editing config must never exceed the safe limits after a refresh.
    LX_CHECK(model.state().config.reserved_ram_bytes <= model.state().max_ram_bytes);
    LX_CHECK(model.state().config.reserved_cpu_cores <= model.state().max_cpu_cores);
}

LX_TEST(gui, validate_adopts_effective_config) {
    gui::GuiModel model;
    LX_CHECK_OK(model.refresh_host());
    // Ask for an absurd amount; validation must clamp rather than fail.
    model.state().config.reserved_ram_bytes = 1024ull * 1024 * kGiB;
    LX_CHECK_OK(model.validate());
    LX_CHECK(model.state().validation_ok);
    LX_CHECK(model.state().config.reserved_ram_bytes <= model.state().max_ram_bytes);
}

LX_TEST(gui, render_produces_frames_and_logs_measured_stats) {
    gui::GuiModel model;
    LX_CHECK_OK(model.refresh_host());
    LX_CHECK_OK(model.validate());
    model.clear_log();
    LX_CHECK_OK(model.render_frames(64, 48, 2));
    // One log line per frame plus the summary line.
    LX_CHECK(model.state().log.size() >= 3);
}

LX_TEST(gui, telemetry_interval_throttles_progress_lines) {
    // telemetry_interval_ms used to be parsed, clamped and saved but never read,
    // so every frame logged a line. With a long interval only the first frame
    // reports progress; the summary still counts every frame.
    gui::GuiModel model;
    LX_CHECK_OK(model.refresh_host());
    LX_CHECK_OK(model.validate());

    // Every frame reports: the baseline the throttle is measured against.
    model.state().config.telemetry_interval_ms = 0;
    model.clear_log();
    LX_CHECK_OK(model.render_frames(32, 32, 4));
    const std::size_t per_frame_lines = model.state().log.size();
    LX_CHECK(per_frame_lines >= 5);  // 4 frames + summary

    // A one-hour interval admits the first frame only.
    model.state().config.telemetry_interval_ms = 60 * 60 * 1000;
    model.clear_log();
    LX_CHECK_OK(model.render_frames(32, 32, 4));
    const std::size_t throttled_lines = model.state().log.size();
    LX_CHECK(throttled_lines < per_frame_lines);
    // Still one progress line for the first frame, plus the summary.
    LX_CHECK(throttled_lines >= 2);
}

LX_TEST(gui, render_rejects_zero_dimensions) {
    gui::GuiModel model;
    LX_CHECK_OK(model.refresh_host());
    LX_CHECK(!model.render_frames(0, 48, 1).ok());
    LX_CHECK(!model.render_frames(64, 0, 1).ok());
    LX_CHECK(!model.render_frames(64, 48, 0).ok());
}

LX_TEST(gui, requested_backend_kind_maps_names) {
    gui::GuiModel model;
    model.state().config.preferred_backend = "vulkan";
    LX_CHECK(model.requested_backend_kind() == backends::BackendKind::Vulkan);
    model.state().config.preferred_backend = "metal";
    LX_CHECK(model.requested_backend_kind() == backends::BackendKind::Metal);
    model.state().config.preferred_backend = "cpu";
    LX_CHECK(model.requested_backend_kind() == backends::BackendKind::Cpu);
    // "auto" must resolve to whatever the probe actually found, not a hardcoded
    // CPU choice, so the GUI never ignores a working GPU.
    model.state().config.preferred_backend = "auto";
    const auto available = backends::probe_available_backends();
    const auto expected =
        available.empty() ? backends::BackendKind::Cpu : available.front().kind;
    LX_CHECK(model.requested_backend_kind() == expected);
    // An unknown name is not a request for the CPU backend; it is a mistake and
    // resolves to the same probe result rather than silently picking CPU.
    model.state().config.preferred_backend = "nonsense";
    LX_CHECK(model.requested_backend_kind() == expected);
}

LX_TEST(gui, filter_toggles_drive_the_render_chain) {
    gui::GuiModel model;
    LX_CHECK_OK(model.refresh_host());
    LX_CHECK_OK(model.validate());

    // Nothing enabled by default: the choices mirror an empty filter list.
    for (const auto& choice : model.state().filters) LX_CHECK(!choice.enabled);

    // Enabling a registered filter creates a config entry and flips the choice.
    LX_CHECK_OK(model.set_filter_enabled("vignette", true));
    bool found_enabled = false;
    for (const auto& choice : model.state().filters) {
        if (choice.name == "vignette") found_enabled = choice.enabled;
    }
    LX_CHECK(found_enabled);
    LX_CHECK_EQ(model.state().config.filters.size(), static_cast<std::size_t>(1));

    // An unknown filter is rejected, not silently added.
    LX_CHECK(!model.set_filter_enabled("does-not-exist", true).ok());

    // Scope and intensity are settable and validated.
    LX_CHECK_OK(model.set_filter_scope("vignette", "game"));
    LX_CHECK(!model.set_filter_scope("vignette", "nonsense").ok());
    LX_CHECK_OK(model.set_filter_intensity("vignette", 0.5f));
    LX_CHECK(!model.set_filter_intensity("does-not-exist", 0.5f).ok());

    // Disabling keeps the tuning so a re-enable restores it.
    LX_CHECK_OK(model.set_filter_enabled("vignette", false));
    LX_CHECK_OK(model.set_filter_enabled("vignette", true));
    LX_CHECK_EQ(model.state().config.filters.size(), static_cast<std::size_t>(1));
    LX_CHECK_EQ(model.state().config.filters[0].scope, std::string("game"));
    LX_CHECK(model.state().config.filters[0].intensity > 0.49f &&
             model.state().config.filters[0].intensity < 0.51f);
}

LX_TEST(gui, disabled_filter_is_skipped_in_render) {
    gui::GuiModel model;
    LX_CHECK_OK(model.refresh_host());
    model.state().config.filters_enabled = true;
    LX_CHECK_OK(model.set_filter_enabled("vignette", true));
    LX_CHECK_OK(model.set_filter_enabled("vignette", false));
    model.clear_log();
    LX_CHECK_OK(model.render_frames(32, 32, 1));
    // The disabled filter must not appear in the per-filter timing log.
    bool saw_filter_time = false;
    for (const auto& line : model.state().log) {
        if (line.text.find("vignette") != std::string::npos) saw_filter_time = true;
    }
    LX_CHECK(!saw_filter_time);
}

LX_TEST_MAIN("gui")
