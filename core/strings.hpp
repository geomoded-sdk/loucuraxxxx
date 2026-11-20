// LoucuraXXXX GPU - small, dependency-free string helpers.
#pragma once

#include <string>
#include <string_view>

namespace lx {

// Lowercases ASCII letters only. Device names and user preferences are ASCII in
// practice, and staying out of locale-aware conversion keeps the result stable
// across platforms (a Turkish locale would otherwise map 'I' oddly).
inline std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// True when `needle` occurs in `haystack`, ignoring ASCII case. An empty needle
// matches everything, which callers use to mean "no preference".
inline bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    return ascii_lower(haystack).find(ascii_lower(needle)) != std::string::npos;
}

}  // namespace lx
