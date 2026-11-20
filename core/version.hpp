// LoucuraXXXX GPU - version information.
#pragma once

#include <cstdint>
#include <string>

#include "core/types.hpp"

namespace lx {

inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 2;
inline constexpr const char* kVersionStage = "INDEV";
inline constexpr const char* kProductName = "LoucuraXXXX GPU";

// The build as a whole is development-stage: the core pipeline works end to
// end on the CPU backend, while GPU backends and OS integrations are partial.
inline constexpr Maturity kMaturity = Maturity::Experimental;

// Compact numeric form, useful for feature gates and serialized configs.
inline constexpr std::uint32_t kVersionNumber =
    static_cast<std::uint32_t>(kVersionMajor * 10000 + kVersionMinor * 100 + kVersionPatch);

inline std::string version_string() {
    return std::string(kProductName) + " " + std::to_string(kVersionMajor) + "." +
           std::to_string(kVersionMinor) + "." + std::to_string(kVersionPatch) + " " + kVersionStage;
}

}  // namespace lx
