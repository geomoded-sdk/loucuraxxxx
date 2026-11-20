// LoucuraXXXX GPU - GUI front-end interface.
//
// Each platform provides one function with this signature. The front end owns
// only windowing and widget plumbing; all behaviour lives in GuiModel.
#pragma once

#include "gui/gui_model.hpp"

namespace lx::gui {

// Runs the interactive GUI loop and returns a process exit code. Implemented by
// the Win32 and X11 translation units. Only declared when a windowing toolkit
// was compiled in.
int run_frontend(GuiModel& model);

// Runs the model without any window: detects the host, validates the config,
// and optionally renders, printing everything to stdout. Used by CI, by the
// test suite, and as the fallback when no windowing toolkit is available.
int run_headless(GuiModel& model, int argc, char** argv);

}  // namespace lx::gui
