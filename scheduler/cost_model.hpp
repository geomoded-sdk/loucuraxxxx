// LoucuraXXXX GPU - hybrid dispatch cost model.
//
// The central question of the project is: given a piece of work, should it run
// on the physical GPU, on the CPU workers, or be split between them? Guessing
// is not acceptable, so the decision is made from measured constants and a
// first-order cost estimate. The constants are calibrated by the benchmark
// harness (benchmarks/calibrate) rather than hard-coded, and start at
// conservative defaults so an uncalibrated build still behaves sensibly.
#pragma once

#include <string>

#include "core/types.hpp"

namespace lx::scheduler {

// Where a unit of work should execute.
enum class DeviceTarget { Cpu, Gpu, Split };

inline const char* to_string(DeviceTarget t) {
    switch (t) {
        case DeviceTarget::Cpu: return "cpu";
        case DeviceTarget::Gpu: return "gpu";
        case DeviceTarget::Split: return "split";
    }
    return "cpu";
}

// Measured or assumed cost characteristics of the two executors.
struct CostConstants {
    // Throughput in bytes processed per second for a memory-bound pass.
    double cpu_throughput_bps = 3.0e9;   // conservative; refined by calibration
    double gpu_throughput_bps = 20.0e9;  // conservative; refined by calibration

    // One-way transfer cost per byte and fixed per-transfer overhead. These
    // dominate small workloads, which is why tiny tasks stay on the CPU.
    double transfer_seconds_per_byte = 1.0 / 12.0e9;
    double transfer_fixed_seconds = 40.0e-6;  // staging + submission + fence

    // Cost of dispatching a GPU command buffer (fence, queue submit).
    double gpu_dispatch_fixed_seconds = 25.0e-6;

    // Effective parallelism on each side. GPU is a wide SIMD machine; CPU has
    // `cpu_worker_count` cores but realistically saturates below that.
    double gpu_parallelism = 256.0;
    double cpu_worker_count = 4.0;

    // How much of the working set already lives in GPU-local memory. 0 means
    // everything must be transferred, 1 means no transfer is needed.
    double gpu_residency = 0.0;

    // True when CPU and GPU share one physical memory pool (Apple silicon, most
    // integrated GPUs). When set, the model charges no transfer cost at all:
    // the data is already where the GPU needs it, and the GPU result is already
    // where the CPU needs it. This is the single biggest lever the layer has,
    // and it is a property of the host, not a tunable.
    bool unified_memory = false;
};

// A request to place one unit of work.
struct DispatchRequest {
    u64 working_set_bytes = 0;   // bytes touched (read+write) by the pass
    u64 output_bytes = 0;        // bytes produced (0 => in-place)
    bool is_embarrassingly_parallel = true;
    bool requires_gpu_only_feature = false;  // e.g. a hardware sampler
    bool output_needed_on_cpu = true;        // whether results come back
    u32 independent_units = 1;               // number of separable chunks
};

// The model's answer, including the estimated wall-clock cost of each option so
// the decision is explainable rather than opaque.
struct DispatchDecision {
    DeviceTarget target = DeviceTarget::Cpu;
    double estimated_cpu_seconds = 0.0;
    double estimated_gpu_seconds = 0.0;
    double estimated_split_seconds = 0.0;
    std::string rationale;
};

class CostModel {
public:
    CostModel() = default;
    explicit CostModel(CostConstants constants) : constants_(constants) {}

    DispatchDecision decide(const DispatchRequest& request) const;

    const CostConstants& constants() const { return constants_; }
    void set_constants(const CostConstants& constants) { constants_ = constants; }

private:
    double estimate_cpu(const DispatchRequest& request) const;
    double estimate_gpu(const DispatchRequest& request) const;

    CostConstants constants_{};
};

}  // namespace lx::scheduler
