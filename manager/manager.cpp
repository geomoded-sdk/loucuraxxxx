#include "manager/manager.hpp"

#include <limits>

#include "core/log.hpp"

namespace lx::manager {

namespace {

int target_index(scheduler::DeviceTarget target) {
    switch (target) {
        case scheduler::DeviceTarget::Cpu: return 0;
        case scheduler::DeviceTarget::Gpu: return 1;
        case scheduler::DeviceTarget::Split: return 2;
    }
    return 0;
}

}  // namespace

ManagerConfig ManagerConfig::from_config(const configurator::Config& config,
                                         u32 performance_cores, u32 efficiency_cores) {
    ManagerConfig out;
    out.priority = config.priority;
    out.allow_cpu_gpu_split = config.allow_cpu_gpu_split;
    out.cpu_worker_count = config.reserved_cpu_cores;
    out.performance_workers =
        scheduler::recommend_performance_threads(config.reserved_cpu_cores,
                                                 performance_cores, efficiency_cores);
    return out;
}

Manager::~Manager() { shutdown(); }

Status Manager::initialize(backends::Backend* backend, memory::MemoryBudget* budget,
                           const ManagerConfig& config) {
    if (initialized_) {
        return Status::error(ErrorCode::ResourceBusy, "manager already initialized");
    }
    config_ = config;
    backend_ = backend;
    budget_ = budget;

    // A max_in_flight_bytes of 0 means "no separate ceiling": the memory budget
    // is then the only admission limit. Deriving one from the budget would
    // change the rejection reason (ResourceBusy instead of OutOfMemory) and is
    // therefore left to the caller to set explicitly.

    // The backend is the authority on whether memory is unified: it knows
    // whether its textures are host memory or device memory. On a unified host
    // (Apple silicon, integrated GPUs) there is no transfer cost, so the model
    // must not charge one.
    if (backend_ != nullptr && backend_->capabilities().unified_memory) {
        config_.cost_constants.unified_memory = true;
    }
    cost_model_.set_constants(config_.cost_constants);
    if (config_.allow_cpu_gpu_split && backend_ == nullptr) {
        // Splitting needs a GPU to split with; without one, never split.
        config_.allow_cpu_gpu_split = false;
    }

    scheduler::ThreadPoolConfig pool_config;
    pool_config.name = "lx-manager";
    pool_config.pin_to_cpus = config_.cpu_affinity;
    if (config_.cpu_worker_count > 0) pool_config.thread_count = config_.cpu_worker_count;
    pool_config.performance_threads = config_.performance_workers;
    cpu_pool_ = std::make_unique<scheduler::ThreadPool>(pool_config);
    LX_TRY(cpu_pool_->start());

    initialized_ = true;
    LX_INFO << "manager initialized: cpu workers=" << cpu_pool_->thread_count()
            << " (" << cpu_pool_->performance_worker_count() << " performance, "
            << cpu_pool_->efficiency_worker_count() << " efficiency)"
            << ", gpu=" << (has_gpu() ? backend_->info().device_name : std::string("none"))
            << ", max in-flight=" << (config_.max_in_flight_bytes / kMiB) << " MiB";
    return Status::success();
}

void Manager::shutdown() {
    if (cpu_pool_) {
        cpu_pool_->stop();
        cpu_pool_.reset();
    }
    queue_.clear();
    // Release any work the caller admitted but never completed, so the shared
    // budget is not left holding reservations after the manager goes away.
    {
        std::lock_guard<std::mutex> lock(reservation_mutex_);
        for (const auto& [id, bytes] : reserved_by_id_) {
            if (bytes > 0 && budget_ != nullptr) budget_->release(bytes);
        }
        reserved_by_id_.clear();
        in_flight_bytes_.store(0, std::memory_order_relaxed);
    }
    initialized_ = false;
}

Result<DispatchOutcome> Manager::plan(const WorkItem& item) {
    if (!initialized_) {
        return Status::error(ErrorCode::InitializationFailed, "manager not initialized");
    }

    // Admission control: refuse rather than build an unbounded backlog.
    if (queue_.depth() >= config_.max_queue_depth) {
        telemetry_.record_rejection();
        return Status::error(ErrorCode::ResourceBusy, "work queue is full");
    }

    scheduler::DispatchRequest request;
    request.working_set_bytes = item.working_set_bytes;
    request.output_bytes = item.output_bytes;
    request.independent_units = item.independent_units;
    request.requires_gpu_only_feature = item.requires_gpu_only_feature;
    request.is_embarrassingly_parallel = item.independent_units > 1;
    request.output_needed_on_cpu = true;

    scheduler::DispatchDecision decision = cost_model_.decide(request);

    // A GPU target is only valid when a real hardware GPU is present.
    if (decision.target != scheduler::DeviceTarget::Cpu && !has_gpu()) {
        decision.target = scheduler::DeviceTarget::Cpu;
        decision.rationale += " (no hardware GPU present; forced to CPU)";
    }
    if (decision.target == scheduler::DeviceTarget::Split &&
        !config_.allow_cpu_gpu_split) {
        // Without splitting, pick the cheaper of the two.
        decision.target = decision.estimated_cpu_seconds <= decision.estimated_gpu_seconds
                              ? scheduler::DeviceTarget::Cpu
                              : scheduler::DeviceTarget::Gpu;
        decision.rationale += " (splitting disabled)";
    }

    WorkItem queued = item;
    queued.id = next_id_++;
    queued.estimated_cpu_seconds = decision.estimated_cpu_seconds;
    queued.estimated_gpu_seconds = decision.estimated_gpu_seconds;

    DispatchOutcome outcome;
    LX_TRY(admit(queued, decision, outcome));

    telemetry_.record_dispatch(target_index(decision.target));
    telemetry_.record_queue_depth(queue_.depth());
    refresh_utilization();
    return outcome;
}

Status Manager::admit(const WorkItem& item, const scheduler::DispatchDecision& decision,
                      DispatchOutcome& outcome) {
    // Memory admission: a GPU-bound pass needs its working set resident. The
    // CPU path runs out of pooled host buffers and is bounded by the budget too.
    // Saturate rather than wrap: an absurd pair of sizes must be rejected, not
    // summed into a small number that sneaks under the ceiling.
    const u64 required =
        item.output_bytes > std::numeric_limits<u64>::max() - item.working_set_bytes
            ? std::numeric_limits<u64>::max()
            : item.working_set_bytes + item.output_bytes;

    // Reservation is check-then-commit, so it must be atomic: take the lock,
    // verify the in-flight ceiling, commit against the budget, then record the
    // reservation. A rejected item holds nothing.
    {
        std::lock_guard<std::mutex> lock(reservation_mutex_);
        const u64 current = in_flight_bytes_.load(std::memory_order_relaxed);
        const u64 ceiling = config_.max_in_flight_bytes;
        const bool over_ceiling =
            required > 0 && ceiling > 0 &&
            (current > ceiling || required > ceiling - current);
        if (over_ceiling) {
            telemetry_.record_rejection();
            return Status::error(ErrorCode::ResourceBusy,
                                 "in-flight byte ceiling reached (" +
                                     std::to_string(current) + "+" +
                                     std::to_string(required) + " > " +
                                     std::to_string(config_.max_in_flight_bytes) + ")");
        }
        if (budget_ != nullptr && required > 0) {
            const Status committed = budget_->commit(required, item.name);
            if (!committed.ok()) {
                telemetry_.record_rejection();
                return committed;
            }
        }
        if (required > 0) {
            reserved_by_id_[item.id] = required;
            in_flight_bytes_.fetch_add(required, std::memory_order_relaxed);
        }
    }

    outcome.id = item.id;
    outcome.target = decision.target;
    outcome.rationale = decision.rationale;
    outcome.estimated_seconds =
        decision.target == scheduler::DeviceTarget::Cpu ? decision.estimated_cpu_seconds
                                                        : decision.estimated_gpu_seconds;
    if (decision.target == scheduler::DeviceTarget::Split) {
        outcome.estimated_seconds = decision.estimated_split_seconds;
    }

    // Data movement: only a GPU-bound pass pays a transfer cost. A CPU pass that
    // already owns the pixels transfers nothing, and residency avoids a copy.
    if (decision.target == scheduler::DeviceTarget::Gpu) {
        outcome.bytes_transferred = item.working_set_bytes;
        telemetry_.record_transfer(item.working_set_bytes, item.output_bytes);
    } else {
        telemetry_.record_residency_avoided(item.working_set_bytes);
    }

    if (decision.target == scheduler::DeviceTarget::Cpu) {
        queue_.push(item);
    }
    // GPU/split work is handed to the backend by the caller using this outcome;
    // the manager records the decision and the transfer accounting.
    return Status::success();
}

std::optional<DispatchOutcome> Manager::next_cpu_task() {
    WorkItem item;
    if (!queue_.wait_pop(item, 0)) return std::nullopt;
    release_reservation(item.id);
    refresh_utilization();
    DispatchOutcome outcome;
    outcome.id = item.id;
    outcome.target = scheduler::DeviceTarget::Cpu;
    outcome.estimated_seconds = item.estimated_cpu_seconds;
    outcome.rationale = "dequeued for CPU execution";
    return outcome;
}

Status Manager::complete_dispatch(u64 id) {
    if (!release_reservation(id)) {
        return Status::error(ErrorCode::OutOfRange,
                             "unknown or already-completed dispatch id");
    }
    return Status::success();
}

u64 Manager::in_flight_bytes() const {
    return in_flight_bytes_.load(std::memory_order_relaxed);
}

// Drops the reservation for `id`, releasing its budget and in-flight bytes.
// Returns false when the id is unknown, so double-completion is reported
// instead of silently corrupting the counters.
bool Manager::release_reservation(u64 id) {
    std::lock_guard<std::mutex> lock(reservation_mutex_);
    auto it = reserved_by_id_.find(id);
    if (it == reserved_by_id_.end()) return false;
    const u64 bytes = it->second;
    reserved_by_id_.erase(it);
    if (bytes > 0) {
        if (budget_ != nullptr) budget_->release(bytes);
        in_flight_bytes_.fetch_sub(bytes, std::memory_order_relaxed);
    }
    return true;
}

void Manager::refresh_utilization() {
    double cpu_queue_pressure = 0.0;
    if (cpu_pool_ && cpu_pool_->thread_count() > 0) {
        // Pending work relative to worker count, clamped to 1. This describes how
        // backed up the queue is; it is not CPU utilization, and it is reported
        // under a name that says so.
        // queued(), not pending(): pending() also counts tasks already running,
        // so a busy pool with an empty queue would read as pressurised. Pressure
        // is about work waiting for a worker.
        const double pending = static_cast<double>(cpu_pool_->queued());
        cpu_queue_pressure =
            std::min(1.0, pending / static_cast<double>(cpu_pool_->thread_count()));
    }
    double memory_utilization = 0.0;
    if (budget_ != nullptr && budget_->limit() > 0) {
        memory_utilization = static_cast<double>(budget_->used()) /
                             static_cast<double>(budget_->limit());
    }
    // No backend exposes a utilization counter, so GPU utilization is not
    // measured and the availability flag stays false. Reporting 0.0 with the flag
    // set would claim an idle GPU; reporting it unset says what is true, which is
    // that nothing measured it.
    const double worker_utilization = cpu_pool_ ? cpu_pool_->utilization() : 0.0;
    telemetry_.set_utilization(cpu_queue_pressure, 0.0, memory_utilization, false,
                               worker_utilization);
    if (budget_ != nullptr) {
        telemetry_.set_budget(budget_->limit(), budget_->used(), budget_->peak());
    }
}

}  // namespace lx::manager
