// LoucuraXXXX GPU - separable Gaussian blur.
#pragma once

#include "filters/filter.hpp"

namespace lx::filters {

// A true separable Gaussian blur: a horizontal pass followed by a vertical
// pass, each with a normalised kernel of radius = ceil(3 * sigma). The radius
// is derived from `quality` so the cost is predictable.
class BlurFilter : public IFilter {
public:
    const FilterInfo& info() const override;
    Status apply(const ConstImage& source, Image destination,
                 const FilterParams& params) const override;
    Status apply_rows(const ConstImage& source, Image destination,
                      const FilterParams& params, u32 row_begin,
                      u32 row_end) const override;
    bool supports_row_split() const override { return true; }
    // A blur reads neighbours, so it cannot run in place.
    bool supports_in_place() const override { return false; }

    GpuFilterPlan gpu_plan(u32 width, u32 height,
                           const FilterParams& params) const override;

    // Base sigma in pixels at "balanced" quality.
    void set_sigma(f32 sigma) { sigma_ = sigma; }

private:
    f32 sigma_ = 2.0f;
};

// Registers this filter with the global registry. Called by the registry.
void register_blur_filter();

}  // namespace lx::filters
