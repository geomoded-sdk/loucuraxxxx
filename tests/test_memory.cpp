// Tests for memory budgeting, the buffer pool and the arena.
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "memory/arena.hpp"
#include "memory/buffer_pool.hpp"
#include "memory/memory_budget.hpp"
#include "test_framework.hpp"

using namespace lx;

namespace {
hardware::MemoryInfo make_memory(u64 total, u64 available) {
    hardware::MemoryInfo info;
    info.total_bytes = total;
    info.available_bytes = available;
    info.page_size_bytes = 4096;
    return info;
}
}  // namespace

LX_TEST(memory, policy_grants_request_within_limits) {
    memory::MemoryPolicy policy;
    const auto plan = policy.plan(make_memory(32 * kGiB, 20 * kGiB), 2 * kGiB);
    LX_CHECK_EQ(plan.granted_bytes, 2 * kGiB);
    LX_CHECK(!plan.clamped);
    LX_CHECK(plan.os_reserve_bytes > 0);
}

LX_TEST(memory, policy_clamps_excessive_request) {
    memory::MemoryPolicy policy;
    // Ask for almost all of an 8 GiB machine; the OS reserve must clamp it.
    const auto plan = policy.plan(make_memory(8 * kGiB, 7 * kGiB), 7 * kGiB);
    LX_CHECK(plan.clamped);
    LX_CHECK(plan.granted_bytes < plan.requested_bytes);
    LX_CHECK(plan.granted_bytes <= 8 * kGiB - plan.os_reserve_bytes);
    LX_CHECK(!plan.reason.empty());
}

LX_TEST(memory, policy_respects_minimum_free_floor) {
    memory::MemoryPolicy policy(0.05, 1024 * kMiB);  // small fraction, 1 GiB floor
    const auto plan = policy.plan(make_memory(4 * kGiB, 3 * kGiB), 3 * kGiB);
    // At least the 1 GiB floor must remain reserved.
    LX_CHECK(plan.os_reserve_bytes >= 1024 * kMiB);
    LX_CHECK(plan.granted_bytes <= 3 * kGiB);
}

LX_TEST(memory, budget_commit_and_release) {
    memory::MemoryBudget budget;
    budget.set_limit(1000);
    LX_CHECK_OK(budget.commit(400, "test"));
    LX_CHECK_EQ(budget.used(), static_cast<u64>(400));
    LX_CHECK_OK(budget.commit(400, "test"));
    LX_CHECK_EQ(budget.used(), static_cast<u64>(800));
    // This would exceed the limit.
    const Status over = budget.commit(400, "test");
    LX_CHECK(!over.ok());
    LX_CHECK(over.code() == ErrorCode::OutOfMemory);
    budget.release(400);
    LX_CHECK_EQ(budget.used(), static_cast<u64>(400));
    LX_CHECK_OK(budget.commit(500, "test"));
    LX_CHECK_EQ(budget.peak(), static_cast<u64>(900));
}

LX_TEST(memory, budget_commit_rejects_overflowing_request) {
    // A request so large that used+bytes would wrap must be refused, not
    // silently accepted as a small commit.
    memory::MemoryBudget budget;
    budget.set_limit(1024);
    LX_CHECK_OK(budget.commit(512, "test"));
    const Status huge = budget.commit(~0ull, "test");
    LX_CHECK(!huge.ok());
    LX_CHECK(huge.code() == ErrorCode::OutOfMemory);
    LX_CHECK_EQ(budget.used(), static_cast<u64>(512));
}

LX_TEST(memory, budget_release_clamps_at_zero) {
    memory::MemoryBudget budget;
    budget.set_limit(1000);
    LX_CHECK_OK(budget.commit(100, "test"));
    budget.release(9999);  // over-release
    LX_CHECK_EQ(budget.used(), static_cast<u64>(0));
}

LX_TEST(memory, arena_allocates_aligned_and_rejects_oversize) {
    memory::Arena arena;
    LX_CHECK_OK(arena.reserve(4096));
    void* a = arena.allocate(64, 64);
    void* b = arena.allocate(64, 64);
    LX_CHECK(a != nullptr);
    LX_CHECK(b != nullptr);
    LX_CHECK_EQ(reinterpret_cast<std::uintptr_t>(a) % 64, static_cast<std::uintptr_t>(0));
    LX_CHECK_EQ(reinterpret_cast<std::uintptr_t>(b) % 64, static_cast<std::uintptr_t>(0));
    // A request larger than the whole arena, and one that would wrap the
    // end offset, must both be refused rather than returning a bad pointer.
    LX_CHECK(arena.allocate(8192, 64) == nullptr);
    LX_CHECK(arena.allocate(~0ull, 64) == nullptr);
    LX_CHECK(arena.allocate(0, 64) == nullptr);
    LX_CHECK(arena.allocate(64, 3) == nullptr);  // not a power of two
    // The failed allocations must not have moved the bump pointer past usable
    // space: a small allocation still succeeds.
    LX_CHECK(arena.allocate(64, 64) != nullptr);
}

LX_TEST(memory, arena_array_alloc_rejects_overflow) {
    memory::Arena arena;
    LX_CHECK_OK(arena.reserve(4096));
    // count * sizeof(T) would wrap; must be refused rather than under-allocate.
    u64* too_many = arena.allocate_array<u64>(~0ull);
    LX_CHECK(too_many == nullptr);
    u64* ok = arena.allocate_array<u64>(8);
    LX_CHECK(ok != nullptr);
}

LX_TEST(memory, policy_tolerates_nonsensical_fraction) {
    // A fraction far above 1 makes total*fraction overflow u64; the reserve must
    // be clamped to the total, never a wrapped or UB value.
    memory::MemoryPolicy policy(1.0e30, 0);
    hardware::MemoryInfo memory;
    memory.total_bytes = 8ull * kGiB;
    memory.available_bytes = 8ull * kGiB;
    const auto plan = policy.plan(memory, 1 * kGiB);
    LX_CHECK(plan.os_reserve_bytes <= memory.total_bytes);
    LX_CHECK(plan.granted_bytes <= memory.total_bytes);
}

LX_TEST(memory, buffer_pool_reuses_blocks) {
    memory::BufferPool pool;
    auto first = pool.acquire(64 * 1024);
    LX_CHECK_OK(first.status());
    LX_CHECK_EQ(pool.stats().misses, static_cast<u64>(1));
    pool.release(std::move(first.value()));
    // The released block is now cached.
    LX_CHECK(pool.stats().pooled_bytes >= 64 * 1024);

    auto second = pool.acquire(64 * 1024);
    LX_CHECK_OK(second.status());
    // The second acquire was served from the pool, not a fresh allocation.
    LX_CHECK_EQ(pool.stats().hits, static_cast<u64>(1));
    LX_CHECK_EQ(pool.stats().misses, static_cast<u64>(1));
}

LX_TEST(memory, buffer_pool_size_class_rounding) {
    LX_CHECK_EQ(memory::size_class_for(1), static_cast<std::size_t>(4096));
    LX_CHECK_EQ(memory::size_class_for(5000), static_cast<std::size_t>(8192));
    LX_CHECK_EQ(memory::size_class_for(8192), static_cast<std::size_t>(8192));
}

LX_TEST(memory, buffer_pool_size_class_never_wraps) {
    // A request far larger than any size class must terminate and return a
    // class that does not wrap to zero (which would previously loop forever).
    const std::size_t huge = memory::size_class_for(~0ull);
    LX_CHECK(huge != 0);
    LX_CHECK(huge >= memory::size_class_for(1u << 20));
    // A size that is already a large power of two stays representable.
    LX_CHECK_EQ(memory::size_class_for(1u << 30), static_cast<std::size_t>(1u << 30));
}

LX_TEST(memory, buffer_pool_is_thread_safe) {
    memory::BufferPool pool;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&pool, &failures] {
            for (int i = 0; i < 200; ++i) {
                auto buffer = pool.acquire(4096);
                if (!buffer.ok()) {
                    ++failures;
                    continue;
                }
                buffer.value().data()[0] = 0xAB;
                pool.release(std::move(buffer.value()));
            }
        });
    }
    for (auto& thread : threads) thread.join();
    LX_CHECK_EQ(failures.load(), 0);
}

LX_TEST(memory, budget_never_overcommits_under_contention) {
    // The budget is what keeps a reservation from starving the OS, so its limit
    // has to hold when many threads commit at once. Each thread tries to commit
    // a slice repeatedly; the total that succeeds must never exceed the limit,
    // and every byte that succeeded must be accounted for.
    memory::MemoryBudget budget;
    budget.set_limit(1024 * 1024);
    constexpr int kThreads = 8;
    constexpr u64 kSlice = 4096;
    std::atomic<u64> granted{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&budget, &granted, &failures] {
            for (int attempt = 0; attempt < 2000; ++attempt) {
                const auto status = budget.commit(kSlice, "contention");
                if (status.ok()) {
                    granted.fetch_add(kSlice, std::memory_order_relaxed);
                } else if (status.code() != ErrorCode::OutOfMemory) {
                    ++failures;
                }
            }
        });
    }
    for (auto& thread : threads) thread.join();

    LX_CHECK_EQ(failures.load(), 0);
    LX_CHECK(granted.load() <= budget.limit());
    LX_CHECK_EQ(budget.used(), granted.load());
    LX_CHECK(budget.used() <= budget.limit());
    LX_CHECK_EQ(budget.available(), budget.limit() - budget.used());
}

LX_TEST(memory, budget_release_returns_capacity_for_reuse) {
    memory::MemoryBudget budget;
    budget.set_limit(64 * 1024);
    LX_CHECK(budget.commit(64 * 1024, "full").ok());
    LX_CHECK(!budget.commit(1, "overflow").ok());
    LX_CHECK_EQ(budget.available(), 0u);
    budget.release(64 * 1024);
    LX_CHECK_EQ(budget.used(), 0u);
    LX_CHECK(budget.commit(64 * 1024, "again").ok());
}

LX_TEST_MAIN("memory")
