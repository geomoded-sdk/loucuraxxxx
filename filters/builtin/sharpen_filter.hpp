// LoucuraXXXX GPU - unsharp-mask sharpening.
#pragma once

#include "filters/filter.hpp"

namespace lx::filters {

// Sharpens by subtracting a blurred copy (unsharp mask): out = src + amount *
// (src - blur(src)). The blur is a small 3x3 box for the fast path; amount is
// scaled by intensity so intensity 0 is a no-op.
class SharpenFilter : public IFilter {
public:
    const FilterInfo& info() const override;
    Status apply(const ConstImage& source, Image destination,
                 const FilterParams& params) const override;
    Status apply_rows(const ConstImage& source, Image destination,
                      const FilterParams& params, u32 row_begin,
                      u32 row_end) const override;
    bool supports_row_split() const override { return true; }
    bool supports_in_place() const override { return false; }

    GpuFilterPlan gpu_plan(u32 width, u32 height,
                           const FilterParams& params) const override;

    void set_amount(f32 amount) { amount_ = amount; }

private:
    f32 amount_ = 0.8f;
};

// Registers this filter with the global registry. Called by the registry.
void register_sharpen_filter();

}  // namespace lx::filters
