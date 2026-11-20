// LoucuraXXXX GPU - the adapter this project can actually present to the OS.
//
// Why this file exists
// --------------------
// A GPU that shows up in the Windows Device Manager is a kernel-mode WDDM
// driver: a signed .sys plus a user-mode driver, installed with administrator
// rights. A normal process cannot create one, and shipping an unsigned one would
// be the "fake GPU identification" the project forbids. macOS is the same story
// with DriverKit entitlements.
//
// There is, however, one interface a user-space program CAN implement and have
// the operating system and applications pick up as a graphics device: a Vulkan
// ICD (Installable Client Driver). It is a shared library exporting
// vk_icdGetInstanceProcAddr, registered by placing a JSON manifest in the
// loader's search path. That is exactly how Mesa's lavapipe (a CPU Vulkan
// device) and MoltenVK appear to applications, with no kernel driver at all.
//
// So the honest meaning of "a virtual GPU that appears as a GPU" in this project
// is: a Vulkan ICD that enumerates one physical device named LoucuraXXXX GPU and
// executes work on the host CPU through this project's real CPU backend. It is a
// software Vulkan device, and it reports itself as one
// (VK_PHYSICAL_DEVICE_TYPE_CPU) rather than claiming to be discrete hardware.
//
// This header holds the parts that are pure data - naming, the manifest, the
// install locations - so they can be tested on any host, including one with no
// Vulkan at all. The ICD entry points themselves live alongside it.
#pragma once

#include <string>
#include <vector>

#include "core/status.hpp"

namespace lx::backends::icd {

// Identity of the virtual adapter. Kept in one place so the manifest, the
// enumeration and the diagnostics cannot disagree about what was installed.
struct AdapterIdentity {
    // Shown in Vulkan device listings and in vulkaninfo.
    std::string device_name = "LoucuraXXXX GPU (virtual, CPU-backed)";
    // Reverse-DNS style, unique among ICDs on the machine.
    std::string icd_id = "loucuraxxxx_gpu";
    // Manifest file name, which is what the loader actually reads.
    std::string manifest_name = "loucuraxxxx_gpu_icd.x86_64.json";
    // Library the manifest points at.
    std::string library_name = "liblxc_icd.so";
};

// The JSON manifest the Vulkan loader reads. `library_path` must be absolute:
// the loader does not resolve relative paths.
//
// `api_version` is what the ICD claims to implement. The installer writes it
// from the ICD's real capabilities rather than hard-coding it, so the claim
// cannot drift away from the code.
std::string manifest_json(const AdapterIdentity& identity,
                          const std::string& absolute_library_path,
                          const std::string& api_version);

// Directories the Vulkan loader searches for manifests, most specific first.
// A per-user location comes first so installation needs no administrator
// rights; system locations follow so callers can report what they found.
std::vector<std::string> manifest_search_paths();

// Where this process would install the manifest without privileges, e.g.
// $XDG_DATA_HOME/vulkan/icd.d on Linux or %LOCALAPPDATA%\\vulkan\\icd.d on
// Windows. Empty when the environment gives no home directory.
std::string user_manifest_directory();

// The variable the loader honours to find extra manifests. Pointing it at the
// directory containing this manifest makes the device visible to processes that
// inherit the environment, without touching any system directory.
const char* loader_override_variable();

// What an installation attempt achieved. Reported so a caller never believes a
// device is visible when it is not.
struct InstallResult {
    bool installed = false;
    std::string manifest_path;       // where the manifest was written
    std::string library_path;        // what the manifest points at
    std::vector<std::string> notes;  // why, and what still has to happen
};

// Validates that a manifest can be installed: the library must exist and the
// destination directory must be creatable. Does not modify anything.
Status check_installable(const AdapterIdentity& identity,
                         const std::string& library_path,
                         const std::string& directory);

// Writes the manifest into `directory`. Refuses rather than half-installing.
Status install_manifest(const AdapterIdentity& identity,
                        const std::string& library_path,
                        const std::string& directory,
                        InstallResult* out);

// Removes a manifest written by install_manifest. Returns success when the file
// is already gone, so uninstalling twice is not an error.
Status remove_manifest(const std::string& manifest_path);

// True when `path` looks like a manifest this project wrote, so removal cannot
// delete somebody else's ICD by mistake.
bool is_our_manifest(const std::string& path);

}  // namespace lx::backends::icd
