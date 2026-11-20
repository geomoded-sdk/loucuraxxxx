// LoucuraXXXX GPU - push-constant layouts shared by the GPU filter passes.
//
// Each struct here is the exact byte layout a filter's GPU pass sends to the
// backend. The layouts are chosen so one block of bytes is valid for both the
// OpenGL std140 uniform block and the Direct3D 11 constant buffer, because the
// two backends declare the same field order. They must stay in sync with the
// shaders in backends/opengl/gl_backend.cpp and backends/d3d11/d3d11_backend.cpp.
//
// GpuPass/GpuFilterPlan themselves live in filters/filter.hpp so IFilter can
// reference them without a cycle.
#pragma once

#include <vector>

#include "core/types.hpp"
#include "filters/filter.hpp"

namespace lx::filters {
namespace push {

// tone: exposure in stops, contrast, saturation, intensity.
struct Tone {
    f32 exposure_stops = 0.0f;
    f32 contrast = 0.0f;
    f32 saturation = 1.0f;
    f32 intensity = 1.0f;
};
static_assert(sizeof(Tone) == 16, "tone push block must be 16 bytes");

// vignette: inner/outer radius, intensity, then the image size in pixels.
struct Vignette {
    f32 inner = 0.45f;
    f32 outer = 1.0f;
    f32 intensity = 1.0f;
    f32 pad0 = 0.0f;
    f32 width = 0.0f;
    f32 height = 0.0f;
    f32 pad1 = 0.0f;
    f32 pad2 = 0.0f;
};
static_assert(sizeof(Vignette) == 32, "vignette push block must be 32 bytes");

// sharpen: unsharp-mask strength and the image size.
struct Sharpen {
    f32 strength = 0.0f;
    f32 pad0 = 0.0f;
    f32 width = 0.0f;
    f32 height = 0.0f;
    f32 pad1 = 0.0f;
    f32 pad2 = 0.0f;
};
static_assert(sizeof(Sharpen) == 24, "sharpen push block must be 24 bytes");

// blur: separable Gaussian, one axis per pass.
struct Blur {
    f32 sigma = 2.0f;
    f32 intensity = 1.0f;
    f32 width = 0.0f;
    f32 height = 0.0f;
    i32 axis = 0;    // 0 = horizontal, 1 = vertical
    i32 radius = 0;  // kernel radius in pixels
    i32 pad0 = 0;
    i32 pad1 = 0;
};
static_assert(sizeof(Blur) == 32, "blur push block must be 32 bytes");

// bloom bright pass: luminance threshold.
struct BloomBright {
    f32 threshold = 200.0f;
    f32 pad0 = 0.0f;
    f32 pad1 = 0.0f;
    f32 pad2 = 0.0f;
};
static_assert(sizeof(BloomBright) == 16, "bloom bright push block must be 16 bytes");

// bloom box blur: radius, axis, image size.
struct BloomBlur {
    i32 radius = 4;
    i32 axis = 0;
    f32 width = 0.0f;
    f32 height = 0.0f;
};
static_assert(sizeof(BloomBlur) == 16, "bloom blur push block must be 16 bytes");

// bloom combine: additive glow weight.
struct BloomCombine {
    f32 intensity = 1.0f;
    f32 pad0 = 0.0f;
    f32 pad1 = 0.0f;
    f32 pad2 = 0.0f;
};
static_assert(sizeof(BloomCombine) == 16, "bloom combine push block must be 16 bytes");

}  // namespace push

// Serialises a push-constant struct to the raw byte block a backend expects.
template <typename T>
std::vector<u8> to_bytes(const T& value) {
    const auto* raw = reinterpret_cast<const u8*>(&value);
    return std::vector<u8>(raw, raw + sizeof(T));
}

}  // namespace lx::filters
