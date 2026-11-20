#include "filters/builtin/vignette_filter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "filters/filter_registry.hpp"
#include "filters/gpu_plan.hpp"

namespace lx::filters {

namespace {
const FilterInfo kInfo{
    "vignette",
    "Radial edge darkening with a smooth falloff",
    FilterTarget::Both,
    Maturity::Implemented,
    1, 1, 1.0f,
};
}  // namespace

const FilterInfo& VignetteFilter::info() const { return kInfo; }

Status VignetteFilter::apply_rows(const ConstImage& source, Image destination,
                                  const FilterParams& params, u32 row_begin,
                                  u32 row_end) const {
    if (!source.valid() || !destination.valid()) {
        return Status::error(ErrorCode::InvalidArgument, "vignette: invalid image");
    }
    if (source.width != destination.width || source.height != destination.height) {
        return Status::error(ErrorCode::InvalidArgument,
                             "vignette: source and destination size mismatch");
    }
    if (row_begin >= row_end || row_end > source.height) {
        return Status::error(ErrorCode::InvalidArgument, "vignette: invalid row range");
    }
    if (params.intensity <= 0.0f) {
        if (source.pixels != destination.pixels) {
            for (u32 y = row_begin; y < row_end; ++y) {
                const u8* src = source.row(y);
                u8* dst = destination.row(y);
                for (u32 x = 0; x < source.width * 4; ++x) dst[x] = src[x];
            }
        }
        return Status::success();
    }

    const f32 cx = static_cast<f32>(source.width) * 0.5f;
    const f32 cy = static_cast<f32>(source.height) * 0.5f;
    // Normalise by the distance to a corner so the falloff spans the frame.
    const f32 max_radius = std::sqrt(cx * cx + cy * cy);
    const f32 inner = std::min(inner_, outer_);
    const f32 outer = std::max(outer_, inner + 1e-3f);

    for (u32 y = row_begin; y < row_end; ++y) {
        const u8* src = source.row(y);
        u8* dst = destination.row(y);
        for (u32 x = 0; x < source.width; ++x) {
            const f32 dx = static_cast<f32>(x) - cx;
            const f32 dy = static_cast<f32>(y) - cy;
            const f32 radius = std::sqrt(dx * dx + dy * dy) / max_radius;
            const f32 t = std::clamp((radius - inner) / (outer - inner), 0.0f, 1.0f);
            // Smoothstep falloff, so the darkening has no hard edge.
            const f32 falloff = t * t * (3.0f - 2.0f * t);
            const f32 factor = 1.0f - falloff * params.intensity;
            const std::size_t o = static_cast<std::size_t>(x) * 4;
            dst[o + 0] = clamp_u8(static_cast<f32>(src[o + 0]) * factor);
            dst[o + 1] = clamp_u8(static_cast<f32>(src[o + 1]) * factor);
            dst[o + 2] = clamp_u8(static_cast<f32>(src[o + 2]) * factor);
            dst[o + 3] = src[o + 3];
        }
    }
    return Status::success();
}

Status VignetteFilter::apply(const ConstImage& source, Image destination,
                             const FilterParams& params) const {
    return apply_rows(source, destination, params, 0, source.height);
}

GpuFilterPlan VignetteFilter::gpu_plan(u32 width, u32 height,
                                       const FilterParams& params) const {
    push::Vignette pc;
    pc.inner = inner_;
    pc.outer = outer_;
    pc.intensity = params.intensity;
    pc.width = static_cast<f32>(width);
    pc.height = static_cast<f32>(height);
    GpuFilterPlan plan;
    plan.supported = true;
    plan.passes.push_back(GpuPass{"vignette", to_bytes(pc), {0}, 1});
    return plan;
}

namespace {
FilterPtr make_vignette() { return FilterPtr(new VignetteFilter()); }
}  // namespace

void register_vignette_filter() {
    static bool done = false;
    if (done) return;
    done = true;
    register_filter(FilterRegistration{kInfo, &make_vignette});
}

}  // namespace lx::filters
