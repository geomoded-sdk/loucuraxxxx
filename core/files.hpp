// LoucuraXXXX GPU - small filesystem helpers.
#pragma once

#include <string>

#include "core/status.hpp"

namespace lx {

// Reads an entire file into a string. Returns IoError if the file is missing.
Result<std::string> read_file(const std::string& path);

// Writes data to a file, creating parent directories when needed.
Status write_file(const std::string& path, const std::string& data);

// Returns true if the path exists and is a regular file.
bool file_exists(const std::string& path);

// Creates directories recursively. Returns Ok if they already exist.
Status make_directories(const std::string& path);

// Returns a per-user configuration directory, creating it if necessary.
// Linux: $XDG_CONFIG_HOME or ~/.config ; Windows: %APPDATA% ; macOS: ~/Library.
Result<std::string> config_directory();

}  // namespace lx
