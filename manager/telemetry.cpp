#include "manager/telemetry.hpp"

namespace lx::manager {

void Telemetry::record_dispatch(int device_target) {
    switch (device_target) {
        case 0: cpu_dispatches_.fetch_add(1, std::memory_order_relaxed); break;
        case 1: gpu_dispatches_.fetch_add(1, std::memory_order_relaxed); break;
        default: split_dispatches_.fetch_add(1, std::memory_order_relaxed); break;
    }
}

void Telemetry::record_rejection() {
    rejections_.fetch_add(1, std::memory_order_relaxed);
}

void Telemetry::record_queue_depth(u64 depth) {
    queue_depth_.store(depth, std::memory_order_relaxed);
    u64 peak = peak_queue_depth_.load(std::memory_order_relaxed);
    while (depth > peak &&
           !peak_queue_depth_.compare_exchange_weak(peak, depth,
                                                    std::memory_order_relaxed)) {
    }
}

void Telemetry::record_transfer(u64 to_gpu_bytes, u64 to_cpu_bytes) {
    bytes_to_gpu_.fetch_add(to_gpu_bytes, std::memory_order_relaxed);
    bytes_to_cpu_.fetch_add(to_cpu_bytes, std::memory_order_relaxed);
}

void Telemetry::record_residency_avoided(u64 bytes) {
    residency_avoided_.fetch_add(bytes, std::memory_order_relaxed);
}

void Telemetry::set_utilization(double cpu, double gpu, double memory, bool gpu_available,
                               double cpu_worker_utilization) {
    std::lock_guard<std::mutex> lock(utilization_mutex_);
    cpu_queue_pressure_ = cpu;
    cpu_worker_utilization_ = cpu_worker_utilization;
    gpu_utilization_ = gpu;
    gpu_utilization_available_ = gpu_available;
    memory_utilization_ = memory;
}

void Telemetry::set_budget(u64 limit, u64 used, u64 peak) {
    limit_.store(limit, std::memory_order_relaxed);
    used_.store(used, std::memory_order_relaxed);
    peak_.store(peak, std::memory_order_relaxed);
}

TelemetrySnapshot Telemetry::snapshot() const {
    TelemetrySnapshot snapshot;
    snapshot.timestamp_micros = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    snapshot.passes_dispatched_to_cpu = cpu_dispatches_.load(std::memory_order_relaxed);
    snapshot.passes_dispatched_to_gpu = gpu_dispatches_.load(std::memory_order_relaxed);
    snapshot.passes_dispatched_split = split_dispatches_.load(std::memory_order_relaxed);
    snapshot.passes_rejected = rejections_.load(std::memory_order_relaxed);
    snapshot.queue_depth = queue_depth_.load(std::memory_order_relaxed);
    snapshot.peak_queue_depth = peak_queue_depth_.load(std::memory_order_relaxed);
    snapshot.bytes_transferred_to_gpu = bytes_to_gpu_.load(std::memory_order_relaxed);
    snapshot.bytes_transferred_to_cpu = bytes_to_cpu_.load(std::memory_order_relaxed);
    snapshot.bytes_avoided_by_residency = residency_avoided_.load(std::memory_order_relaxed);
    snapshot.budget_limit_bytes = limit_.load(std::memory_order_relaxed);
    snapshot.budget_used_bytes = used_.load(std::memory_order_relaxed);
    snapshot.budget_peak_bytes = peak_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(utilization_mutex_);
        snapshot.cpu_queue_pressure = cpu_queue_pressure_;
        snapshot.cpu_worker_utilization = cpu_worker_utilization_;
        snapshot.gpu_utilization_available = gpu_utilization_available_;
        snapshot.gpu_utilization = gpu_utilization_;
        snapshot.memory_utilization = memory_utilization_;
    }
    return snapshot;
}

void Telemetry::reset() {
    cpu_dispatches_.store(0);
    gpu_dispatches_.store(0);
    split_dispatches_.store(0);
    rejections_.store(0);
    queue_depth_.store(0);
    peak_queue_depth_.store(0);
    bytes_to_gpu_.store(0);
    bytes_to_cpu_.store(0);
    residency_avoided_.store(0);
    limit_.store(0);
    used_.store(0);
    peak_.store(0);
    std::lock_guard<std::mutex> lock(utilization_mutex_);
    cpu_queue_pressure_ = 0.0;
    cpu_worker_utilization_ = 0.0;
    gpu_utilization_available_ = false;
    gpu_utilization_ = 0.0;
    memory_utilization_ = 0.0;
}

}  // namespace lx::manager
