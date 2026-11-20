// LoucuraXXXX GPU - vignette (radial darkening).
#pragma once

#include "filters/filter.hpp"

namespace lx::filters {

// Darkens the frame toward its corners with a smooth radial falloff. The
// falloff starts at `inner` of the normalised radius and reaches full strength
// at `outer`.
class VignetteFilter : public IFilter {
public:
    const FilterInfo& info() const override;
    Status apply(const ConstImage& source, Image destination,
                 const FilterParams& params) const override;
    Status apply_rows(const ConstImage& source, Image destination,
                      const FilterParams& params, u32 row_begin,
                      u32 row_end) const override;
    bool supports_row_split() const override { return true; }
    bool supports_in_place() const override { return true; }

    GpuFilterPlan gpu_plan(u32 width, u32 height,
                           const FilterParams& params) const override;

    void set_inner(f32 inner) { inner_ = inner; }
    void set_outer(f32 outer) { outer_ = outer; }

private:
    f32 inner_ = 0.45f;
    f32 outer_ = 1.0f;
};

// Registers this filter with the global registry. Called by the registry.
void register_vignette_filter();

}  // namespace lx::filters
