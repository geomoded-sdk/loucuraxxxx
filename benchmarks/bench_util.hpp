// LoucuraXXXX GPU - benchmark utilities.
//
// Benchmarks must be reproducible: every result records the workload, the
// configuration, and a summary that can be compared run to run. The harness
// never prints a number it did not measure.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "core/types.hpp"

namespace lx::benchmarks {

using Clock = std::chrono::steady_clock;

inline u64 now_micros() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
            .count());
}

// Runs `body` `iterations` times and returns the per-iteration wall times in
// microseconds. A warmup pass is run first so the first-touch cost of buffers
// does not pollute the measurement.
template <typename Fn>
std::vector<u64> measure(int iterations, int warmup, Fn&& body) {
    for (int i = 0; i < warmup; ++i) body();
    std::vector<u64> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    for (int i = 0; i < iterations; ++i) {
        const u64 start = now_micros();
        body();
        samples.push_back(now_micros() - start);
    }
    return samples;
}

struct Summary {
    u64 min_us = 0;
    u64 median_us = 0;
    u64 mean_us = 0;
    u64 p95_us = 0;
    u64 max_us = 0;
};

inline Summary summarize(std::vector<u64> samples) {
    Summary summary;
    if (samples.empty()) return summary;
    std::sort(samples.begin(), samples.end());
    summary.min_us = samples.front();
    summary.max_us = samples.back();
    summary.median_us = samples[samples.size() / 2];
    summary.p95_us = samples[static_cast<std::size_t>(samples.size() * 0.95)];
    const u64 total = std::accumulate(samples.begin(), samples.end(), u64{0});
    summary.mean_us = total / samples.size();
    return summary;
}

inline void print_summary(const std::string& label, const Summary& summary, u64 work_items) {
    // Wide enough for the longest label the harness emits, which is a backend
    // row carrying the software-rasterizer marker.
    std::printf("%-52s min=%7llu median=%7llu mean=%7llu p95=%7llu max=%7llu us",
                label.c_str(),
                static_cast<unsigned long long>(summary.min_us),
                static_cast<unsigned long long>(summary.median_us),
                static_cast<unsigned long long>(summary.mean_us),
                static_cast<unsigned long long>(summary.p95_us),
                static_cast<unsigned long long>(summary.max_us));
    if (work_items > 0 && summary.median_us > 0) {
        const double throughput =
            static_cast<double>(work_items) / (static_cast<double>(summary.median_us) / 1e6);
        std::printf("  (%.2f M items/s)", throughput / 1e6);
    }
    std::printf("\n");
}

}  // namespace lx::benchmarks
