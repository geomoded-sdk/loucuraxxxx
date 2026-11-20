#include "renderer/frame_stats.hpp"

#include <algorithm>
#include <cmath>

namespace lx::renderer {

void StatsWindow::push(const FrameStats& stats) {
    samples_.push_back(stats);
    ++total_frames_;
    if (samples_.size() > capacity_) {
        samples_.erase(samples_.begin());
    }
}

double StatsWindow::average_fps() const {
    if (samples_.empty()) return 0.0;
    u64 total = 0;
    for (const auto& s : samples_) {
        total += s.cpu_time_nanos + s.gpu_time_nanos;
    }
    // A zero total here would mean every frame measured as no time at all, which
    // is not a frame rate of 0 -- it is no measurement. Returning 0.0 would print
    // "0 fps" and read as an extremely slow renderer.
    if (total == 0) return 0.0;
    const double seconds = static_cast<double>(total) / 1.0e9;
    return static_cast<double>(samples_.size()) / seconds;
}

double StatsWindow::average_frame_ms() const {
    if (samples_.empty()) return 0.0;
    u64 total = 0;
    for (const auto& s : samples_) {
        total += s.cpu_time_nanos + s.gpu_time_nanos;
    }
    return static_cast<double>(total) / static_cast<double>(samples_.size()) / 1.0e6;
}

u64 StatsWindow::percentile_frame_nanos(double percentile) const {
    if (samples_.empty()) return 0;
    std::vector<u64> times;
    times.reserve(samples_.size());
    for (const auto& s : samples_) times.push_back(s.cpu_time_nanos + s.gpu_time_nanos);
    std::sort(times.begin(), times.end());
    const double clamped = std::clamp(percentile, 0.0, 1.0);
    std::size_t index = static_cast<std::size_t>(clamped * (times.size() - 1));
    return times[index];
}

u64 StatsWindow::percentile_frame_micros(double percentile) const {
    return percentile_frame_nanos(percentile) / 1000;
}

u64 StatsWindow::min_frame_nanos() const {
    if (samples_.empty()) return 0;
    u64 value = UINT64_MAX;
    for (const auto& s : samples_) value = std::min(value, s.cpu_time_nanos + s.gpu_time_nanos);
    return value;
}

u64 StatsWindow::max_frame_nanos() const {
    u64 value = 0;
    for (const auto& s : samples_) value = std::max(value, s.cpu_time_nanos + s.gpu_time_nanos);
    return value;
}

u64 StatsWindow::min_frame_micros() const { return min_frame_nanos() / 1000; }

u64 StatsWindow::max_frame_micros() const { return max_frame_nanos() / 1000; }

void StatsWindow::reset() {
    samples_.clear();
    total_frames_ = 0;
}

}  // namespace lx::renderer
