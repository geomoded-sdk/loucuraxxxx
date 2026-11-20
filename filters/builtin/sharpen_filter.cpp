#include "filters/builtin/sharpen_filter.hpp"

#include <algorithm>
#include <vector>

#include "filters/filter_registry.hpp"
#include "filters/gpu_plan.hpp"

namespace lx::filters {

namespace {
const FilterInfo kInfo{
    "sharpen",
    "Unsharp-mask sharpening",
    FilterTarget::Both,
    Maturity::Implemented,
    1, 2, 1.0f,
};
}  // namespace

const FilterInfo& SharpenFilter::info() const { return kInfo; }

Status SharpenFilter::apply_rows(const ConstImage& source, Image destination,
                                const FilterParams& params, u32 row_begin,
                                u32 row_end) const {
    if (!source.valid() || !destination.valid()) {
        return Status::error(ErrorCode::InvalidArgument, "sharpen: invalid image");
    }
    if (source.width != destination.width || source.height != destination.height) {
        return Status::error(ErrorCode::InvalidArgument,
                             "sharpen: source and destination size mismatch");
    }
    if (row_begin >= row_end || row_end > source.height) {
        return Status::error(ErrorCode::InvalidArgument, "sharpen: invalid row range");
    }
    const f32 strength = amount_ * params.intensity;
    if (strength <= 0.0f) {
        if (source.pixels != destination.pixels) {
            for (u32 y = row_begin; y < row_end; ++y) {
                const u8* src = source.row(y);
                u8* dst = destination.row(y);
                for (u32 x = 0; x < source.width * 4; ++x) dst[x] = src[x];
            }
        }
        return Status::success();
    }

    const u32 w = source.width;
    const u32 h = source.height;
    for (u32 y = row_begin; y < row_end; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const std::size_t out = (static_cast<std::size_t>(y) * w + x) * 4;
            // 3x3 box average of the neighbourhood (clamped at the border).
            f32 avg[3] = {0.0f, 0.0f, 0.0f};
            for (int dy = -1; dy <= 1; ++dy) {
                const int sy = std::clamp(static_cast<int>(y) + dy, 0,
                                          static_cast<int>(h) - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    const int sx = std::clamp(static_cast<int>(x) + dx, 0,
                                              static_cast<int>(w) - 1);
                    const std::size_t idx =
                        (static_cast<std::size_t>(sy) * w + static_cast<u32>(sx)) * 4;
                    avg[0] += source.pixels[idx + 0];
                    avg[1] += source.pixels[idx + 1];
                    avg[2] += source.pixels[idx + 2];
                }
            }
            for (int c = 0; c < 3; ++c) {
                const f32 blurred = avg[static_cast<std::size_t>(c)] / 9.0f;
                const f32 original = source.pixels[out + static_cast<std::size_t>(c)];
                const f32 sharpened = original + strength * (original - blurred);
                destination.pixels[out + static_cast<std::size_t>(c)] = clamp_u8(sharpened);
            }
            destination.pixels[out + 3] = source.pixels[out + 3];
        }
    }
    return Status::success();
}

Status SharpenFilter::apply(const ConstImage& source, Image destination,
                            const FilterParams& params) const {
    return apply_rows(source, destination, params, 0, source.height);
}

GpuFilterPlan SharpenFilter::gpu_plan(u32 width, u32 height,
                                      const FilterParams& params) const {
    push::Sharpen pc;
    pc.strength = amount_ * params.intensity;
    pc.width = static_cast<f32>(width);
    pc.height = static_cast<f32>(height);
    GpuFilterPlan plan;
    plan.supported = true;
    plan.passes.push_back(GpuPass{"sharpen", to_bytes(pc), {0}, 1});
    return plan;
}

namespace {
FilterPtr make_sharpen() { return FilterPtr(new SharpenFilter()); }
}  // namespace

void register_sharpen_filter() {
    static bool done = false;
    if (done) return;
    done = true;
    register_filter(FilterRegistration{kInfo, &make_sharpen});
}

}  // namespace lx::filters
