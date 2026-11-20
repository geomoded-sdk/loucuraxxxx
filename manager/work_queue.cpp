#include "manager/work_queue.hpp"

#include <chrono>

namespace lx::manager {

void WorkQueue::push(WorkItem item) {
    std::lock_guard<std::mutex> lock(mutex_);
    item.sequence = sequence_++;
    ++enqueued_;
    bytes_enqueued_ += item.working_set_bytes;
    queue_.push(Entry{std::move(item)});
    cv_.notify_one();
}

std::optional<WorkItem> WorkQueue::try_pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return std::nullopt;
    WorkItem item = queue_.top().item;
    queue_.pop();
    ++dequeued_;
    return item;
}

bool WorkQueue::wait_pop(WorkItem& out, u32 timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [this] { return !queue_.empty(); })) {
        return false;
    }
    out = queue_.top().item;
    queue_.pop();
    ++dequeued_;
    return true;
}

std::size_t WorkQueue::depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

u64 WorkQueue::total_enqueued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enqueued_;
}

u64 WorkQueue::total_dequeued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dequeued_;
}

u64 WorkQueue::total_bytes_enqueued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_enqueued_;
}

void WorkQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_ = {};
}

}  // namespace lx::manager
