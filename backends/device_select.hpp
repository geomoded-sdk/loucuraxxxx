// LoucuraXXXX GPU - device preference matching, independent of any graphics API.
//
// The Vulkan backend enumerates physical devices and needs to honour
// BackendCreateInfo::preferred_device. That decision is plain string matching, so
// it lives here where it can be tested against several candidate names without a
// GPU, a driver, or a loader.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/strings.hpp"

namespace lx::backends {

// Chooses an index into `device_names`.
//
// An empty preference selects the first device (index 0). A non-empty preference
// selects the first device whose name contains it, ignoring ASCII case. When
// nothing matches, the first device is chosen and `matched` is set to false so
// the caller can say so instead of pretending the preference was honoured.
//
// Returns 0 when `device_names` is empty, in which case there is nothing to
// select and the caller must treat the enumeration as failed.
inline std::size_t select_device(const std::vector<std::string>& device_names,
                                 const std::string& preference, bool* matched = nullptr) {
    if (matched) *matched = false;
    if (device_names.empty()) return 0;
    if (preference.empty()) {
        if (matched) *matched = true;  // no preference: the default is correct
        return 0;
    }
    for (std::size_t i = 0; i < device_names.size(); ++i) {
        if (contains_ci(device_names[i], preference)) {
            if (matched) *matched = true;
            return i;
        }
    }
    return 0;  // fall back to the default device
}

}  // namespace lx::backends
