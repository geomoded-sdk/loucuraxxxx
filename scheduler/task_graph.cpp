#include "scheduler/task_graph.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>

#include "core/log.hpp"

namespace lx::scheduler {

TaskGraph::~TaskGraph() { clear(); }

TaskGraph::NodeId TaskGraph::add_node(std::string name, ThreadPool::Task task) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* node = new Node();
    node->name = std::move(name);
    node->task = std::move(task);
    nodes_.push_back(node);
    return static_cast<NodeId>(nodes_.size() - 1);
}

bool TaskGraph::reaches(NodeId from, NodeId target) const {
    // Depth-first search over forward edges to detect a cycle before inserting
    // an edge target -> from.
    std::vector<bool> visited(nodes_.size(), false);
    std::deque<NodeId> stack{from};
    while (!stack.empty()) {
        const NodeId current = stack.back();
        stack.pop_back();
        if (current == target) return true;
        if (current >= nodes_.size() || visited[current]) continue;
        visited[current] = true;
        for (const NodeId dep : nodes_[current]->dependencies) {
            stack.push_back(dep);
        }
    }
    return false;
}

Status TaskGraph::add_dependency(NodeId node, NodeId dependency) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (node >= nodes_.size() || dependency >= nodes_.size()) {
        return Status::error(ErrorCode::OutOfRange, "task graph node id out of range");
    }
    if (node == dependency) {
        return Status::error(ErrorCode::InvalidArgument, "a node cannot depend on itself");
    }
    // Adding node -> dependency closes a cycle if `node` is already reachable
    // from `dependency`.
    if (reaches(dependency, node)) {
        return Status::error(ErrorCode::InvalidArgument,
                             "dependency edge would create a cycle");
    }
    nodes_[node]->dependencies.push_back(dependency);
    nodes_[node]->remaining.fetch_add(1, std::memory_order_relaxed);
    return Status::success();
}

u32 TaskGraph::submit_ready() {
    u32 dispatched = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        Node* node = nodes_[i];
        if (node->submitted.load(std::memory_order_acquire)) continue;
        if (node->remaining.load(std::memory_order_acquire) != 0) continue;

        bool expected = false;
        if (!node->submitted.compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel)) {
            continue;
        }
        ++dispatched;
        // When this node finishes, decrement successors' counters and try to
        // release anything that becomes ready.
        NodeId id = static_cast<NodeId>(i);
        pool_.submit([this, node, id] {
            if (node->task) node->task();
            node->finished.store(true, std::memory_order_release);
            completed_.fetch_add(1, std::memory_order_acq_rel);

            // Release dependents, then dispatch anything that became ready.
            {
                std::lock_guard<std::mutex> guard(mutex_);
                for (Node* candidate : nodes_) {
                    for (NodeId dep : candidate->dependencies) {
                        if (dep == id) {
                            candidate->remaining.fetch_sub(1, std::memory_order_acq_rel);
                        }
                    }
                }
            }
            submit_ready();
        }, id);
    }
    return dispatched;
}

Status TaskGraph::run_to_completion() {
    const u32 dispatched = submit_ready();
    if (dispatched == 0 && nodes_.size() > 0 && completed_.load() == 0) {
        return Status::error(ErrorCode::InvalidArgument,
                             "task graph has nodes but none are runnable (cycle?)");
    }
    // Wait for all nodes to finish.
    while (completed_.load(std::memory_order_acquire) < nodes_.size()) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return Status::success();
}

void TaskGraph::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Node* node : nodes_) delete node;
    nodes_.clear();
    completed_.store(0);
}

}  // namespace lx::scheduler
