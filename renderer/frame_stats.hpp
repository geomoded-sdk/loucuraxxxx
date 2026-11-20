// LoucuraXXXX GPU - render timing and throughput statistics.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/types.hpp"

namespace lx::renderer {

// Per-frame timing, collected from a monotonic clock. These are measurements,
// not estimates: nothing here is fabricated.
struct FrameStats {
    u64 frame_index = 0;
    // Nanoseconds are the stored unit because a small frame can finish in under
    // a microsecond. Storing microseconds truncated those to zero, and the FPS
    // average then reported 0 for a fast frame. The *_micros members are derived
    // views for display and are never the source of truth.
    u64 cpu_time_nanos = 0;     // time to build and submit the frame
    u64 gpu_time_nanos = 0;     // time the backend reported, 0 when unavailable
    u64 filter_time_nanos = 0;
    u64 present_time_nanos = 0;
    u64 bytes_uploaded = 0;
    u64 bytes_downloaded = 0;

    u64 cpu_time_micros() const { return cpu_time_nanos / 1000; }
    u64 gpu_time_micros() const { return gpu_time_nanos / 1000; }
    u64 filter_time_micros() const { return filter_time_nanos / 1000; }
    u64 present_time_micros() const { return present_time_nanos / 1000; }
};

// A rolling window used to compute smoothed FPS and frame-time percentiles.
class StatsWindow {
public:
    explicit StatsWindow(std::size_t capacity = 120) : capacity_(capacity) {}

    void push(const FrameStats& stats);

    double average_fps() const;
    double average_frame_ms() const;
    // Percentiles in nanoseconds, plus microsecond views for display. A p99 of a
    // sub-microsecond frame is not zero, and callers that report it must see that.
    u64 percentile_frame_nanos(double percentile) const;  // 0..1
    u64 min_frame_nanos() const;
    u64 max_frame_nanos() const;
    u64 percentile_frame_micros(double percentile) const;
    u64 min_frame_micros() const;
    u64 max_frame_micros() const;
    std::size_t sample_count() const { return samples_.size(); }
    u64 total_frames() const { return total_frames_; }

    void reset();

private:
    std::size_t capacity_;
    std::vector<FrameStats> samples_;
    u64 total_frames_ = 0;
};

}  // namespace lx::renderer
