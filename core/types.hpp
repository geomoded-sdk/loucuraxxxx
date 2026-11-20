// LoucuraXXXX GPU - shared value types used across modules.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lx {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

inline constexpr u64 kKiB = 1024ull;
inline constexpr u64 kMiB = 1024ull * 1024ull;
inline constexpr u64 kGiB = 1024ull * 1024ull * 1024ull;

struct Version {
    u32 major = 0;
    u32 minor = 0;
    u32 patch = 0;
    std::string to_string() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." +
               std::to_string(patch);
    }
};

// Maturity classification. Every public capability is tagged with one of these
// so users can distinguish what is real from what is planned.
enum class Maturity {
    Implemented,  // Built, tested, exercised in CI.
    Experimental, // Built and functional, but unstable or limited coverage.
    Planned,      // Documented intent, no working code yet.
};

inline const char* to_string(Maturity m) {
    switch (m) {
        case Maturity::Implemented: return "implemented";
        case Maturity::Experimental: return "experimental";
        case Maturity::Planned: return "planned";
    }
    return "unknown";
}

// A 2D extent in pixels.
struct Extent2D {
    u32 width = 0;
    u32 height = 0;
    u64 pixel_count() const { return static_cast<u64>(width) * height; }
};

// A 32-bit RGBA color, 8 bits per channel, byte order R,G,B,A.
struct Color32 {
    u8 r = 0, g = 0, b = 0, a = 255;
};

}  // namespace lx
