// LoucuraXXXX GPU - X11 GUI front end.
//
// A native X11 window drawn directly with Xlib (no widget toolkit dependency).
// It shows the detected host and the configuration, and drives the same
// GuiModel actions through single-key shortcuts. All behaviour is in GuiModel;
// this file only draws text and maps keys to actions.
//
// Keys:
//   r  refresh host        v  validate configuration
//   Enter  render a frame  s  save configuration   l  load configuration
//   q / Esc  quit
#include "gui/frontend.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <sstream>
#include <string>
#include <vector>

#include "core/version.hpp"

namespace lx::gui {
namespace {

// Builds the lines the window displays. Kept as plain strings so the same view
// is easy to reason about and matches the headless output.
std::vector<std::string> build_lines(const GuiModel& model) {
    const GuiState& s = model.state();
    std::vector<std::string> lines;
    lines.push_back(lx::version_string());
    lines.push_back("CPU: " + s.cpu_model);
    lines.push_back("  cores: " + std::to_string(s.logical_cores) + " logical, " +
                    std::to_string(s.physical_cores) + " physical");
    lines.push_back("OS: " + s.os_summary);
    lines.push_back("Memory: " + std::to_string(s.total_memory_bytes / kMiB) + " MiB total, " +
                    std::to_string(s.available_memory_bytes / kMiB) + " MiB available (" +
                    s.memory_topology + ")");
    lines.push_back("GPUs: " + std::to_string(s.gpus.size()));
    for (const auto& gpu : s.gpus) lines.push_back("  - " + gpu);
    lines.push_back("Backends: " + std::to_string(s.backends.size()));
    for (const auto& b : s.backends) {
        lines.push_back("  - " + b.name + " [" + b.kind +
                        (b.is_hardware ? ", hardware" : ", software") + "]");
    }
    lines.push_back("Filters: " + std::to_string(s.filters.size()));
    for (std::size_t i = 0; i < s.filters.size(); ++i) {
        const auto& f = s.filters[i];
        lines.push_back(std::string(i == s.selected_filter ? " >" : "  ") + " [" +
                        (f.enabled ? "x" : " ") + "] " + f.name + " scope=" + f.scope);
    }
    lines.push_back("Safe limits: RAM <= " + std::to_string(s.max_ram_bytes / kMiB) +
                    " MiB, cores <= " + std::to_string(s.max_cpu_cores));
    lines.push_back("");
    lines.push_back("Config: RAM=" + std::to_string(s.config.reserved_ram_bytes / kMiB) +
                    " MiB cores=" + std::to_string(s.config.reserved_cpu_cores) +
                    " backend=" + s.config.preferred_backend);
    lines.push_back("");
    lines.push_back("[r]efresh  [v]alidate  [Enter]render  [s]ave  [l]oad  [q]uit");
    lines.push_back("[Tab]next filter  [space]toggle  [a]ny/game/system scope  [-/+]intensity");
    lines.push_back("---- log ----");
    const std::size_t start = s.log.size() > 12 ? s.log.size() - 12 : 0;
    for (std::size_t i = start; i < s.log.size(); ++i) {
        lines.push_back(std::string(s.log[i].is_error ? "[err] " : "  ") + s.log[i].text);
    }
    return lines;
}

void redraw(Display* display, Window window, GC gc, const GuiModel& model) {
    const std::vector<std::string> lines = build_lines(model);
    XClearWindow(display, window);
    int y = 20;
    for (const auto& line : lines) {
        XDrawString(display, window, gc, 12, y, line.c_str(), static_cast<int>(line.size()));
        y += 16;
    }
}

}  // namespace

int run_frontend(GuiModel& model) {
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        // No X server (e.g. a headless CI box). Fall back rather than pretend a
        // window was shown.
        return run_headless(model, 0, nullptr);
    }
    const int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(display, RootWindow(display, screen), 40, 40, 720, 560, 1,
                                        BlackPixel(display, screen),
                                        WhitePixel(display, screen));
    XStoreName(display, window, "LoucuraXXXX GPU");
    XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask);
    XMapWindow(display, window);

    GC gc = XCreateGC(display, window, 0, nullptr);
    XSetForeground(display, gc, BlackPixel(display, screen));

    model.refresh_host();

    bool running = true;
    while (running) {
        XEvent event;
        XNextEvent(display, &event);
        switch (event.type) {
            case Expose:
                redraw(display, window, gc, model);
                break;
            case KeyPress: {
                const KeySym key = XLookupKeysym(&event.xkey, 0);
                switch (key) {
                    case XK_q:
                    case XK_Escape:
                        running = false;
                        break;
                    case XK_r:
                        model.refresh_host();
                        break;
                    case XK_v:
                        model.validate();
                        break;
                    case XK_Return:
                        model.render_frames(160, 120, 3);
                        break;
                    case XK_s:
                        model.save_config();
                        break;
                    case XK_l:
                        model.load_config();
                        break;
                    case XK_Tab: {
                        GuiState& s = model.state();
                        if (!s.filters.empty()) {
                            s.selected_filter = (s.selected_filter + 1) % s.filters.size();
                        }
                        break;
                    }
                    case XK_space:
                    case XK_f: {
                        GuiState& s = model.state();
                        if (s.selected_filter < s.filters.size()) {
                            const FilterChoice& f = s.filters[s.selected_filter];
                            model.set_filter_enabled(f.name, !f.enabled);
                        }
                        break;
                    }
                    case XK_a: {
                        // Cycle the selected filter's scope any -> game -> system.
                        GuiState& s = model.state();
                        if (s.selected_filter < s.filters.size()) {
                            const std::string& scope = s.filters[s.selected_filter].scope;
                            const std::string next =
                                scope == "any" ? "game" : (scope == "game" ? "system" : "any");
                            model.set_filter_scope(s.filters[s.selected_filter].name, next);
                        }
                        break;
                    }
                    case XK_minus: {
                        GuiState& s = model.state();
                        if (s.selected_filter < s.filters.size()) {
                            const FilterChoice& f = s.filters[s.selected_filter];
                            model.set_filter_intensity(f.name, f.default_intensity - 0.1f);
                        }
                        break;
                    }
                    case XK_plus:
                    case XK_equal: {
                        GuiState& s = model.state();
                        if (s.selected_filter < s.filters.size()) {
                            const FilterChoice& f = s.filters[s.selected_filter];
                            model.set_filter_intensity(f.name, f.default_intensity + 0.1f);
                        }
                        break;
                    }
                    default:
                        break;
                }
                redraw(display, window, gc, model);
                break;
            }
            case DestroyNotify:
                running = false;
                break;
            default:
                break;
        }
    }

    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}

}  // namespace lx::gui
