// LoucuraXXXX GPU - dependency-ordered task graph.
//
// A render frame is a graph of passes: some run in order, some are independent
// and may run concurrently. The graph tracks how many predecessors each node
// still waits on and submits a node the moment that count reaches zero, which
// extracts the frame's available parallelism without a global barrier per pass.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "scheduler/thread_pool.hpp"

namespace lx::scheduler {

class TaskGraph {
public:
    using NodeId = u32;
    static constexpr NodeId kInvalidNode = 0xFFFFFFFFu;

    explicit TaskGraph(ThreadPool& pool) : pool_(pool) {}
    ~TaskGraph();

    // The graph owns its nodes and is referenced by the pool's running tasks,
    // so it must not be copied or moved: a copy would double-own every node.
    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;
    TaskGraph(TaskGraph&&) = delete;
    TaskGraph& operator=(TaskGraph&&) = delete;

    // Adds a node with no dependencies. Returns its id.
    NodeId add_node(std::string name, ThreadPool::Task task);

    // Declares that `node` must wait for `dependency` to complete.
    // Fails on unknown ids or on an edge that would create a cycle.
    Status add_dependency(NodeId node, NodeId dependency);

    // Submits all nodes whose dependencies are already satisfied and returns
    // the number of nodes dispatched. Safe to call repeatedly.
    u32 submit_ready();

    // Submits ready nodes and blocks until the whole graph has run.
    Status run_to_completion();

    // Clears the graph for reuse. Must not be called while running.
    void clear();

    std::size_t node_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return nodes_.size();
    }
    bool complete() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return completed_.load() == nodes_.size();
    }

private:
    struct Node {
        std::string name;
        ThreadPool::Task task;
        std::vector<NodeId> dependencies;
        std::atomic<u32> remaining{0};
        std::atomic<bool> submitted{false};
        std::atomic<bool> finished{false};
    };

    bool reaches(NodeId from, NodeId target) const;

    ThreadPool& pool_;
    std::vector<Node*> nodes_;  // owned
    std::atomic<u64> completed_{0};
    mutable std::mutex mutex_;
};

}  // namespace lx::scheduler
