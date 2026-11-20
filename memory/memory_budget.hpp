// LoucuraXXXX GPU - memory budget and reservation policy.
//
// The virtual GPU never assumes it may take all free RAM. This module computes
// a safe reservation from the host's real limits and tracks allocations against
// that reservation, so a misconfiguration cannot push the machine into swap or
// an OOM kill.
#pragma once

#include <atomic>
#include <cstddef>
#include <string>

#include "core/status.hpp"
#include "core/types.hpp"
#include "hardware/hardware_info.hpp"

namespace lx::memory {

// Snapshot of a reservation request, kept for reporting.
struct ReservationPlan {
    u64 system_total_bytes = 0;
    u64 system_available_bytes = 0;
    u64 os_reserve_bytes = 0;   // held back for the OS and other processes
    u64 requested_bytes = 0;    // what the user asked for
    u64 granted_bytes = 0;      // what policy actually allows
    bool clamped = false;       // true when granted < requested
    std::string reason;         // explains a clamp
};

// Computes a safe grant for a requested reservation. The OS reserve is a
// percentage of total RAM with an absolute floor, so small machines keep
// enough headroom and large machines do not hold back gigabytes needlessly.
class MemoryPolicy {
public:
    // fraction_of_total: share of *total* RAM the OS is guaranteed to keep
    // (default 0.20). minimum_free: absolute floor kept available (default
    // 512 MiB). Both apply; the stricter of the two wins.
    MemoryPolicy(double fraction_of_total = 0.20, u64 minimum_free = 512 * kMiB)
        : fraction_(fraction_of_total), minimum_free_(minimum_free) {}

    ReservationPlan plan(const hardware::MemoryInfo& memory, u64 requested_bytes) const;

    double os_fraction() const { return fraction_; }
    u64 minimum_free() const { return minimum_free_; }

private:
    double fraction_;
    u64 minimum_free_;
};

// Thread-safe accounting of the granted budget. Every real allocation made on
// behalf of the virtual GPU is committed here first; the commit fails rather
// than overrunning the budget.
class MemoryBudget {
public:
    MemoryBudget() = default;

    void configure(const ReservationPlan& plan);
    void set_limit(u64 bytes);

    u64 limit() const { return limit_.load(std::memory_order_relaxed); }
    u64 used() const { return used_.load(std::memory_order_relaxed); }
    u64 peak() const { return peak_.load(std::memory_order_relaxed); }
    u64 available() const {
        const u64 lim = limit();
        const u64 u = used();
        return u >= lim ? 0 : lim - u;
    }

    // Reserves bytes against the budget. Fails with OutOfMemory when the
    // request would exceed the limit.
    Status commit(u64 bytes, const std::string& owner);

    // Releases a previous commit. Over-release is clamped at zero.
    void release(u64 bytes);

    const ReservationPlan& plan() const { return plan_; }

private:
    std::atomic<u64> limit_{0};
    std::atomic<u64> used_{0};
    std::atomic<u64> peak_{0};
    ReservationPlan plan_{};
};

}  // namespace lx::memory
