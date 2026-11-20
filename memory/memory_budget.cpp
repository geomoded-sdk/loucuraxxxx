#include "memory/memory_budget.hpp"

#include <algorithm>

#include "core/log.hpp"

namespace lx::memory {

ReservationPlan MemoryPolicy::plan(const hardware::MemoryInfo& memory,
                                   u64 requested_bytes) const {
    ReservationPlan plan;
    plan.system_total_bytes = memory.total_bytes;
    plan.system_available_bytes = memory.available_bytes;
    plan.requested_bytes = requested_bytes;

    if (memory.total_bytes == 0) {
        plan.reason = "system memory unknown; reservation denied";
        plan.granted_bytes = 0;
        plan.clamped = requested_bytes > 0;
        return plan;
    }

    // Headroom the OS keeps: the larger of the fractional and absolute rules.
    // The product can exceed u64 (or be NaN for a nonsensical fraction), and
    // casting such a double to u64 is UB, so clamp to the total first.
    const double fractional = static_cast<double>(memory.total_bytes) * fraction_;
    u64 by_fraction = 0;
    if (fractional > 0.0) {
        by_fraction = fractional >= static_cast<double>(memory.total_bytes)
                          ? memory.total_bytes
                          : static_cast<u64>(fractional);
    }
    plan.os_reserve_bytes = std::max(by_fraction, minimum_free_);
    if (plan.os_reserve_bytes >= memory.total_bytes) {
        plan.os_reserve_bytes = memory.total_bytes / 2;  // never reserve everything
    }

    // Upper bound on what the virtual GPU may ever hold: everything that is
    // not the OS reserve, further capped by what is *currently* available.
    const u64 hard_ceiling = memory.total_bytes - plan.os_reserve_bytes;
    const u64 currently_free = memory.available_bytes > plan.os_reserve_bytes
                                   ? memory.available_bytes - plan.os_reserve_bytes
                                   : 0;
    const u64 safe_ceiling = std::min(hard_ceiling, currently_free);

    if (requested_bytes <= safe_ceiling) {
        plan.granted_bytes = requested_bytes;
    } else {
        plan.granted_bytes = safe_ceiling;
        plan.clamped = true;
        if (currently_free < hard_ceiling) {
            plan.reason = "limited by currently available memory";
        } else {
            plan.reason = "limited by OS reserve policy";
        }
    }
    return plan;
}

void MemoryBudget::configure(const ReservationPlan& plan) {
    plan_ = plan;
    set_limit(plan.granted_bytes);
}

void MemoryBudget::set_limit(u64 bytes) {
    limit_.store(bytes, std::memory_order_relaxed);
    const u64 current = used_.load(std::memory_order_relaxed);
    if (current > bytes) {
        // Shrinking below the in-use amount: available() reports zero and new
        // commits are refused until allocations are released.
        LX_WARN << "memory budget lowered below current usage (" << current << " > "
                << bytes << ")";
    }
}

Status MemoryBudget::commit(u64 bytes, const std::string& owner) {
    if (bytes == 0) return Status::success();
    u64 current = used_.load(std::memory_order_relaxed);
    const u64 lim = limit_.load(std::memory_order_relaxed);
    while (true) {
        // Overflow-safe: `current + bytes` could wrap for an absurd request and
        // slip past the check, so compare against the remaining headroom instead.
        const u64 headroom = current < lim ? lim - current : 0;
        if (bytes > headroom) {
            return Status::error(ErrorCode::OutOfMemory,
                                 "budget exhausted committing " + std::to_string(bytes) +
                                     " bytes for '" + owner + "' (" +
                                     std::to_string(current) + "/" + std::to_string(lim) +
                                     " used)");
        }
        if (used_.compare_exchange_weak(current, current + bytes,
                                        std::memory_order_acq_rel)) {
            break;
        }
    }
    u64 peak = peak_.load(std::memory_order_relaxed);
    const u64 now = used_.load(std::memory_order_relaxed);
    while (now > peak &&
           !peak_.compare_exchange_weak(peak, now, std::memory_order_acq_rel)) {
    }
    return Status::success();
}

void MemoryBudget::release(u64 bytes) {
    if (bytes == 0) return;
    u64 current = used_.load(std::memory_order_relaxed);
    while (true) {
        const u64 next = current >= bytes ? current - bytes : 0;
        if (used_.compare_exchange_weak(current, next, std::memory_order_acq_rel)) break;
    }
}

}  // namespace lx::memory
