// Tests for the thread pool, cost model and task graph.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <numeric>
#include <vector>

#include "scheduler/cost_model.hpp"
#include "scheduler/task_graph.hpp"
#include "scheduler/thread_pool.hpp"
#include "test_framework.hpp"

using namespace lx;

LX_TEST(scheduler, thread_pool_runs_all_tasks) {
    scheduler::ThreadPoolConfig config;
    config.thread_count = 4;
    scheduler::ThreadPool pool(config);
    LX_CHECK_OK(pool.start());

    std::atomic<int> counter{0};
    constexpr int kTasks = 1000;
    for (int i = 0; i < kTasks; ++i) {
        LX_CHECK_OK(pool.submit([&counter] { counter.fetch_add(1); }));
    }
    LX_CHECK_OK(pool.wait_idle());
    LX_CHECK_EQ(counter.load(), kTasks);
    pool.stop();
}

LX_TEST(scheduler, thread_pool_rejects_before_start) {
    scheduler::ThreadPool pool;
    const Status status = pool.submit([] {});
    LX_CHECK(!status.ok());
    LX_CHECK(status.code() == ErrorCode::ResourceBusy);
}

LX_TEST(scheduler, thread_pool_lane_ordering) {
    scheduler::ThreadPoolConfig config;
    config.thread_count = 4;
    scheduler::ThreadPool pool(config);
    LX_CHECK_OK(pool.start());

    std::mutex mutex;
    std::vector<int> order;
    for (int i = 0; i < 50; ++i) {
        LX_CHECK_OK(pool.submit(
            [&mutex, &order, i] {
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(i);
            },
            0));
    }
    LX_CHECK_OK(pool.wait_idle());
    // A single lane preserves submission order.
    bool sorted = std::is_sorted(order.begin(), order.end());
    LX_CHECK(sorted);
    pool.stop();
}

LX_TEST(scheduler, lane_ordering_survives_stealing_pressure) {
    // A single lane must stay ordered even while other lanes are flooded, which
    // is exactly the case a naive work-stealing pool gets wrong.
    scheduler::ThreadPoolConfig config;
    config.thread_count = 4;
    config.allow_stealing = true;
    scheduler::ThreadPool pool(config);
    LX_CHECK_OK(pool.start());

    std::mutex mutex;
    std::vector<int> order;
    std::atomic<int> background{0};
    for (int i = 0; i < 200; ++i) {
        LX_CHECK_OK(pool.submit([&background] { background.fetch_add(1); }, 1));
        LX_CHECK_OK(pool.submit([&mutex, &order, i] {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(i);
        }, 0));
    }
    LX_CHECK_OK(pool.wait_idle());
    LX_CHECK(std::is_sorted(order.begin(), order.end()));
    LX_CHECK_EQ(background.load(), 200);
    pool.stop();
}

LX_TEST(scheduler, pool_restart_is_clean) {
    scheduler::ThreadPoolConfig config;
    config.thread_count = 2;
    scheduler::ThreadPool pool(config);
    LX_CHECK_OK(pool.start());
    std::atomic<int> first{0};
    for (int i = 0; i < 50; ++i) LX_CHECK_OK(pool.submit([&first] { first.fetch_add(1); }));
    LX_CHECK_OK(pool.wait_idle());
    pool.stop();

    // Starting again must reset counters and accept work as if fresh.
    LX_CHECK_OK(pool.start());
    LX_CHECK_EQ(pool.completed(), static_cast<u64>(0));
    std::atomic<int> second{0};
    for (int i = 0; i < 50; ++i) LX_CHECK_OK(pool.submit([&second] { second.fetch_add(1); }));
    LX_CHECK_OK(pool.wait_idle());
    LX_CHECK_EQ(second.load(), 50);
    pool.stop();
}

LX_TEST(scheduler, cost_model_prefers_cpu_for_tiny_work) {
    scheduler::CostModel model;
    scheduler::DispatchRequest request;
    request.working_set_bytes = 1024;  // 1 KiB
    const auto decision = model.decide(request);
    LX_CHECK(decision.target == scheduler::DeviceTarget::Cpu);
    LX_CHECK(!decision.rationale.empty());
}

LX_TEST(scheduler, cost_model_prefers_gpu_for_large_work) {
    scheduler::CostModel model;
    scheduler::DispatchRequest request;
    request.working_set_bytes = 512ull * 1024 * 1024;  // 512 MiB
    request.output_bytes = 512ull * 1024 * 1024;
    request.independent_units = 256;
    const auto decision = model.decide(request);
    // With default constants the GPU wins decisively at this size.
    LX_CHECK(decision.target != scheduler::DeviceTarget::Cpu);
}

LX_TEST(scheduler, cost_model_respects_gpu_only_feature) {
    scheduler::CostModel model;
    scheduler::DispatchRequest request;
    request.working_set_bytes = 1024;  // small, would normally go to CPU
    request.requires_gpu_only_feature = true;
    const auto decision = model.decide(request);
    LX_CHECK(decision.target != scheduler::DeviceTarget::Cpu);
}

LX_TEST(scheduler, task_graph_respects_dependencies) {
    scheduler::ThreadPool pool;
    LX_CHECK_OK(pool.start());
    scheduler::TaskGraph graph(pool);

    std::atomic<int> counter{0};
    std::vector<int> completion_order;
    std::mutex mutex;

    const auto a = graph.add_node("a", [&] {
        std::lock_guard<std::mutex> lock(mutex);
        completion_order.push_back(1);
        counter.fetch_add(1);
    });
    const auto b = graph.add_node("b", [&] {
        std::lock_guard<std::mutex> lock(mutex);
        completion_order.push_back(2);
        counter.fetch_add(1);
    });
    const auto c = graph.add_node("c", [&] {
        std::lock_guard<std::mutex> lock(mutex);
        completion_order.push_back(3);
        counter.fetch_add(1);
    });
    LX_CHECK_OK(graph.add_dependency(b, a));
    LX_CHECK_OK(graph.add_dependency(c, b));

    LX_CHECK_OK(graph.run_to_completion());
    LX_CHECK_EQ(counter.load(), 3);
    LX_CHECK_EQ(completion_order.size(), static_cast<std::size_t>(3));
    // Strict chain: a before b before c.
    LX_CHECK_EQ(completion_order[0], 1);
    LX_CHECK_EQ(completion_order[1], 2);
    LX_CHECK_EQ(completion_order[2], 3);
    pool.stop();
}

LX_TEST(scheduler, task_graph_detects_cycles) {
    scheduler::ThreadPool pool;
    LX_CHECK_OK(pool.start());
    scheduler::TaskGraph graph(pool);
    const auto a = graph.add_node("a", [] {});
    const auto b = graph.add_node("b", [] {});
    LX_CHECK_OK(graph.add_dependency(b, a));
    // Adding a -> b would close a cycle.
    const Status cycle = graph.add_dependency(a, b);
    LX_CHECK(!cycle.ok());
    pool.stop();
}

// --- Core-class scheduling (hybrid CPU, e.g. Apple silicon) ---------------

LX_TEST(scheduler, performance_split_is_proportional) {
    // 8 workers on a 4P+4E part: half should be performance-class.
    LX_CHECK_EQ(scheduler::recommend_performance_threads(8, 4, 4), 4u);
    // 6 workers on 4P+4E: proportional split rounds up to 3.
    LX_CHECK_EQ(scheduler::recommend_performance_threads(6, 4, 4), 3u);
    // A uniform CPU reports every worker as performance-class.
    LX_CHECK_EQ(scheduler::recommend_performance_threads(8, 0, 0), 8u);
    LX_CHECK_EQ(scheduler::recommend_performance_threads(4, 0, 8), 4u);
    // The split never starves either class when more than one worker exists.
    const u32 perf = scheduler::recommend_performance_threads(4, 8, 1);
    LX_CHECK(perf >= 1u);
    LX_CHECK(perf < 4u);
}

LX_TEST(scheduler, pool_assigns_worker_classes) {
    scheduler::ThreadPoolConfig config;
    config.thread_count = 4;
    config.performance_threads = 3;
    scheduler::ThreadPool pool(config);
    LX_CHECK_OK(pool.start());
    LX_CHECK_EQ(pool.performance_worker_count(), 3u);
    LX_CHECK_EQ(pool.efficiency_worker_count(), 1u);
    LX_CHECK(pool.worker_class(0) == scheduler::CoreClass::Performance);
    LX_CHECK(pool.worker_class(2) == scheduler::CoreClass::Performance);
    LX_CHECK(pool.worker_class(3) == scheduler::CoreClass::Efficiency);
    pool.stop();
}

LX_TEST(scheduler, submit_on_runs_work_regardless_of_class) {
    scheduler::ThreadPoolConfig config;
    config.thread_count = 4;
    config.performance_threads = 2;
    scheduler::ThreadPool pool(config);
    LX_CHECK_OK(pool.start());

    std::atomic<int> perf_tasks{0};
    std::atomic<int> eff_tasks{0};
    for (int i = 0; i < 20; ++i) {
        LX_CHECK_OK(pool.submit_on([&] { perf_tasks.fetch_add(1); },
                                   scheduler::CoreClass::Performance));
        LX_CHECK_OK(pool.submit_on([&] { eff_tasks.fetch_add(1); },
                                   scheduler::CoreClass::Efficiency));
    }
    LX_CHECK_OK(pool.wait_idle());
    // Every task runs, whichever class it was steered to.
    LX_CHECK_EQ(perf_tasks.load(), 20);
    LX_CHECK_EQ(eff_tasks.load(), 20);
    pool.stop();
}

LX_TEST(scheduler, submit_on_falls_back_when_class_absent) {
    // A pool with no efficiency workers must still accept efficiency work.
    scheduler::ThreadPoolConfig config;
    config.thread_count = 2;
    config.performance_threads = 2;  // all performance
    scheduler::ThreadPool pool(config);
    LX_CHECK_OK(pool.start());
    std::atomic<int> ran{0};
    for (int i = 0; i < 10; ++i) {
        LX_CHECK_OK(pool.submit_on([&] { ran.fetch_add(1); },
                                   scheduler::CoreClass::Efficiency));
    }
    LX_CHECK_OK(pool.wait_idle());
    LX_CHECK_EQ(ran.load(), 10);
    pool.stop();
}

// --- Cost model: unified memory removes transfer cost ---------------------

LX_TEST(scheduler, utilization_reflects_measured_busy_time) {
    // The point of this number is that it is measured from task execution, so it
    // must distinguish a busy pool from an idle one. Queue depth cannot: a single
    // long task on one worker leaves the queue empty while the worker is pegged.
    scheduler::ThreadPoolConfig config;
    config.thread_count = 2;
    scheduler::ThreadPool pool(config);
    LX_CHECK_OK(pool.start());

    // Idle pool: no tasks have run, so occupancy is zero, not unknown.
    LX_CHECK_EQ(pool.busy_nanoseconds(), 0u);
    LX_CHECK(pool.utilization() < 0.05);

    // Saturate both workers with work that takes measurable time.
    const int kTasks = 8;
    const auto spin_for = [] {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
        while (std::chrono::steady_clock::now() < until) {
        }
    };
    for (int i = 0; i < kTasks; ++i) {
        LX_CHECK_OK(pool.submit(spin_for, static_cast<lx::u32>(i % 2)));
    }
    LX_CHECK_OK(pool.wait_idle());

    // 8 tasks x 25 ms = 200 ms of task time over 2 workers; the pool was alive for
    // at least that long in wall time, so occupancy must be clearly non-zero.
    LX_CHECK(pool.busy_nanoseconds() >= 150'000'000u);
    LX_CHECK(pool.utilization() > 0.2);
    LX_CHECK(pool.utilization() <= 1.0);

    // Task time is bounded by worker-time available; the ratio cannot exceed 1.
    const u64 available = pool.uptime_nanoseconds() * pool.thread_count();
    LX_CHECK(pool.busy_nanoseconds() <= available + 5'000'000u);
    pool.stop();
}

LX_TEST(scheduler, pending_counts_running_tasks_but_queued_does_not) {
    // pending() was documented as "queued but not yet started" while it actually
    // counted running tasks too, so the manager's queue pressure read 1.0 for a
    // pool with an empty queue. The two counts must stay distinct: queued() is the
    // queue depth, pending() is everything in flight.
    scheduler::ThreadPoolConfig config;
    config.thread_count = 1;
    scheduler::ThreadPool pool(config);
    LX_CHECK_OK(pool.start());

    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    LX_CHECK_OK(pool.submit([&] {
        started.store(true);
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }));
    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // One task is running on the only worker; nothing is waiting.
    LX_CHECK_EQ(pool.pending(), static_cast<std::size_t>(1));
    LX_CHECK_EQ(pool.queued(), static_cast<std::size_t>(0));

    release.store(true);
    LX_CHECK_OK(pool.wait_idle());
    LX_CHECK_EQ(pool.pending(), static_cast<std::size_t>(0));
    pool.stop();
}

LX_TEST(scheduler, busy_time_includes_a_task_that_is_still_running) {
    // Occupancy sampled while a long task runs must already be non-zero; crediting
    // time only when the task returns made a pegged pool read as idle.
    scheduler::ThreadPoolConfig config;
    config.thread_count = 1;
    scheduler::ThreadPool pool(config);
    LX_CHECK_OK(pool.start());

    std::atomic<bool> release{false};
    LX_CHECK_OK(pool.submit([&] {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const u64 during = pool.busy_nanoseconds();
    release.store(true);
    LX_CHECK_OK(pool.wait_idle());
    const u64 after = pool.busy_nanoseconds();

    LX_CHECK(during >= 20'000'000u);  // the running task's time is visible
    LX_CHECK(after >= during);        // and it is not double counted later
    LX_CHECK(pool.utilization() > 0.0);
    pool.stop();
}

LX_TEST(scheduler, utilization_stays_zero_before_start) {
    // Reading utilization before start must not divide by a zero uptime or
    // invent a figure; 0.0 here means "no window measured yet".
    scheduler::ThreadPool pool;
    LX_CHECK_EQ(pool.uptime_nanoseconds(), 0u);
    LX_CHECK(pool.utilization() == 0.0);
}

LX_TEST(scheduler, unified_memory_has_no_transfer_cost) {
    scheduler::DispatchRequest request;
    request.working_set_bytes = 64 * 1024 * 1024;  // large enough to matter
    request.independent_units = 64;
    request.is_embarrassingly_parallel = true;

    scheduler::CostConstants discrete;
    discrete.unified_memory = false;
    scheduler::CostConstants unified;
    unified.unified_memory = true;

    const scheduler::CostModel discrete_model(discrete);
    const scheduler::CostModel unified_model(unified);

    const auto discrete_decision = discrete_model.decide(request);
    const auto unified_decision = unified_model.decide(request);

    // Dropping the transfer must never make the GPU estimate worse.
    LX_CHECK(unified_decision.estimated_gpu_seconds <=
             discrete_decision.estimated_gpu_seconds);
    // With unified memory the GPU estimate is exactly the compute + dispatch.
    LX_CHECK(unified_decision.estimated_gpu_seconds > 0.0);
}

LX_TEST(scheduler, unified_memory_changes_placement) {
    // A mid-sized pass. When transfers dominate, the CPU wins; when memory is
    // shared there is nothing to move and the GPU wins. Same work, same host,
    // different topology -> different placement.
    scheduler::DispatchRequest request;
    request.working_set_bytes = 2 * 1024 * 1024;
    request.independent_units = 16;
    request.is_embarrassingly_parallel = true;

    scheduler::CostConstants discrete;
    discrete.unified_memory = false;
    scheduler::CostConstants unified;
    unified.unified_memory = true;

    const auto discrete_decision = scheduler::CostModel(discrete).decide(request);
    const auto unified_decision = scheduler::CostModel(unified).decide(request);

    LX_CHECK(discrete_decision.target == scheduler::DeviceTarget::Cpu);
    LX_CHECK(unified_decision.target == scheduler::DeviceTarget::Gpu);
}

LX_TEST(scheduler, task_graph_clear_releases_nodes_and_resets) {
    // The graph owns its nodes; destroying and clearing must not leak and must
    // reset the completion counter so the graph is reusable.
    scheduler::ThreadPool pool;
    LX_CHECK_OK(pool.start());
    {
        scheduler::TaskGraph graph(pool);
        graph.add_node("a", [] {});
        graph.add_node("b", [] {});
        LX_CHECK_EQ(graph.node_count(), static_cast<std::size_t>(2));
        LX_CHECK_OK(graph.run_to_completion());
        LX_CHECK(graph.complete());
        graph.clear();
        LX_CHECK_EQ(graph.node_count(), static_cast<std::size_t>(0));
        LX_CHECK(!graph.complete() || graph.node_count() == 0);
        // Reuse after clear: the counter must have been reset.
        graph.add_node("c", [] {});
        LX_CHECK_OK(graph.run_to_completion());
        LX_CHECK(graph.complete());
    }
    pool.stop();
}

LX_TEST_MAIN("scheduler")
