// LoucuraXXXX GPU - priority work queue.
//
// Passes awaiting placement sit here until the manager has a free executor and
// budget for them. The queue is ordered by priority and then by arrival, and it
// reports depth so the manager can throttle admission before the machine is
// oversubscribed.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

#include "core/types.hpp"

namespace lx::manager {

// One unit of schedulable graphics work.
struct WorkItem {
    u64 id = 0;
    u32 priority = 0;          // higher runs first
    u64 sequence = 0;          // arrival order, breaks priority ties
    u64 working_set_bytes = 0;
    u64 output_bytes = 0;
    u32 independent_units = 1;
    bool requires_gpu_only_feature = false;
    std::string name;

    // Estimated cost, filled in by the manager when the item is enqueued.
    double estimated_cpu_seconds = 0.0;
    double estimated_gpu_seconds = 0.0;
};

class WorkQueue {
public:
    struct Entry {
        WorkItem item;
        bool operator<(const Entry& other) const {
            if (item.priority != other.item.priority) {
                return item.priority < other.item.priority;  // higher first
            }
            return item.sequence > other.item.sequence;  // earlier first
        }
    };

    void push(WorkItem item);
    std::optional<WorkItem> try_pop();
    bool wait_pop(WorkItem& out, u32 timeout_ms);

    std::size_t depth() const;
    // Counters are written under mutex_, so they must be read under it too.
    u64 total_enqueued() const;
    u64 total_dequeued() const;
    u64 total_bytes_enqueued() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::priority_queue<Entry> queue_;
    u64 sequence_ = 0;
    u64 enqueued_ = 0;
    u64 dequeued_ = 0;
    u64 bytes_enqueued_ = 0;
};

}  // namespace lx::manager
