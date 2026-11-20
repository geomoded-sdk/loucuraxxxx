#include "filters/builtin/tone_filter.hpp"

#include <cmath>

#include "filters/filter_registry.hpp"
#include "filters/gpu_plan.hpp"

namespace lx::filters {

namespace {
const FilterInfo kInfo{
    "tone",
    "Exposure, contrast and saturation colour grade",
    FilterTarget::Both,
    Maturity::Implemented,
    1, 1, 1.0f,
};
}  // namespace

const FilterInfo& ToneFilter::info() const { return kInfo; }

Status ToneFilter::apply_rows(const ConstImage& source, Image destination,
                             const FilterParams& params, u32 row_begin,
                             u32 row_end) const {
    if (!source.valid() || !destination.valid()) {
        return Status::error(ErrorCode::InvalidArgument, "tone: invalid image");
    }
    if (source.width != destination.width || source.height != destination.height) {
        return Status::error(ErrorCode::InvalidArgument,
                             "tone: source and destination size mismatch");
    }
    if (row_begin >= row_end || row_end > source.height) {
        return Status::error(ErrorCode::InvalidArgument, "tone: invalid row range");
    }
    const f32 blend = params.intensity;
    if (blend <= 0.0f) {
        // Exact no-op: copy through so the chain contract still holds.
        if (source.pixels != destination.pixels) {
            for (u32 y = row_begin; y < row_end; ++y) {
                const u8* src = source.row(y);
                u8* dst = destination.row(y);
                for (u32 x = 0; x < source.width * 4; ++x) dst[x] = src[x];
            }
        }
        return Status::success();
    }

    const f32 gain = std::pow(2.0f, exposure_stops_);
    // Contrast as a linear push around mid-grey (0.5 in normalised space).
    const f32 contrast = 1.0f + contrast_;
    const f32 saturation = saturation_;

    for (u32 y = row_begin; y < row_end; ++y) {
        const u8* src = source.row(y);
        u8* dst = destination.row(y);
        for (u32 x = 0; x < source.width; ++x) {
            const std::size_t i = static_cast<std::size_t>(x) * 4;
            f32 r = static_cast<f32>(src[i + 0]);
            f32 g = static_cast<f32>(src[i + 1]);
            f32 b = static_cast<f32>(src[i + 2]);

            r *= gain; g *= gain; b *= gain;
            r = (r - 128.0f) * contrast + 128.0f;
            g = (g - 128.0f) * contrast + 128.0f;
            b = (b - 128.0f) * contrast + 128.0f;

            const f32 luma = luminance(clamp_u8(r), clamp_u8(g), clamp_u8(b));
            r = luma + (r - luma) * saturation;
            g = luma + (g - luma) * saturation;
            b = luma + (b - luma) * saturation;

            // Blend toward the graded value by intensity.
            dst[i + 0] = clamp_u8(static_cast<f32>(src[i + 0]) + (r - src[i + 0]) * blend);
            dst[i + 1] = clamp_u8(static_cast<f32>(src[i + 1]) + (g - src[i + 1]) * blend);
            dst[i + 2] = clamp_u8(static_cast<f32>(src[i + 2]) + (b - src[i + 2]) * blend);
            dst[i + 3] = src[i + 3];
        }
    }
    return Status::success();
}

Status ToneFilter::apply(const ConstImage& source, Image destination,
                         const FilterParams& params) const {
    return apply_rows(source, destination, params, 0, source.height);
}

GpuFilterPlan ToneFilter::gpu_plan(u32 width, u32 height,
                                   const FilterParams& params) const {
    (void)width;
    (void)height;
    push::Tone pc;
    pc.exposure_stops = exposure_stops_;
    pc.contrast = contrast_;
    pc.saturation = saturation_;
    pc.intensity = params.intensity;
    GpuFilterPlan plan;
    plan.supported = true;
    plan.passes.push_back(GpuPass{"tone", to_bytes(pc), {0}, 1});
    return plan;
}

namespace {
FilterPtr make_tone() { return FilterPtr(new ToneFilter()); }
}  // namespace

void register_tone_filter() {
    static bool done = false;
    if (done) return;
    done = true;
    register_filter(FilterRegistration{kInfo, &make_tone});
}

}  // namespace lx::filters
