// LoucuraXXXX GPU - simple RGBA image used by the filter pipeline.
//
// Filters operate on a non-owning view of 8-bit RGBA pixels. This keeps the
// filter math independent of where the pixels live: host memory for the CPU
// path, or a staging buffer mapped back from a GPU texture.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "core/status.hpp"
#include "core/types.hpp"

namespace lx::filters {

// A mutable, non-owning view over a tightly packed RGBA8 image.
struct Image {
    u8* pixels = nullptr;
    u32 width = 0;
    u32 height = 0;

    bool valid() const { return pixels != nullptr && width > 0 && height > 0; }
    u64 byte_size() const {
        return static_cast<u64>(width) * height * 4ull;
    }
    std::size_t pixel_count() const {
        return static_cast<std::size_t>(width) * height;
    }

    u8* row(u32 y) {
        return pixels + static_cast<std::size_t>(y) * width * 4;
    }
    const u8* row(u32 y) const {
        return pixels + static_cast<std::size_t>(y) * width * 4;
    }
};

// A read-only view, used for the source of a filter that writes elsewhere.
struct ConstImage {
    const u8* pixels = nullptr;
    u32 width = 0;
    u32 height = 0;

    bool valid() const { return pixels != nullptr && width > 0 && height > 0; }
    const u8* row(u32 y) const {
        return pixels + static_cast<std::size_t>(y) * width * 4;
    }
    Image mutable_view() const {
        return Image{const_cast<u8*>(pixels), width, height};
    }
};

inline u8 clamp_u8(int value) {
    return static_cast<u8>(std::clamp(value, 0, 255));
}

inline u8 clamp_u8(float value) {
    return static_cast<u8>(std::clamp(value, 0.0f, 255.0f));
}

inline float luminance(u8 r, u8 g, u8 b) {
    // Rec. 709 luma coefficients.
    return 0.2126f * static_cast<float>(r) + 0.7152f * static_cast<float>(g) +
           0.0722f * static_cast<float>(b);
}

}  // namespace lx::filters
