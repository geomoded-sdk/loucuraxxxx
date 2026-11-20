#include "scheduler/thread_pool.hpp"

#include <algorithm>
#include <chrono>

#include "core/clock.hpp"
#include "core/log.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace lx::scheduler {

u32 recommend_performance_threads(u32 total_threads, u32 performance_cores,
                                  u32 efficiency_cores) {
    if (total_threads == 0) return 0;
    if (performance_cores == 0 || efficiency_cores == 0) {
        // Uniform CPU: every worker is the same class.
        return total_threads;
    }
    const u32 total_cores = performance_cores + efficiency_cores;
    // Proportional split, rounded up so performance work is not starved.
    u32 perf = static_cast<u32>(
        (static_cast<double>(total_threads) * performance_cores + total_cores - 1) /
        total_cores);
    perf = std::max(1u, std::min(perf, total_threads));
    // Keep at least one efficiency worker when there is more than one worker.
    if (total_threads > 1 && perf == total_threads) perf = total_threads - 1;
    return perf;
}

ThreadPoolConfig thread_pool_config_for(u32 reserved_cores, u32 performance_cores,
                                        u32 efficiency_cores, std::string name) {
    ThreadPoolConfig config;
    config.name = std::move(name);
    config.thread_count = reserved_cores;
    // On a hybrid CPU the split keeps latency-sensitive filter work on the
    // performance cores; on a uniform CPU every worker is the same class.
    config.performance_threads =
        recommend_performance_threads(config.thread_count, performance_cores,
                                      efficiency_cores);
    return config;
}

ThreadPool::ThreadPool(const ThreadPoolConfig& config) : config_(config) {
    if (config_.thread_count == 0) {
        const u32 hw = std::max(1u, std::thread::hardware_concurrency());
        config_.thread_count = hw > 1 ? hw - 1 : 1;
    }
}

ThreadPool::~ThreadPool() { stop(); }

Status ThreadPool::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return Status::success();

    stop_requested_.store(false);
    pending_.store(0);
    completed_.store(0);
    next_lane_.store(0);
    // Clamp the performance split so it can never exceed the worker count.
    const u32 perf = std::min(config_.performance_threads, config_.thread_count);
    workers_.reserve(config_.thread_count);
    for (u32 i = 0; i < config_.thread_count; ++i) {
        auto worker = std::make_unique<Worker>();
        worker->index = i;
        // Leading workers are performance-class; the rest efficiency-class.
        worker->core_class = i < perf ? CoreClass::Performance : CoreClass::Efficiency;
        workers_.push_back(std::move(worker));
    }
    for (u32 i = 0; i < config_.thread_count; ++i) {
        workers_[i]->thread = std::thread([this, i] { worker_loop(i); });
    }
    started_nanos_.store(now_nanos(), std::memory_order_relaxed);
    LX_DEBUG << "thread pool '" << config_.name << "' started with "
             << config_.thread_count << " workers";
    return Status::success();
}

void ThreadPool::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    stop_requested_.store(true, std::memory_order_release);
    // Wake every worker so it observes the stop flag instead of sleeping out
    // its poll interval. Each worker is notified under its own mutex.
    for (auto& worker : workers_) {
        {
            std::lock_guard<std::mutex> lock(worker->mutex);
            worker->cv.notify_all();
        }
    }
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) worker->thread.join();
    }
    workers_.clear();
    pending_.store(0);
    running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        idle_cv_.notify_all();
    }
    LX_DEBUG << "thread pool '" << config_.name << "' stopped";
}

Status ThreadPool::submit(Task task, u32 lane) {
    if (!task) {
        return Status::error(ErrorCode::InvalidArgument, "null task submitted");
    }
    if (!running_.load(std::memory_order_acquire)) {
        return Status::error(ErrorCode::ResourceBusy, "thread pool is not running");
    }
    const u32 count = static_cast<u32>(workers_.size());
    if (count == 0) {
        return Status::error(ErrorCode::ResourceBusy, "thread pool has no workers");
    }
    const bool explicit_lane = lane != kAnyLane;
    const u32 index = explicit_lane
                          ? (lane % count)
                          : (next_lane_.fetch_add(1, std::memory_order_relaxed) % count);

    pending_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(workers_[index]->mutex);
        workers_[index]->queue.push_back(Entry{std::move(task), explicit_lane});
        // Notify under the worker's own lock: the worker waits on the same
        // mutex, so this cannot miss the wakeup.
        workers_[index]->cv.notify_one();
    }
    return Status::success();
}

Status ThreadPool::submit_on(Task task, CoreClass core_class) {
    if (!task) {
        return Status::error(ErrorCode::InvalidArgument, "null task submitted");
    }
    if (!running_.load(std::memory_order_acquire)) {
        return Status::error(ErrorCode::ResourceBusy, "thread pool is not running");
    }
    const u32 count = static_cast<u32>(workers_.size());
    if (count == 0) {
        return Status::error(ErrorCode::ResourceBusy, "thread pool has no workers");
    }
    // Pick a worker of the requested class, rotating to spread load. If no
    // worker matches the class (uniform pool, or only one class present), fall
    // back to the ordinary rotating submission so work is never dropped.
    u32 index = kAnyLane;
    for (u32 attempt = 0; attempt < count; ++attempt) {
        const u32 candidate = next_lane_.fetch_add(1, std::memory_order_relaxed) % count;
        if (workers_[candidate]->core_class == core_class) {
            index = candidate;
            break;
        }
    }
    if (index == kAnyLane) {
        index = next_lane_.fetch_add(1, std::memory_order_relaxed) % count;
    }

    pending_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(workers_[index]->mutex);
        workers_[index]->queue.push_back(Entry{std::move(task), false});
        workers_[index]->cv.notify_one();
    }
    return Status::success();
}

u32 ThreadPool::performance_worker_count() const {
    u32 n = 0;
    for (const auto& w : workers_) {
        if (w->core_class == CoreClass::Performance) ++n;
    }
    return n;
}

u32 ThreadPool::efficiency_worker_count() const {
    return static_cast<u32>(workers_.size()) - performance_worker_count();
}

CoreClass ThreadPool::worker_class(u32 index) const {
    if (index < workers_.size()) return workers_[index]->core_class;
    return CoreClass::Performance;
}

bool ThreadPool::try_run_local(u32 index) {
    Entry entry;
    {
        std::lock_guard<std::mutex> lock(workers_[index]->mutex);
        if (workers_[index]->queue.empty()) return false;
        entry = std::move(workers_[index]->queue.front());
        workers_[index]->queue.pop_front();
    }
    run_entry(index, std::move(entry));
    completed_.fetch_add(1, std::memory_order_relaxed);
    notify_task_finished();
    return true;
}

// Decrements the pending count and wakes anyone blocked in wait_idle() once the
// pool drains. The notify happens under idle_mutex_ so it cannot race the
// waiter's predicate check and lose the wakeup.
void ThreadPool::notify_task_finished() {
    if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        idle_cv_.notify_all();
    }
}

bool ThreadPool::try_steal(u32 index, Task& out) {
    if (!config_.allow_stealing) return false;
    const u32 count = static_cast<u32>(workers_.size());
    for (u32 offset = 1; offset < count; ++offset) {
        const u32 victim = (index + offset) % count;
        std::lock_guard<std::mutex> lock(workers_[victim]->mutex);
        if (workers_[victim]->queue.empty()) continue;
        // Never steal a lane-bound task: it belongs to an ordered sequence.
        // Skip to the next victim rather than stopping, since the back of the
        // queue is what a steal would take.
        if (workers_[victim]->queue.back().lane_bound) continue;
        // Steal from the back: the owner works the front, minimising contention.
        out = std::move(workers_[victim]->queue.back().task);
        workers_[victim]->queue.pop_back();
        return true;
    }
    return false;
}

void ThreadPool::pin_current_thread(u32 index) const {
    if (index >= config_.pin_to_cpus.size()) return;
    const u32 cpu = config_.pin_to_cpus[index];
#if defined(_WIN32)
    // A processor group holds at most 64 logical CPUs; a shift by >= 64 is
    // undefined and the mask cannot express it anyway. Skip rather than misbehave.
    if (cpu >= 64) return;
    ::SetThreadAffinityMask(::GetCurrentThread(), static_cast<DWORD_PTR>(1ull << cpu));
#elif defined(__linux__)
    // CPU_SET past CPU_SETSIZE is out of bounds on the cpu_set_t.
    if (cpu >= CPU_SETSIZE) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
#else
    (void)cpu;
#endif
}

void ThreadPool::apply_core_class_hint(u32 index) const {
#if defined(__APPLE__)
    // On Apple silicon the scheduler places a thread on a performance or
    // efficiency core according to its QoS class. USER_INTERACTIVE/ USER_INITIATED
    // are steered to performance cores; UTILITY/BACKGROUND to efficiency cores.
    // This is the supported way to express the intent; affinity masks are not.
    const CoreClass cls = index < workers_.size() ? workers_[index]->core_class
                                                   : CoreClass::Performance;
    const qos_class_t qos =
        cls == CoreClass::Performance ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY;
    ::pthread_set_qos_class_self_np(qos, 0);
#else
    (void)index;
#endif
}

// Runs one task on `index` and accounts for its time. The worker is marked active
// before the task starts, so sampling utilization while a long task runs sees the
// time already spent instead of nothing until the task returns.
void ThreadPool::run_task(u32 index, Task task) {
    Worker& worker = *workers_[index];
    const u64 start = now_nanos();
    worker.active_since.store(start, std::memory_order_relaxed);
    task();
    const u64 elapsed = now_nanos() - start;
    worker.active_since.store(0, std::memory_order_relaxed);
    worker.busy_nanos.fetch_add(elapsed, std::memory_order_relaxed);
    busy_nanos_.fetch_add(elapsed, std::memory_order_relaxed);
}

void ThreadPool::run_entry(u32 index, Entry entry) {
    run_task(index, std::move(entry.task));
}

u64 ThreadPool::busy_nanoseconds() const {
    u64 total = busy_nanos_.load(std::memory_order_relaxed);
    // Include tasks that are running right now: a worker mid-task has already
    // spent real time on it.
    const u64 now = now_nanos();
    for (const auto& worker : workers_) {
        const u64 since = worker->active_since.load(std::memory_order_relaxed);
        if (since != 0 && now > since) total += now - since;
    }
    return total;
}

std::size_t ThreadPool::queued() const {
    std::size_t total = 0;
    for (const auto& worker : workers_) {
        std::lock_guard<std::mutex> lock(worker->mutex);
        total += worker->queue.size();
    }
    return total;
}

u64 ThreadPool::uptime_nanoseconds() const {
    const u64 started = started_nanos_.load(std::memory_order_relaxed);
    if (started == 0) return 0;
    const u64 now = now_nanos();
    return now > started ? now - started : 0;
}

double ThreadPool::utilization() const {
    const u64 workers = thread_count();
    const u64 uptime = uptime_nanoseconds();
    if (workers == 0 || uptime == 0) return 0.0;
    // busy_nanos_ is summed across workers, so the denominator is the total
    // worker-time available in the same window. The result can exceed 1.0 only
    // through clock granularity on a very short window, so it is clamped.
    const double available = static_cast<double>(uptime) * static_cast<double>(workers);
    const double busy = static_cast<double>(busy_nanoseconds());
    const double ratio = busy / available;
    return ratio > 1.0 ? 1.0 : ratio;
}

void ThreadPool::worker_loop(u32 index) {
    apply_core_class_hint(index);
    pin_current_thread(index);
    while (!stop_requested_.load(std::memory_order_acquire)) {
        if (try_run_local(index)) continue;

        Task stolen;
        if (try_steal(index, stolen)) {
            run_task(index, std::move(stolen));
            completed_.fetch_add(1, std::memory_order_relaxed);
            notify_task_finished();
            continue;
        }

        // Nothing to do: wait briefly on our own queue, then re-check. The wait
        // is bounded because work may arrive on another worker's queue and we
        // want a chance to steal it.
        std::unique_lock<std::mutex> lock(workers_[index]->mutex);
        workers_[index]->cv.wait_for(lock, std::chrono::milliseconds(2), [this, index] {
            return stop_requested_.load(std::memory_order_acquire) ||
                   !workers_[index]->queue.empty();
        });
    }
}

Status ThreadPool::wait_idle() {
    for (const auto& worker : workers_) {
        if (worker->thread.get_id() == std::this_thread::get_id()) {
            return Status::error(ErrorCode::InvalidArgument,
                                 "wait_idle must not be called from a worker thread");
        }
    }
    std::unique_lock<std::mutex> lock(idle_mutex_);
    // The predicate load must be acquire. The final task decrements pending_ to
    // zero *before* it takes idle_mutex_, so this thread can acquire the mutex,
    // observe the zero and return without ever synchronising with that task
    // through the mutex. Reading the zero with an acquire load synchronises with
    // the completing task's release (which in turn chains to every earlier
    // completion), so writes made by finished tasks are visible to the caller.
    idle_cv_.wait(lock, [this] {
        return pending_.load(std::memory_order_acquire) == 0;
    });
    return Status::success();
}

std::size_t ThreadPool::pending() const {
    return pending_.load(std::memory_order_relaxed);
}

}  // namespace lx::scheduler
