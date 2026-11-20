#include "filters/builtin/blur_filter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "filters/filter_registry.hpp"
#include "filters/gpu_plan.hpp"

namespace lx::filters {

namespace {

const FilterInfo kInfo{
    "blur",
    "Separable Gaussian blur with quality-controlled radius",
    FilterTarget::Both,
    Maturity::Implemented,
    1, 3, 1.0f,
};

f32 quality_scale(const std::string& quality) {
    if (quality == "fast") return 0.5f;
    if (quality == "quality") return 1.75f;
    return 1.0f;  // balanced
}

// Builds a normalised 1D Gaussian kernel for the given sigma.
std::vector<f32> build_kernel(f32 sigma) {
    const int radius = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
    std::vector<f32> kernel(static_cast<std::size_t>(radius * 2 + 1));
    const f32 denom = 2.0f * sigma * sigma;
    f32 sum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        const f32 value = std::exp(-static_cast<f32>(i * i) / denom);
        kernel[static_cast<std::size_t>(i + radius)] = value;
        sum += value;
    }
    for (f32& value : kernel) value /= sum;
    return kernel;
}

// One separable pass over the output rows [row_begin, row_end).
//
// `src_base_row` is the global row index of src's first row and `dst_base_row`
// the same for dst, so a band can write into a buffer that starts at a different
// global row than the source. The whole of src is readable (a vertical pass
// reaches `radius` rows outside the output band), which is what lets bands run
// independently. Edge pixels clamp to the border sample, which avoids darkening
// at the frame edge.
void blur_pass(const u8* src, u32 src_base_row, u8* dst, u32 dst_base_row,
               u32 width, u32 height, const std::vector<f32>& kernel,
               bool horizontal, f32 blend, u32 row_begin, u32 row_end) {
    const int radius = static_cast<int>(kernel.size() / 2);
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    for (u32 y = row_begin; y < row_end; ++y) {
        const u8* src_row = src + (static_cast<std::size_t>(y - src_base_row) * row_bytes);
        u8* dst_row = dst + (static_cast<std::size_t>(y - dst_base_row) * row_bytes);
        for (u32 x = 0; x < width; ++x) {
            f32 accum[3] = {0.0f, 0.0f, 0.0f};
            for (int k = -radius; k <= radius; ++k) {
                int sx = static_cast<int>(x);
                int sy = static_cast<int>(y);
                if (horizontal) sx = std::clamp(sx + k, 0, static_cast<int>(width) - 1);
                else sy = std::clamp(sy + k, 0, static_cast<int>(height) - 1);
                const u8* tap =
                    src + (static_cast<std::size_t>(sy - src_base_row) * row_bytes);
                const std::size_t idx = static_cast<std::size_t>(sx) * 4;
                const f32 w = kernel[static_cast<std::size_t>(k + radius)];
                accum[0] += w * static_cast<f32>(tap[idx + 0]);
                accum[1] += w * static_cast<f32>(tap[idx + 1]);
                accum[2] += w * static_cast<f32>(tap[idx + 2]);
            }
            const std::size_t out = static_cast<std::size_t>(x) * 4;
            for (int c = 0; c < 3; ++c) {
                const std::size_t o = out + static_cast<std::size_t>(c);
                dst_row[o] = clamp_u8(static_cast<f32>(src_row[o]) +
                                      (accum[c] - static_cast<f32>(src_row[o])) * blend);
            }
            dst_row[out + 3] = src_row[out + 3];
        }
    }
}

}  // namespace

const FilterInfo& BlurFilter::info() const { return kInfo; }

Status BlurFilter::apply_rows(const ConstImage& source, Image destination,
                             const FilterParams& params, u32 row_begin,
                             u32 row_end) const {
    if (!source.valid() || !destination.valid()) {
        return Status::error(ErrorCode::InvalidArgument, "blur: invalid image");
    }
    if (source.width != destination.width || source.height != destination.height) {
        return Status::error(ErrorCode::InvalidArgument,
                             "blur: source and destination size mismatch");
    }
    if (row_begin >= row_end || row_end > source.height) {
        return Status::error(ErrorCode::InvalidArgument, "blur: invalid row range");
    }
    if (params.intensity <= 0.0f) {
        if (source.pixels != destination.pixels) {
            const std::size_t row_bytes = static_cast<std::size_t>(source.width) * 4;
            for (u32 y = row_begin; y < row_end; ++y) {
                std::memcpy(destination.row(y), source.row(y), row_bytes);
            }
        }
        return Status::success();
    }

    const f32 sigma = sigma_ * quality_scale(params.quality);
    const std::vector<f32> kernel = build_kernel(sigma);
    const int radius = static_cast<int>(kernel.size() / 2);
    const u32 width = source.width;
    const u32 height = source.height;

    // A band is independent because the vertical pass only reads rows within
    // `radius` of the rows it writes. Recompute those halo rows locally so no
    // band depends on another band's output.
    const u32 halo_begin = row_begin > static_cast<u32>(radius) ? row_begin - radius : 0;
    const u32 halo_end =
        std::min(height, row_end + static_cast<u32>(radius));
    std::vector<u8> scratch(static_cast<std::size_t>(halo_end - halo_begin) * width * 4, 0);

    // Pass 1: horizontal, source rows [halo_begin, halo_end) -> scratch.
    // source.pixels starts at global row 0; scratch starts at global row
    // halo_begin, so the two base rows differ.
    blur_pass(source.pixels, 0, scratch.data(), halo_begin, width, height,
              kernel, true, 1.0f, halo_begin, halo_end);
    // Pass 2: vertical, scratch -> destination rows [row_begin, row_end).
    blur_pass(scratch.data(), halo_begin, destination.pixels, 0, width, height, kernel,
              false, params.intensity, row_begin, row_end);
    return Status::success();
}

Status BlurFilter::apply(const ConstImage& source, Image destination,
                         const FilterParams& params) const {
    return apply_rows(source, destination, params, 0, source.height);
}

GpuFilterPlan BlurFilter::gpu_plan(u32 width, u32 height,
                                   const FilterParams& params) const {
    const f32 sigma = sigma_ * quality_scale(params.quality);
    const int radius = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
    push::Blur horizontal;
    horizontal.sigma = sigma;
    horizontal.intensity = 1.0f;  // the horizontal pass is always full strength
    horizontal.width = static_cast<f32>(width);
    horizontal.height = static_cast<f32>(height);
    horizontal.axis = 0;
    horizontal.radius = radius;
    push::Blur vertical = horizontal;
    vertical.axis = 1;
    vertical.intensity = params.intensity;  // the blend is applied on the last pass

    GpuFilterPlan plan;
    plan.supported = true;
    // Horizontal into a scratch texture, then vertical into the real output.
    plan.scratch_textures = 1;
    plan.passes.push_back(GpuPass{"blur_horizontal", to_bytes(horizontal), {0}, 2});
    plan.passes.push_back(GpuPass{"blur_vertical", to_bytes(vertical), {2}, 1});
    return plan;
}

namespace {
FilterPtr make_blur() { return FilterPtr(new BlurFilter()); }
}  // namespace

void register_blur_filter() {
    static bool done = false;
    if (done) return;
    done = true;
    register_filter(FilterRegistration{kInfo, &make_blur});
}

}  // namespace lx::filters
