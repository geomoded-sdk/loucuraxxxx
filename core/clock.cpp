#include "core/clock.hpp"

#include <chrono>

namespace lx {

namespace {
u64 steady_nanos() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}
}  // namespace

u64 now_micros() { return steady_nanos() / 1000; }

u64 now_nanos() { return steady_nanos(); }

}  // namespace lx
