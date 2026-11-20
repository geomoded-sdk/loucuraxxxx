// Tests for the manager: queueing, admission control, placement and telemetry.
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "backends/cpu/cpu_backend.hpp"
#include "manager/manager.hpp"
#include "memory/memory_budget.hpp"
#include "test_framework.hpp"

using namespace lx;

namespace {

manager::WorkItem make_item(u64 bytes, u32 priority = 0, const std::string& name = "pass") {
    manager::WorkItem item;
    item.priority = priority;
    item.working_set_bytes = bytes;
    item.output_bytes = bytes;
    item.independent_units = 64;
    item.name = name;
    return item;
}

}  // namespace

LX_TEST(manager, work_queue_orders_by_priority_then_arrival) {
    manager::WorkQueue queue;
    queue.push(make_item(1, 0, "low"));
    queue.push(make_item(1, 5, "high"));
    queue.push(make_item(1, 0, "low2"));

    auto first = queue.try_pop();
    auto second = queue.try_pop();
    auto third = queue.try_pop();
    LX_CHECK(first.has_value());
    LX_CHECK_EQ(first->name, std::string("high"));
    LX_CHECK_EQ(second->name, std::string("low"));
    LX_CHECK_EQ(third->name, std::string("low2"));
    LX_CHECK(!queue.try_pop().has_value());
}

LX_TEST(manager, work_queue_counters_track_push_and_pop) {
    manager::WorkQueue queue;
    LX_CHECK_EQ(queue.total_enqueued(), static_cast<u64>(0));
    queue.push(make_item(4096, 0, "a"));
    queue.push(make_item(8192, 0, "b"));
    LX_CHECK_EQ(queue.total_enqueued(), static_cast<u64>(2));
    LX_CHECK_EQ(queue.total_bytes_enqueued(), static_cast<u64>(4096 + 8192));
    LX_CHECK_EQ(queue.depth(), static_cast<std::size_t>(2));
    LX_CHECK(queue.try_pop().has_value());
    LX_CHECK_EQ(queue.total_dequeued(), static_cast<u64>(1));
    LX_CHECK_EQ(queue.depth(), static_cast<std::size_t>(1));
}

LX_TEST(manager, initialize_without_gpu_works) {
    memory::MemoryBudget budget;
    budget.set_limit(64 * kMiB);

    manager::Manager manager;
    manager::ManagerConfig config;
    LX_CHECK_OK(manager.initialize(nullptr, &budget, config));
    LX_CHECK(!manager.has_gpu());
    manager.shutdown();
}

LX_TEST(manager, tiny_work_is_placed_on_cpu) {
    memory::MemoryBudget budget;
    budget.set_limit(64 * kMiB);
    manager::Manager manager;
    LX_CHECK_OK(manager.initialize(nullptr, &budget, {}));

    auto outcome = manager.plan(make_item(4 * kKiB, 0, "tiny"));
    LX_CHECK_OK(outcome.status());
    LX_CHECK(outcome.value().target == scheduler::DeviceTarget::Cpu);
    LX_CHECK(!outcome.value().rationale.empty());
    manager.shutdown();
}

LX_TEST(manager, large_work_without_gpu_is_forced_to_cpu) {
    memory::MemoryBudget budget;
    budget.set_limit(2 * kGiB);
    manager::Manager manager;
    LX_CHECK_OK(manager.initialize(nullptr, &budget, {}));

    auto outcome = manager.plan(make_item(256 * kMiB, 0, "big"));
    LX_CHECK_OK(outcome.status());
    // No hardware GPU present, so even GPU-preferred work must land on the CPU.
    LX_CHECK(outcome.value().target == scheduler::DeviceTarget::Cpu);
    manager.shutdown();
}

LX_TEST(manager, admission_control_rejects_when_queue_full) {
    memory::MemoryBudget budget;
    budget.set_limit(64 * kMiB);
    manager::Manager manager;
    manager::ManagerConfig config;
    config.max_queue_depth = 2;
    LX_CHECK_OK(manager.initialize(nullptr, &budget, config));

    // Small items go to the CPU queue and stay there (nothing dequeues them).
    LX_CHECK_OK(manager.plan(make_item(4 * kKiB)));
    LX_CHECK_OK(manager.plan(make_item(4 * kKiB)));
    auto rejected = manager.plan(make_item(4 * kKiB));
    LX_CHECK(!rejected.ok());
    LX_CHECK(rejected.status().code() == ErrorCode::ResourceBusy);
    LX_CHECK_EQ(manager.telemetry().snapshot().passes_rejected, static_cast<u64>(1));
    manager.shutdown();
}

LX_TEST(manager, budget_exhaustion_is_reported) {
    memory::MemoryBudget budget;
    budget.set_limit(1 * kMiB);
    manager::Manager manager;
    LX_CHECK_OK(manager.initialize(nullptr, &budget, {}));

    // A working set larger than the whole budget must be refused, not crash.
    auto outcome = manager.plan(make_item(4 * kMiB));
    LX_CHECK(!outcome.ok());
    LX_CHECK(outcome.status().code() == ErrorCode::OutOfMemory);
    manager.shutdown();
}

LX_TEST(manager, telemetry_counts_dispatches) {
    memory::MemoryBudget budget;
    budget.set_limit(64 * kMiB);
    manager::Manager manager;
    LX_CHECK_OK(manager.initialize(nullptr, &budget, {}));

    for (int i = 0; i < 5; ++i) LX_CHECK_OK(manager.plan(make_item(4 * kKiB)));
    const auto snapshot = manager.telemetry().snapshot();
    LX_CHECK_EQ(snapshot.passes_dispatched_to_cpu, static_cast<u64>(5));
    LX_CHECK(snapshot.queue_depth > 0);
    manager.shutdown();
}

LX_TEST(manager, in_flight_ceiling_is_enforced) {
    memory::MemoryBudget budget;
    budget.set_limit(64 * kMiB);
    manager::Manager manager;
    manager::ManagerConfig config;
    config.max_in_flight_bytes = 8 * kMiB;
    LX_CHECK_OK(manager.initialize(nullptr, &budget, config));

    // Each item reserves 2 MiB (working set + output). Four fit under 8 MiB.
    for (int i = 0; i < 4; ++i) {
        LX_CHECK_OK(manager.plan(make_item(1 * kMiB)));
    }
    LX_CHECK_EQ(manager.in_flight_bytes(), static_cast<u64>(8 * kMiB));
    // The fifth would exceed the ceiling and must be refused, not queued.
    auto rejected = manager.plan(make_item(1 * kMiB));
    LX_CHECK(!rejected.ok());
    LX_CHECK(rejected.status().code() == ErrorCode::ResourceBusy);
    manager.shutdown();
}

LX_TEST(manager, admit_rejects_overflowing_size) {
    memory::MemoryBudget budget;
    budget.set_limit(64 * kMiB);
    manager::Manager manager;
    LX_CHECK_OK(manager.initialize(nullptr, &budget, {}));

    // working_set + output would wrap to a tiny number; admission must still
    // reject it instead of silently accepting an impossible reservation.
    manager::WorkItem huge = make_item(~0ull);
    const auto outcome = manager.plan(huge);
    LX_CHECK(!outcome.ok());
    LX_CHECK_EQ(manager.in_flight_bytes(), static_cast<u64>(0));
    manager.shutdown();
}

LX_TEST(manager, complete_dispatch_releases_reservation) {
    memory::MemoryBudget budget;
    budget.set_limit(64 * kMiB);
    manager::Manager manager;
    manager::ManagerConfig config;
    config.max_in_flight_bytes = 4 * kMiB;
    LX_CHECK_OK(manager.initialize(nullptr, &budget, config));

    auto outcome = manager.plan(make_item(1 * kMiB));  // 2 MiB reserved
    LX_CHECK_OK(outcome.status());
    const u64 id = outcome.value().id;
    LX_CHECK(manager.in_flight_bytes() > 0);

    LX_CHECK_OK(manager.complete_dispatch(id));
    LX_CHECK_EQ(manager.in_flight_bytes(), static_cast<u64>(0));
    LX_CHECK_EQ(budget.used(), static_cast<u64>(0));
    // Completing the same id twice is reported, not silently accepted.
    LX_CHECK(!manager.complete_dispatch(id).ok());
    manager.shutdown();
}

LX_TEST(manager, shutdown_releases_outstanding_reservations) {
    memory::MemoryBudget budget;
    budget.set_limit(64 * kMiB);
    manager::Manager manager;
    LX_CHECK_OK(manager.initialize(nullptr, &budget, {}));
    LX_CHECK_OK(manager.plan(make_item(4 * kMiB)));
    LX_CHECK(budget.used() > 0);
    // Shutdown must not leak the budget a caller never completed.
    manager.shutdown();
    LX_CHECK_EQ(budget.used(), static_cast<u64>(0));
}

LX_TEST(manager, next_cpu_task_dequeues_and_releases_budget) {
    memory::MemoryBudget budget;
    budget.set_limit(64 * kMiB);
    manager::Manager manager;
    LX_CHECK_OK(manager.initialize(nullptr, &budget, {}));

    LX_CHECK_OK(manager.plan(make_item(1 * kMiB)));
    LX_CHECK(budget.used() > 0);
    auto task = manager.next_cpu_task();
    LX_CHECK(task.has_value());
    LX_CHECK_EQ(budget.used(), static_cast<u64>(0));
    manager.shutdown();
}

LX_TEST(manager, cpu_field_reports_queue_pressure_not_utilization) {
    // The field was called cpu_utilization while measuring pending/threads. That
    // reads 0.0 when every worker is busy with one long pass and the queue is
    // empty -- the opposite of what "utilization" implies. The rename makes the
    // name match the measurement; this test pins the semantics so a future
    // busy-time sampler cannot quietly reuse the field for a different quantity.
    memory::MemoryBudget budget;
    manager::Manager manager;
    manager::ManagerConfig config;
    config.cpu_worker_count = 1;
    LX_CHECK_OK(manager.initialize(nullptr, &budget, config));

    // Nothing queued: pressure is 0 even though a worker exists.
    LX_CHECK_EQ(manager.telemetry().snapshot().cpu_queue_pressure, 0.0);

    // Telemetry stores the value it is given, unchanged.
    manager::Telemetry telemetry;
    telemetry.set_utilization(0.75, 0.0, 0.5, false);
    const auto snapshot = telemetry.snapshot();
    LX_CHECK_EQ(snapshot.cpu_queue_pressure, 0.75);
    LX_CHECK_EQ(snapshot.memory_utilization, 0.5);
}

LX_TEST(manager, worker_utilization_is_measured_not_inferred) {
    // Queue pressure and worker occupancy answer different questions. A single
    // long task on one worker leaves the queue empty (pressure 0) while the worker
    // is fully busy. The manager must therefore report occupancy from the pool's
    // measured task time, not from queue depth.
    memory::MemoryBudget budget;
    manager::Manager manager;
    manager::ManagerConfig config;
    config.cpu_worker_count = 2;
    LX_CHECK_OK(manager.initialize(nullptr, &budget, config));

    manager.refresh_utilization();
    LX_CHECK_EQ(manager.telemetry().snapshot().cpu_worker_utilization, 0.0);

    // Occupy one worker with a task long enough to register, then sample while it
    // is still running: the queue is empty, but occupancy is not zero.
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    LX_CHECK_OK(manager.cpu_pool().submit([&] {
        started.store(true);
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }));
    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    manager.refresh_utilization();
    const auto busy = manager.telemetry().snapshot();
    release.store(true);
    LX_CHECK_OK(manager.cpu_pool().wait_idle());

    LX_CHECK_EQ(busy.cpu_queue_pressure, 0.0);
    LX_CHECK(busy.cpu_worker_utilization > 0.0);
    LX_CHECK(busy.cpu_worker_utilization <= 1.0);
    manager.shutdown();
}

LX_TEST(manager, gpu_utilization_is_not_claimed_when_unmeasured) {
    // The snapshot carries a flag alongside gpu_utilization because 0.0 alone is
    // ambiguous. No backend reports utilization, so the flag must stay false; if
    // this ever starts passing true, a consumer will display a number nothing
    // measured.
    memory::MemoryBudget budget;
    budget.set_limit(64 * kMiB);
    manager::Manager manager;
    LX_CHECK_OK(manager.initialize(nullptr, &budget, {}));
    manager.refresh_utilization();
    const auto snapshot = manager.telemetry().snapshot();
    LX_CHECK(!snapshot.gpu_utilization_available);
    LX_CHECK_EQ(snapshot.gpu_utilization, 0.0);
    // CPU and memory utilization are measured, so they are not gated by the flag.
    LX_CHECK(snapshot.cpu_queue_pressure >= 0.0);
    LX_CHECK(snapshot.cpu_queue_pressure <= 1.0);
    LX_CHECK(snapshot.memory_utilization >= 0.0);
    manager.shutdown();
}

LX_TEST(manager, utilization_flag_reports_when_a_measurement_exists) {
    // The flag must be usable, not permanently false: when a caller does supply a
    // measurement, it has to survive to the snapshot.
    manager::Telemetry telemetry;
    telemetry.set_utilization(0.5, 0.0, 0.25, true);
    auto snapshot = telemetry.snapshot();
    LX_CHECK(snapshot.gpu_utilization_available);
    LX_CHECK_EQ(snapshot.gpu_utilization, 0.0);  // idle GPU, distinct from absent
    LX_CHECK_EQ(snapshot.cpu_queue_pressure, 0.5);

    telemetry.reset();
    snapshot = telemetry.snapshot();
    LX_CHECK(!snapshot.gpu_utilization_available);
    LX_CHECK_EQ(snapshot.cpu_queue_pressure, 0.0);
}

LX_TEST_MAIN("manager")
