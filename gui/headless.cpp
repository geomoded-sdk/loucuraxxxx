#include "gui/frontend.hpp"

#include <iostream>
#include <string>

namespace lx::gui {

namespace {

// Renders the model's state to stdout. Kept separate so both the headless mode
// and the self-test print the same, honest view of what was detected.
void print_state(const GuiModel& model) {
    const GuiState& s = model.state();
    std::cout << "CPU: " << s.cpu_model << "\n";
    std::cout << "  cores: " << s.logical_cores << " logical, " << s.physical_cores
              << " physical";
    if (s.hybrid_cpu) {
        std::cout << " (" << s.performance_cores << " performance, " << s.efficiency_cores
                  << " efficiency)";
    }
    std::cout << "\n";
    std::cout << "OS: " << s.os_summary << "\n";
    std::cout << "Memory: " << (s.total_memory_bytes / kMiB) << " MiB total, "
              << (s.available_memory_bytes / kMiB) << " MiB available ("
              << s.memory_topology << ")\n";
    std::cout << "GPUs:\n";
    if (s.gpus.empty()) {
        std::cout << "  (none)\n";
    }
    for (const auto& gpu : s.gpus) std::cout << "  - " << gpu << "\n";
    std::cout << "Backends:\n";
    for (const auto& b : s.backends) {
        std::cout << "  - " << b.name << " [" << b.kind
                  << (b.is_hardware ? ", hardware" : ", software") << "]\n";
    }
    std::cout << "Filters:\n";
    for (const auto& f : s.filters) {
        std::cout << "  - " << f.name << ": " << f.description << "\n";
    }
    std::cout << "Safe limits: RAM <= " << (s.max_ram_bytes / kMiB) << " MiB, cores <= "
              << s.max_cpu_cores << "\n";
}

}  // namespace

int run_headless(GuiModel& model, int /*argc*/, char** /*argv*/) {
    if (!model.refresh_host().ok()) {
        std::cerr << "headless: host detection failed\n";
        return 1;
    }
    print_state(model);

    if (!model.validate().ok()) {
        std::cerr << "headless: configuration validation failed\n";
        return 1;
    }
    // A short render proves the pipeline works end to end without a window.
    if (!model.render_frames(160, 120, 2).ok()) {
        std::cerr << "headless: render failed\n";
        return 1;
    }

    std::cout << "\nLog:\n";
    for (const auto& line : model.state().log) {
        std::cout << (line.is_error ? "  [err] " : "  ") << line.text << "\n";
    }
    return 0;
}

}  // namespace lx::gui
