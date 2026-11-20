#include "backends/vulkan/icd/icd_manifest.hpp"

#include <cstdlib>

#include "core/files.hpp"
#include "core/json.hpp"
#include "core/log.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace lx::backends::icd {

namespace {

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

// Absolute paths are required by the loader. A relative library path would be
// silently ignored, so refuse it here where the error can be explained.
bool is_absolute(const std::string& path) {
#if defined(_WIN32)
    return path.size() > 2 && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
#else
    return !path.empty() && path[0] == '/';
#endif
}

std::string join_path(const std::string& directory, const std::string& name) {
#if defined(_WIN32)
    const char separator = '\\';
#else
    const char separator = '/';
#endif
    if (directory.empty()) return name;
    if (directory.back() == separator) return directory + name;
    return directory + separator + name;
}

}  // namespace

std::string manifest_json(const AdapterIdentity& identity,
                          const std::string& absolute_library_path,
                          const std::string& api_version) {
    // The loader reads exactly these keys. "library_arch" keeps a 32-bit loader
    // from trying to open a 64-bit ICD and failing confusingly.
    Json root = Json::object();
    root["file_format_version"] = Json("1.0.0");
    Json icd = Json::object();
    icd["library_path"] = Json(absolute_library_path);
#if defined(_WIN32)
    icd["library_arch"] = Json("64");
#endif
    icd["api_version"] = Json(api_version);
    // The loader ignores unknown keys, so this marker is free. Without it the
    // file is indistinguishable from any other ICD's manifest and removal has no
    // safe way to know it wrote the file itself.
    icd["loucuraxxxx_gpu"] = Json(identity.icd_id);
    root["ICD"] = icd;
    return root.dump();
}

std::string user_manifest_directory() {
#if defined(_WIN32)
    const std::string local = env_or_empty("LOCALAPPDATA");
    if (local.empty()) return {};
    return join_path(join_path(local, "vulkan"), "icd.d");
#else
    const std::string xdg = env_or_empty("XDG_DATA_HOME");
    if (!xdg.empty()) return join_path(join_path(xdg, "vulkan"), "icd.d");
    const std::string home = env_or_empty("HOME");
    if (home.empty()) return {};
    return join_path(join_path(join_path(home, ".local"), "share"), "vulkan/icd.d");
#endif
}

std::vector<std::string> manifest_search_paths() {
    std::vector<std::string> paths;
    // Most specific first: the per-user location is what this project installs
    // into, and it needs no privileges.
    const std::string user = user_manifest_directory();
    if (!user.empty()) paths.push_back(user);
#if defined(_WIN32)
    const std::string program_data = env_or_empty("PROGRAMDATA");
    if (!program_data.empty()) {
        paths.push_back(join_path(join_path(program_data, "vulkan"), "icd.d"));
    }
#else
    paths.emplace_back("/usr/local/share/vulkan/icd.d");
    paths.emplace_back("/usr/share/vulkan/icd.d");
    paths.emplace_back("/etc/vulkan/icd.d");
#endif
    return paths;
}

const char* loader_override_variable() {
    // The loader's own variable. It takes a colon-separated (semicolon on
    // Windows) list of manifest files, not directories, on current loaders; the
    // caller is told which files to list.
    return "VK_ICD_FILENAMES";
}

Status check_installable(const AdapterIdentity& identity,
                         const std::string& library_path,
                         const std::string& directory) {
    if (identity.manifest_name.empty()) {
        return Status::error(ErrorCode::InvalidArgument, "manifest name is empty");
    }
    if (!is_absolute(library_path)) {
        return Status::error(
            ErrorCode::InvalidArgument,
            "library path must be absolute; the Vulkan loader ignores relative paths: " +
                library_path);
    }
    if (!file_exists(library_path)) {
        return Status::error(ErrorCode::IoError,
                             "ICD library not found: " + library_path);
    }
    if (directory.empty()) {
        return Status::error(ErrorCode::InvalidArgument,
                             "no install directory; set HOME or XDG_DATA_HOME");
    }
    // Creating the directory is part of "can we install here", so a read-only
    // home is reported now rather than as a failed write later.
    Status created = make_directories(directory);
    if (!created.ok()) {
        return Status::error(created.code(),
                             "cannot create manifest directory '" + directory +
                                 "': " + created.message());
    }
    return Status::success();
}

Status install_manifest(const AdapterIdentity& identity,
                        const std::string& library_path,
                        const std::string& directory,
                        InstallResult* out) {
    InstallResult result;
    result.library_path = library_path;
    result.manifest_path = join_path(directory, identity.manifest_name);

    Status ready = check_installable(identity, library_path, directory);
    if (!ready.ok()) {
        result.notes.push_back(ready.message());
        if (out) *out = result;
        return ready;
    }

    const std::string manifest =
        manifest_json(identity, library_path, "1.2.0");
    Status written = write_file(result.manifest_path, manifest);
    if (!written.ok()) {
        result.notes.push_back("cannot write manifest: " + written.message());
        if (out) *out = result;
        return written;
    }

    result.installed = true;
    result.notes.emplace_back("manifest written to " + result.manifest_path);
    // Saying how the device becomes visible matters: writing the file is not the
    // same as every application seeing it, and the difference is the user's to
    // resolve.
    result.notes.emplace_back(
        std::string("the loader finds it via ") + loader_override_variable() + "=" +
        result.manifest_path +
        ", or automatically when the directory is in its default search path");
    result.notes.emplace_back(
        "this is a software Vulkan device executed on the host CPU; it does not "
        "add hardware and will not beat the physical GPU on GPU-bound work");
    LX_INFO << "installed Vulkan ICD manifest at " << result.manifest_path;
    if (out) *out = result;
    return Status::success();
}

bool is_our_manifest(const std::string& path) {
    // Only files carrying this project's ICD id are treated as ours, so a
    // mistyped path cannot remove another vendor's ICD.
    auto contents = read_file(path);
    if (!contents.ok()) return false;
    // Parsed rather than substring-matched, so a file that merely mentions the
    // name in a comment or path is not treated as ours.
    auto parsed = Json::parse(contents.value());
    if (!parsed.ok()) return false;
    const Json& root = parsed.value();
    if (!root.is_object() || !root.has("ICD")) return false;
    const Json& icd = root.at("ICD");
    return icd.is_object() && icd.has("loucuraxxxx_gpu");
}

Status remove_manifest(const std::string& manifest_path) {
    if (manifest_path.empty()) {
        return Status::error(ErrorCode::InvalidArgument, "manifest path is empty");
    }
    if (!file_exists(manifest_path)) return Status::success();
    if (!is_our_manifest(manifest_path)) {
        return Status::error(ErrorCode::InvalidArgument,
                             "refusing to remove a manifest this project did not write: " +
                                 manifest_path);
    }
#if defined(_WIN32)
    if (::DeleteFileA(manifest_path.c_str()) == 0) {
        return Status::error(ErrorCode::IoError,
                             "cannot remove manifest: " + manifest_path);
    }
#else
    if (::unlink(manifest_path.c_str()) != 0) {
        return Status::error(ErrorCode::IoError,
                             "cannot remove manifest: " + manifest_path);
    }
#endif
    LX_INFO << "removed Vulkan ICD manifest " << manifest_path;
    return Status::success();
}

}  // namespace lx::backends::icd
