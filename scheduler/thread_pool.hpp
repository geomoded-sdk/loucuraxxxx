// LoucuraXXXX GPU - fixed-size thread pool with per-worker queues.
//
// The pool backs the CPU side of the hybrid pipeline. Each worker owns a deque
// and steals from the back of others when its own queue is empty, which keeps
// load balanced without a central lock on the hot path. Threads can be pinned
// to specific logical CPUs so the caller can keep the virtual GPU off the cores
// a game is already using.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

namespace lx::scheduler {

// The kind of core a worker prefers to run on. On a hybrid CPU (Apple silicon,
// Intel P/E, ARM big.LITTLE) latency-sensitive work belongs on performance
// cores and background work on efficiency cores; on a uniform CPU the
// distinction collapses and everything is Performance.
enum class CoreClass { Performance, Efficiency };

inline const char* to_string(CoreClass c) {
    return c == CoreClass::Performance ? "performance" : "efficiency";
}

struct ThreadPoolConfig {
    u32 thread_count = 0;          // 0 => hardware_concurrency() - 1
    bool allow_stealing = true;
    std::vector<u32> pin_to_cpus;  // optional CPU affinity mask per worker
    std::string name = "lx-cpu";

    // Number of leading workers that are performance-class. 0 means every
    // worker is the same class (a uniform CPU, or the caller does not care).
    // When set, submit_on() can steer work to the right kind of core, and on
    // macOS each worker requests the matching scheduler QoS class.
    u32 performance_threads = 0;
};

// Chooses how many of `total_threads` workers should be performance-class to
// match a hybrid CPU, leaving the efficiency cores for background work so the
// foreground application keeps the fast cores. Returns `total_threads` (all
// performance) on a uniform CPU, where the distinction does not exist.
//
// The split is proportional to the core counts but always keeps at least one
// performance worker and one efficiency worker when both classes exist, so a
// tiny pool still has somewhere to put each kind of task.
u32 recommend_performance_threads(u32 total_threads, u32 performance_cores,
                                  u32 efficiency_cores);

// Builds a pool configuration for the `reserved_cores` the user granted the
// virtual GPU, sized to the host's hybrid topology. `performance_cores` and
// `efficiency_cores` come from HardwareInfo; pass 0/0 on a uniform CPU. When
// `reserved_cores` is 0 the pool falls back to the default (hardware
// concurrency minus one), which the ThreadPool applies itself.
ThreadPoolConfig thread_pool_config_for(u32 reserved_cores, u32 performance_cores,
                                        u32 efficiency_cores,
                                        std::string name = "lx-cpu");

class ThreadPool {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(const ThreadPoolConfig& config = {});
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Starts the worker threads. Idempotent: calling twice is a no-op.
    Status start();

    // Signals shutdown and joins all workers. Idempotent.
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }
    u32 thread_count() const { return static_cast<u32>(workers_.size()); }

    // Enqueues a task. Tasks are dispatched to the worker owning `lane`, which
    // lets a caller keep an ordered sequence of work on one cache-hot core.
    // Returns ResourceBusy if the pool is not running.
    Status submit(Task task, u32 lane = kAnyLane);

    // Enqueues a task preferring a core class. On a hybrid CPU, performance
    // work is queued to a performance worker and background work to an
    // efficiency worker; on a uniform pool this is equivalent to submit().
    // The pool never refuses work because a class is busy: it falls back to any
    // worker rather than stalling.
    Status submit_on(Task task, CoreClass core_class);

    // Blocks until every queued task has finished. Safe to call from outside
    // the pool; calling it from a worker would deadlock and is refused.
    Status wait_idle();

    // Tasks submitted but not yet finished, including those now running. This is
    // an in-flight count, not a queue length: use queued() for work that is
    // waiting for a worker.
    std::size_t pending() const;

    // Tasks sitting in worker queues, not yet started. This is the queue depth;
    // a long task occupying every worker leaves it at zero.
    std::size_t queued() const;

    // Total tasks completed since start.
    u64 completed() const { return completed_.load(std::memory_order_relaxed); }

    // Measured worker occupancy in [0,1]: task execution time summed across all
    // workers, divided by (workers * wall time since the pool started). This is a
    // real measurement of the pool's own threads, taken by timing each task, not
    // an inference from queue depth. It reads near 1.0 when every worker is busy
    // and near 0.0 when the pool is idle. It describes the reserved worker
    // threads only; it is not whole-machine CPU utilization.
    double utilization() const;

    // Summed task execution nanoseconds across all workers, including tasks still
    // running. A single long task therefore shows up as it executes rather than
    // only when it finishes.
    u64 busy_nanoseconds() const;

    // Nanoseconds since start() completed; 0 before the pool starts.
    u64 uptime_nanoseconds() const;

    // How many workers are performance-class / efficiency-class.
    u32 performance_worker_count() const;
    u32 efficiency_worker_count() const;

    // The class a given worker runs as. Out-of-range indices report Performance.
    CoreClass worker_class(u32 index) const;

    static constexpr u32 kAnyLane = 0xFFFFFFFFu;

private:
    // A queued item. `lane_bound` records that the task was submitted to an
    // explicit lane, which is an ordered sequence: such a task must not be
    // stolen, or it could run before an earlier task in the same lane.
    struct Entry {
        Task task;
        bool lane_bound = false;
    };

    struct Worker {
        std::deque<Entry> queue;
        mutable std::mutex mutex;
        // Each worker waits on its own condition variable. A condition variable
        // must be paired with exactly one mutex; sharing one across the
        // per-worker mutexes is undefined behaviour and loses wakeups.
        std::condition_variable cv;
        std::thread thread;
        u32 index = 0;
        CoreClass core_class = CoreClass::Performance;
        // Start time of the task this worker is running, or 0 when idle. Per
        // worker so that sampling occupancy does not need a global lock.
        std::atomic<u64> active_since{0};
        std::atomic<u64> busy_nanos{0};  // completed task time on this worker
    };

    void worker_loop(u32 index);
    void run_task(u32 index, Task task);
    void run_entry(u32 index, Entry entry);
    bool try_run_local(u32 index);
    bool try_steal(u32 index, Task& out);
    void notify_task_finished();
    void pin_current_thread(u32 index) const;
    void apply_core_class_hint(u32 index) const;

    ThreadPoolConfig config_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<u64> completed_{0};
    std::atomic<u64> busy_nanos_{0};  // completed task time only
    std::atomic<u64> started_nanos_{0};
    std::atomic<std::size_t> pending_{0};
    std::atomic<u32> next_lane_{0};
    mutable std::mutex idle_mutex_;
    std::condition_variable idle_cv_;
};

}  // namespace lx::scheduler
