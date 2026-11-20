// LoucuraXXXX GPU - graphical front end entry point.
//
// Usage:
//   lx_gui                 open the window (when a toolkit is available)
//   lx_gui --headless      run the model and print results, no window
//   lx_gui --self-test     run the model end to end and exit non-zero on failure
//
// The headless and self-test modes exist so the GUI's behaviour can be verified
// on machines with no display (CI, cross-compiled Windows under Wine) without
// pretending a window was shown.
#include <cstring>
#include <iostream>
#include <string>

#include "core/version.hpp"
#include "gui/frontend.hpp"
#include "gui/gui_model.hpp"

namespace {

void print_usage() {
    std::cout << lx::version_string() << " - graphical front end\n\n"
              << "Usage: lx_gui [--headless] [--self-test] [--frames N]\n"
              << "  (no flags)   open the window when a toolkit is available\n"
              << "  --headless   detect, validate and print, without a window\n"
              << "  --self-test  exercise the model and exit non-zero on failure\n";
}

}  // namespace

int main(int argc, char** argv) {
    lx::gui::GuiModel model;

    bool headless = false;
    bool self_test = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--headless") headless = true;
        if (arg == "--self-test") self_test = true;
    }

    if (self_test) {
        lx::gui::GuiModel probe;
        if (!probe.refresh_host().ok()) {
            std::cerr << "self-test: host detection failed\n";
            return 1;
        }
        if (probe.state().backends.empty()) {
            std::cerr << "self-test: no backends available\n";
            return 1;
        }
        if (!probe.validate().ok()) {
            std::cerr << "self-test: default configuration failed validation\n";
            return 1;
        }
        if (!probe.render_frames(160, 120, 2).ok()) {
            std::cerr << "self-test: render failed\n";
            return 1;
        }
        std::cout << "self-test OK: " << probe.state().backends.size() << " backends, "
                  << probe.state().filters.size() << " filters\n";
        return 0;
    }

#if LX_GUI_HAS_WINDOWING
    if (!headless) {
        return lx::gui::run_frontend(model);
    }
#else
    // No windowing toolkit was compiled in: --headless is implied.
    (void)headless;
#endif
    return lx::gui::run_headless(model, argc, argv);
}
