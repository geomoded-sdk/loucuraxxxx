// LoucuraXXXX GPU - utilization telemetry.
//
// The manager samples real counters (dispatches, queue depth, bytes moved) and
// exposes them as a snapshot. Nothing here is estimated: each field is either
// measured or left at zero when the underlying value is unavailable.
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>

#include "core/types.hpp"

namespace lx::manager {

struct TelemetrySnapshot {
    u64 timestamp_micros = 0;

    // Scheduling
    u64 passes_dispatched_to_cpu = 0;
    u64 passes_dispatched_to_gpu = 0;
    u64 passes_dispatched_split = 0;
    u64 passes_rejected = 0;

    // Queues
    u64 queue_depth = 0;
    u64 peak_queue_depth = 0;

    // Bytes
    u64 bytes_transferred_to_gpu = 0;
    u64 bytes_transferred_to_cpu = 0;
    u64 bytes_avoided_by_residency = 0;

    // CPU queue pressure (0..1): pending work divided by worker count, clamped.
    // This is NOT CPU utilization and is not named as such. It is high when more
    // work is queued than there are workers, and it reads 0.0 when one long pass
    // occupies every worker with an empty queue -- the opposite of what a
    // utilization figure would say. Compare cpu_worker_utilization below, which
    // measures busy time directly.
    double cpu_queue_pressure = 0.0;
    // Measured occupancy of the reserved worker threads (0..1): task execution
    // time summed over workers divided by worker-time elapsed. Sampled from the
    // pool's own timing, so it is high while workers run tasks regardless of
    // queue depth. It covers the reserved threads only -- not the whole machine,
    // and not the game's threads.
    double cpu_worker_utilization = 0.0;
    // 0 is ambiguous on its own: it means either "no GPU reported" or "the GPU
    // is idle". Consumers must check this flag before showing the number, so an
    // idle GPU is not mistaken for an absent one.
    bool gpu_utilization_available = false;
    double gpu_utilization = 0.0;
    double memory_utilization = 0.0;

    // Budget
    u64 budget_limit_bytes = 0;
    u64 budget_used_bytes = 0;
    u64 budget_peak_bytes = 0;
};

// Thread-safe counters the manager increments as it works.
class Telemetry {
public:
    void record_dispatch(int device_target);  // 0 cpu, 1 gpu, 2 split
    void record_rejection();
    void record_queue_depth(u64 depth);
    void record_transfer(u64 to_gpu_bytes, u64 to_cpu_bytes);
    void record_residency_avoided(u64 bytes);

    // `cpu` is queue pressure, not utilization; see TelemetrySnapshot.
    // `gpu_available` distinguishes "the GPU is idle" (0.0, available) from
    // "nothing measured the GPU" (no backend reports it).
    void set_utilization(double cpu, double gpu, double memory, bool gpu_available = false,
                         double cpu_worker_utilization = 0.0);
    void set_budget(u64 limit, u64 used, u64 peak);

    TelemetrySnapshot snapshot() const;
    void reset();

private:
    std::atomic<u64> cpu_dispatches_{0};
    std::atomic<u64> gpu_dispatches_{0};
    std::atomic<u64> split_dispatches_{0};
    std::atomic<u64> rejections_{0};
    std::atomic<u64> queue_depth_{0};
    std::atomic<u64> peak_queue_depth_{0};
    std::atomic<u64> bytes_to_gpu_{0};
    std::atomic<u64> bytes_to_cpu_{0};
    std::atomic<u64> residency_avoided_{0};
    std::atomic<u64> limit_{0};
    std::atomic<u64> used_{0};
    std::atomic<u64> peak_{0};

    mutable std::mutex utilization_mutex_;
    double cpu_queue_pressure_ = 0.0;
    double cpu_worker_utilization_ = 0.0;
    bool gpu_utilization_available_ = false;
    double gpu_utilization_ = 0.0;
    double memory_utilization_ = 0.0;
};

}  // namespace lx::manager
