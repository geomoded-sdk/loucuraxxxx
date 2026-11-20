#include "filters/builtin/bloom_filter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "filters/filter_registry.hpp"
#include "filters/gpu_plan.hpp"

namespace lx::filters {

namespace {

const FilterInfo kInfo{
    "bloom",
    "Threshold bright regions, blur them, and add the glow back",
    FilterTarget::Both,
    Maturity::Implemented,
    1, 2, 1.0f,
};

f32 quality_scale(const std::string& quality) {
    if (quality == "fast") return 0.5f;
    if (quality == "quality") return 1.5f;
    return 1.0f;
}

// Isolates pixels brighter than the threshold into a separate buffer, scaled so
// only the excess above the threshold contributes. Rows [row_begin, row_end).
void bright_pass(const u8* src, u32 src_base_row, u8* dst, u32 dst_base_row, u32 width,
                 u32 row_begin, u32 row_end, f32 threshold) {
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    for (u32 y = row_begin; y < row_end; ++y) {
        const u8* s = src + (static_cast<std::size_t>(y - src_base_row) * row_bytes);
        u8* d = dst + (static_cast<std::size_t>(y - dst_base_row) * row_bytes);
        for (u32 x = 0; x < width; ++x) {
            const std::size_t o = static_cast<std::size_t>(x) * 4;
            const f32 luma = luminance(s[o + 0], s[o + 1], s[o + 2]);
            if (luma > threshold) {
                d[o + 0] = s[o + 0];
                d[o + 1] = s[o + 1];
                d[o + 2] = s[o + 2];
            } else {
                d[o + 0] = 0;
                d[o + 1] = 0;
                d[o + 2] = 0;
            }
            d[o + 3] = s[o + 3];
        }
    }
}

// Separable box blur, used as the cheap approximation of a Gaussian for the
// glow, which is intentionally soft so the difference is not visible. Runs over
// output rows [row_begin, row_end); `src_base_row` is the global row index of
// src's first row and `dst_base_row` that of dst, so a band can offset into a
// buffer that starts at a different global row. The whole of src is readable,
// which is what makes the vertical pass independent between bands.
void box_blur(const u8* src, u32 src_base_row, u8* dst, u32 dst_base_row, u32 width,
              u32 height, int radius, bool horizontal, u32 row_begin, u32 row_end) {
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    for (u32 y = row_begin; y < row_end; ++y) {
        const u8* src_row = src + (static_cast<std::size_t>(y - src_base_row) * row_bytes);
        u8* dst_row = dst + (static_cast<std::size_t>(y - dst_base_row) * row_bytes);
        for (u32 x = 0; x < width; ++x) {
            f32 accum[3] = {0.0f, 0.0f, 0.0f};
            int count = 0;
            for (int k = -radius; k <= radius; ++k) {
                int sx = static_cast<int>(x);
                int sy = static_cast<int>(y);
                if (horizontal) sx = std::clamp(sx + k, 0, static_cast<int>(width) - 1);
                else sy = std::clamp(sy + k, 0, static_cast<int>(height) - 1);
                const u8* tap =
                    src + (static_cast<std::size_t>(sy - src_base_row) * row_bytes);
                const std::size_t idx = static_cast<std::size_t>(sx) * 4;
                accum[0] += tap[idx + 0];
                accum[1] += tap[idx + 1];
                accum[2] += tap[idx + 2];
                ++count;
            }
            const std::size_t out = static_cast<std::size_t>(x) * 4;
            for (int c = 0; c < 3; ++c) {
                dst_row[out + static_cast<std::size_t>(c)] =
                    clamp_u8(accum[c] / static_cast<f32>(count));
            }
            dst_row[out + 3] = src_row[out + 3];
        }
    }
}

}  // namespace

const FilterInfo& BloomFilter::info() const { return kInfo; }

Status BloomFilter::apply_rows(const ConstImage& source, Image destination,
                              const FilterParams& params, u32 row_begin,
                              u32 row_end) const {
    if (!source.valid() || !destination.valid()) {
        return Status::error(ErrorCode::InvalidArgument, "bloom: invalid image");
    }
    if (source.width != destination.width || source.height != destination.height) {
        return Status::error(ErrorCode::InvalidArgument,
                             "bloom: source and destination size mismatch");
    }
    if (row_begin >= row_end || row_end > source.height) {
        return Status::error(ErrorCode::InvalidArgument, "bloom: invalid row range");
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

    const u32 width = source.width;
    const u32 height = source.height;
    const int radius = std::max(1, static_cast<int>(radius_ * quality_scale(params.quality)));

    // The vertical blur reads `radius` rows outside the output band, so the
    // bright-pass and horizontal blur must cover that halo locally; then the
    // band is independent of every other band.
    const u32 halo_begin = row_begin > static_cast<u32>(radius) ? row_begin - radius : 0;
    const u32 halo_end = std::min(height, row_end + static_cast<u32>(radius));
    const std::size_t halo_bytes =
        static_cast<std::size_t>(halo_end - halo_begin) * width * 4;
    std::vector<u8> bright(halo_bytes, 0);
    std::vector<u8> blurred(halo_bytes, 0);

    bright_pass(source.pixels, 0, bright.data(), halo_begin, width, halo_begin, halo_end,
                threshold_);
    box_blur(bright.data(), halo_begin, blurred.data(), halo_begin, width, height, radius,
             true, halo_begin, halo_end);
    box_blur(blurred.data(), halo_begin, bright.data(), halo_begin, width, height, radius,
             false, row_begin, row_end);

    // Additive combine, weighted by intensity, over the output rows.
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    for (u32 y = row_begin; y < row_end; ++y) {
        const u8* base = source.pixels + static_cast<std::size_t>(y) * row_bytes;
        const u8* glow = bright.data() + (static_cast<std::size_t>(y - halo_begin) * row_bytes);
        u8* dst = destination.pixels + static_cast<std::size_t>(y) * row_bytes;
        for (u32 x = 0; x < width; ++x) {
            const std::size_t o = static_cast<std::size_t>(x) * 4;
            for (int c = 0; c < 3; ++c) {
                dst[o + static_cast<std::size_t>(c)] =
                    clamp_u8(static_cast<f32>(base[o + static_cast<std::size_t>(c)]) +
                             static_cast<f32>(glow[o + static_cast<std::size_t>(c)]) *
                                 params.intensity);
            }
            dst[o + 3] = base[o + 3];
        }
    }
    return Status::success();
}

Status BloomFilter::apply(const ConstImage& source, Image destination,
                          const FilterParams& params) const {
    return apply_rows(source, destination, params, 0, source.height);
}

GpuFilterPlan BloomFilter::gpu_plan(u32 width, u32 height,
                                    const FilterParams& params) const {
    const int radius = std::max(1, static_cast<int>(radius_ * quality_scale(params.quality)));
    push::BloomBright bright;
    bright.threshold = threshold_;
    push::BloomBlur blur_h;
    blur_h.radius = radius;
    blur_h.axis = 0;
    blur_h.width = static_cast<f32>(width);
    blur_h.height = static_cast<f32>(height);
    push::BloomBlur blur_v = blur_h;
    blur_v.axis = 1;
    push::BloomCombine combine;
    combine.intensity = params.intensity;

    GpuFilterPlan plan;
    plan.supported = true;
    // bright(0->2), blur_h(2->3), blur_v(3->2), combine(base 0 + glow 2 -> 1).
    plan.scratch_textures = 2;
    plan.passes.push_back(GpuPass{"bloom_bright", to_bytes(bright), {0}, 2});
    plan.passes.push_back(GpuPass{"bloom_blur_horizontal", to_bytes(blur_h), {2}, 3});
    plan.passes.push_back(GpuPass{"bloom_blur_vertical", to_bytes(blur_v), {3}, 2});
    plan.passes.push_back(GpuPass{"bloom_combine", to_bytes(combine), {0, 2}, 1});
    return plan;
}

namespace {
FilterPtr make_bloom() { return FilterPtr(new BloomFilter()); }
}  // namespace

void register_bloom_filter() {
    static bool done = false;
    if (done) return;
    done = true;
    register_filter(FilterRegistration{kInfo, &make_bloom});
}

}  // namespace lx::filters
