// LoucuraXXXX GPU - bloom (bright-pass, blur, additive combine).
#pragma once

#include "filters/filter.hpp"

namespace lx::filters {

// Extracts pixels above a luminance threshold, blurs them, and adds the result
// back. This is a lighting effect: it makes genuinely bright regions glow
// without washing out the rest of the image.
class BloomFilter : public IFilter {
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

    void set_threshold(f32 threshold) { threshold_ = threshold; }
    void set_radius(f32 radius) { radius_ = radius; }

private:
    f32 threshold_ = 200.0f;
    f32 radius_ = 4.0f;
};

// Registers this filter with the global registry. Called by the registry.
void register_bloom_filter();

}  // namespace lx::filters
