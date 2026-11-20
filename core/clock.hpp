// LoucuraXXXX GPU - monotonic clock readings used for all reported timings.
#pragma once

#include "core/types.hpp"

namespace lx {

// Microseconds from a monotonic clock. Suitable for durations, never for dates.
//
// Resolution matters here: a whole 64x64 CPU frame can finish in well under one
// microsecond, and truncating that to 0 makes the FPS average divide by zero and
// report 0 fps for a fast frame. Callers that report performance must therefore
// use now_nanos() and only convert to microseconds for display.
u64 now_micros();

// Nanoseconds from the same monotonic clock, so the two stay comparable.
u64 now_nanos();

}  // namespace lx
