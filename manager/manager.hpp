// LoucuraXXXX GPU - manager.
//
// The manager is the coordinator. It owns the CPU worker pool, the work queue,
// and the cost model, and decides for each unit of work whether it runs on the
// CPU workers, on the physical GPU, or split across both. It admits work only
// when there is budget for it, moves data only when a transfer actually pays
// off, and publishes telemetry so the decision can be audited.
//
// The physical GPU is never replaced. Work handed to it goes through the same
// backend interface the rest of the project uses; the manager merely chooses.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "backends/backend.hpp"
#include "configurator/config.hpp"
#include "core/status.hpp"
#include "manager/telemetry.hpp"
#include "manager/work_queue.hpp"
#include "memory/memory_budget.hpp"
#include "scheduler/cost_model.hpp"
#include "scheduler/thread_pool.hpp"

namespace lx::manager {

struct ManagerConfig {
    configurator::Priority priority = configurator::Priority::Normal;
    bool allow_cpu_gpu_split = true;
    u32 max_queue_depth = 256;
    u64 max_in_flight_bytes = 0;   // 0 => no separate ceiling (budget only)
    scheduler::CostConstants cost_constants{};
    std::vector<u32> cpu_affinity; // empty => no pinning

    // Hybrid CPU topology. When set, the CPU pool reserves `performance_workers`
    // workers for latency-sensitive work and the rest for background work, and
    // on macOS each worker requests the matching QoS class so the OS places it
    // on a performance or efficiency core. Leave 0 on a uniform CPU.
    u32 cpu_worker_count = 0;      // 0 => derived from the host
    u32 performance_workers = 0;   // 0 => every worker is performance-class

    // Builds a manager config from a validated user configuration: the reserved
    // cores become the CPU worker count, and the host's hybrid topology decides
    // the performance/efficiency split. This is the single place the reserved
    // cores turn into real worker threads.
    static ManagerConfig from_config(const configurator::Config& config,
                                     u32 performance_cores, u32 efficiency_cores);
};

// What happened to one submitted item.
struct DispatchOutcome {
    u64 id = 0;
    scheduler::DeviceTarget target = scheduler::DeviceTarget::Cpu;
    double estimated_seconds = 0.0;
    u64 bytes_transferred = 0;
    std::string rationale;
};

class Manager {
public:
    Manager() = default;
    ~Manager();

    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    // Binds the manager to a backend and a memory budget, and starts the CPU
    // worker pool. `backend` may be null only when no GPU work will be
    // submitted; the CPU path always works.
    Status initialize(backends::Backend* backend, memory::MemoryBudget* budget,
                      const ManagerConfig& config);

    // Stops the worker pool and releases resources. Idempotent.
    void shutdown();

    bool initialized() const { return initialized_; }

    // Places one item: enqueues it, consults the cost model, and records the
    // decision. Returns the decision so the caller can act on it (the manager
    // does not execute arbitrary caller code on its own threads).
    Result<DispatchOutcome> plan(const WorkItem& item);

    // Takes the next item the manager decided should run on the CPU, or an
    // empty optional when none is ready. The item's memory reservation is
    // released here, since the manager owns the CPU execution path.
    std::optional<DispatchOutcome> next_cpu_task();

    // Reports that a GPU/split dispatch the caller executed has finished, so
    // its memory reservation is released. The manager does not run GPU work on
    // its own threads, so the caller must call this once the backend is done
    // with the item. Returns OutOfRange when the id is unknown or already
    // completed.
    Status complete_dispatch(u64 id);

    // Bytes currently reserved by admitted but not-yet-completed work.
    u64 in_flight_bytes() const;

    // Reports whether a GPU is available to receive work.
    bool has_gpu() const { return backend_ != nullptr && backend_->info().capabilities.is_hardware; }

    const Telemetry& telemetry() const { return telemetry_; }
    Telemetry& telemetry() { return telemetry_; }

    scheduler::ThreadPool& cpu_pool() { return *cpu_pool_; }
    const scheduler::CostModel& cost_model() const { return cost_model_; }
    const WorkQueue& queue() const { return queue_; }

    // Samples utilization from live counters and updates telemetry.
    void refresh_utilization();

private:
    Status admit(const WorkItem& item, const scheduler::DispatchDecision& decision,
                 DispatchOutcome& outcome);
    // Releases the reservation for `id`. Returns false when unknown.
    bool release_reservation(u64 id);

    bool initialized_ = false;
    ManagerConfig config_{};
    backends::Backend* backend_ = nullptr;
    memory::MemoryBudget* budget_ = nullptr;
    std::unique_ptr<scheduler::ThreadPool> cpu_pool_;
    scheduler::CostModel cost_model_{};
    WorkQueue queue_{};
    Telemetry telemetry_{};
    u64 next_id_ = 1;
    // Bytes reserved per admitted, not-yet-completed dispatch id. The manager
    // owns the CPU path and releases there; the caller releases GPU/split work
    // through complete_dispatch().
    mutable std::mutex reservation_mutex_;
    std::unordered_map<u64, u64> reserved_by_id_;
    std::atomic<u64> in_flight_bytes_{0};
};

}  // namespace lx::manager
