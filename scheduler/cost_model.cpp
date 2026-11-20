#include "scheduler/cost_model.hpp"

#include <algorithm>
#include <cmath>

namespace lx::scheduler {

double CostModel::estimate_cpu(const DispatchRequest& request) const {
    if (request.working_set_bytes == 0) return 0.0;
    // Serial time divided by usable parallelism. The worker count is capped by
    // the number of independent units: a single indivisible pass cannot be
    // parallelised further.
    const double parallel_units =
        std::max(1.0, std::min(constants_.cpu_worker_count,
                               static_cast<double>(request.independent_units)));
    const double serial_seconds =
        static_cast<double>(request.working_set_bytes) / constants_.cpu_throughput_bps;
    return serial_seconds / parallel_units;
}

double CostModel::estimate_gpu(const DispatchRequest& request) const {
    // Unified memory means the working set is already resident for both
    // executors: there is nothing to move and no fixed staging cost. This is
    // what makes GPU dispatch cheap on Apple silicon and integrated GPUs.
    u64 transfer_bytes = 0;
    double transfer_seconds = 0.0;
    if (!constants_.unified_memory) {
        transfer_bytes =
            static_cast<u64>(static_cast<double>(request.working_set_bytes) *
                             (1.0 - constants_.gpu_residency));
        transfer_seconds =
            static_cast<double>(transfer_bytes) * constants_.transfer_seconds_per_byte +
            (transfer_bytes > 0 ? constants_.transfer_fixed_seconds : 0.0) *
                (request.output_needed_on_cpu ? 2.0 : 1.0);
    }

    const double compute_seconds =
        static_cast<double>(request.working_set_bytes) /
        (constants_.gpu_throughput_bps * constants_.gpu_parallelism / 64.0);
    const double dispatch_seconds = constants_.gpu_dispatch_fixed_seconds;

    return transfer_seconds + compute_seconds + dispatch_seconds;
}

DispatchDecision CostModel::decide(const DispatchRequest& request) const {
    DispatchDecision decision;

    if (request.requires_gpu_only_feature) {
        decision.target = DeviceTarget::Gpu;
        decision.rationale = "pass requires a GPU-only feature";
        decision.estimated_gpu_seconds = estimate_gpu(request);
        decision.estimated_cpu_seconds = std::nan("");
        return decision;
    }

    decision.estimated_cpu_seconds = estimate_cpu(request);
    decision.estimated_gpu_seconds = estimate_gpu(request);

    // A split runs the GPU portion and the CPU portion concurrently on
    // disjoint halves. The wall-clock cost is the slower of the two halves plus
    // the transfer of the GPU half.
    DispatchRequest half = request;
    half.working_set_bytes = request.working_set_bytes / 2;
    half.output_bytes = request.output_bytes / 2;
    half.independent_units = std::max(1u, request.independent_units / 2);
    decision.estimated_split_seconds =
        std::max(estimate_cpu(half), estimate_gpu(half));

    // Only consider a split when there is genuinely enough work to amortise the
    // transfer, and the pass can be divided. With unified memory there is no
    // transfer to amortise, so the size floor drops and splitting pays off much
    // sooner.
    const u64 split_floor =
        constants_.unified_memory ? 256 * 1024 : 4 * 1024 * 1024;
    const bool splittable = request.is_embarrassingly_parallel &&
                            request.independent_units >= 4 &&
                            request.working_set_bytes >= split_floor;

    if (splittable && decision.estimated_split_seconds < decision.estimated_cpu_seconds &&
        decision.estimated_split_seconds < decision.estimated_gpu_seconds) {
        decision.target = DeviceTarget::Split;
        decision.rationale = "splitting halves the wall-clock cost of either executor";
    } else if (decision.estimated_gpu_seconds < decision.estimated_cpu_seconds) {
        decision.target = DeviceTarget::Gpu;
        decision.rationale = "GPU estimate is lower and transfer is amortised";
    } else {
        decision.target = DeviceTarget::Cpu;
        decision.rationale = "CPU estimate is lower; transfer would dominate";
    }
    return decision;
}

}  // namespace lx::scheduler
