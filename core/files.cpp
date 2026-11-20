#include "core/files.hpp"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace lx {

Result<std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Status::error(ErrorCode::IoError, "cannot open for reading: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Status write_file(const std::string& path, const std::string& data) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) {
        LX_TRY(make_directories(path.substr(0, slash)));
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return Status::error(ErrorCode::IoError, "cannot open for writing: " + path);
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) {
        return Status::error(ErrorCode::IoError, "write failed: " + path);
    }
    return Status::success();
}

bool file_exists(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return static_cast<bool>(in);
}

Status make_directories(const std::string& path) {
    if (path.empty()) return Status::success();
    // Create every missing ancestor, not just the leaf: config paths like
    // "<home>/Library/Application Support/LoucuraXXXX-GPU" have intermediate
    // directories that may not exist yet, and a single mkdir would fail on the
    // first missing level.
    std::string current;
    current.reserve(path.size());
    std::size_t start = 0;
    // Preserve a drive prefix ("C:\") or a leading root so we never try to
    // create "C:" or "/" itself.
    if (path.size() >= 2 && path[1] == ':') {
        current = path.substr(0, 2);
        start = 2;
    } else if (path[0] == '/' || path[0] == '\\') {
        current = path.substr(0, 1);
        start = 1;
    }
    for (std::size_t i = start; i <= path.size(); ++i) {
        const bool at_end = i == path.size();
        const bool is_sep = !at_end && (path[i] == '/' || path[i] == '\\');
        if (!at_end && !is_sep) {
            current.push_back(path[i]);
            continue;
        }
        if (!current.empty() && current.back() != '/' && current.back() != '\\') {
            // EEXIST is success here: an ancestor may already exist, or a
            // previous component may have created it. Anything else is a real
            // failure and must be reported rather than silently ignored.
#if defined(_WIN32)
            if (_mkdir(current.c_str()) != 0 && errno != EEXIST) {
                return Status::error(ErrorCode::IoError, "mkdir failed: " + current);
            }
#else
            if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                return Status::error(ErrorCode::IoError, "mkdir failed: " + current);
            }
#endif
        }
        if (!at_end) current.push_back('/');
    }
    return Status::success();
}

Result<std::string> config_directory() {
    const char* base = nullptr;
#if defined(_WIN32)
    base = std::getenv("APPDATA");
    std::string dir = base ? std::string(base) + "\\LoucuraXXXX-GPU"
                           : std::string(".\\LoucuraXXXX-GPU");
#elif defined(__APPLE__)
    base = std::getenv("HOME");
    std::string dir = base ? std::string(base) + "/Library/Application Support/LoucuraXXXX-GPU"
                           : std::string("./LoucuraXXXX-GPU");
#else
    base = std::getenv("XDG_CONFIG_HOME");
    std::string dir;
    if (base && *base) {
        dir = std::string(base) + "/loucuraxxxx-gpu";
    } else {
        const char* home = std::getenv("HOME");
        dir = home ? std::string(home) + "/.config/loucuraxxxx-gpu"
                   : std::string("./loucuraxxxx-gpu");
    }
#endif
    LX_TRY(make_directories(dir));
    return dir;
}

}  // namespace lx
