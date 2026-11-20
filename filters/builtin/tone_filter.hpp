// LoucuraXXXX GPU - tone adjustment filter (brightness / contrast / saturation).
#pragma once

#include "filters/filter.hpp"

namespace lx::filters {

// Applies a colour-grade: exposure in stops, a contrast curve around mid-grey,
// and a saturation blend. All three are interpolated by `intensity`, so
// intensity 0 is always an exact no-op.
class ToneFilter : public IFilter {
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

    // -2..+2 stops.
    void set_exposure_stops(f32 stops) { exposure_stops_ = stops; }
    // -1..+1, 0 is neutral.
    void set_contrast(f32 contrast) { contrast_ = contrast; }
    // 0..2, 1 is neutral.
    void set_saturation(f32 saturation) { saturation_ = saturation; }

private:
    f32 exposure_stops_ = 0.0f;
    f32 contrast_ = 0.0f;
    f32 saturation_ = 1.0f;
};

// Registers this filter with the global registry. Called by the registry.
void register_tone_filter();

}  // namespace lx::filters
