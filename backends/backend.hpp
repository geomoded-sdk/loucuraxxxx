// LoucuraXXXX GPU - graphics backend abstraction.
//
// A backend is a concrete way to execute graphics work: a real API (Vulkan,
// Direct3D 11, OpenGL, Metal) or the CPU reference path. The renderer and the
// manager talk only to this interface, which keeps the hybrid logic independent
// of any one API and makes the CPU path a first-class peer rather than a
// special case.
//
// The interface is deliberately explicit about what is real. A backend that
// cannot do something must report that via capabilities() or return
// NotSupported; it must never silently no-op.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace lx::backends {

enum class BackendKind { Cpu, Vulkan, Direct3D11, OpenGL, Metal };

inline const char* to_string(BackendKind kind) {
    switch (kind) {
        case BackendKind::Cpu: return "cpu";
        case BackendKind::Vulkan: return "vulkan";
        case BackendKind::Direct3D11: return "d3d11";
        case BackendKind::OpenGL: return "opengl";
        case BackendKind::Metal: return "metal";
    }
    return "unknown";
}

// Texture formats the pipeline understands. Kept small and explicit.
enum class PixelFormat { RGBA8Unorm, RGBA8Srgb, R32Float };

inline u32 bytes_per_pixel(PixelFormat format) {
    switch (format) {
        case PixelFormat::RGBA8Unorm:
        case PixelFormat::RGBA8Srgb: return 4;
        case PixelFormat::R32Float: return 4;
    }
    return 4;
}

// Byte size of a tightly packed texture. Computed in 64-bit so large frames do
// not wrap: width*height*bpp overflows u32 well before the dimensions the
// capabilities advertise.
inline u64 texture_byte_size(u32 width, u32 height, PixelFormat format) {
    return static_cast<u64>(width) * height * bytes_per_pixel(format);
}

struct TextureDesc {
    u32 width = 0;
    u32 height = 0;
    PixelFormat format = PixelFormat::RGBA8Unorm;
    bool sampled = true;    // read by a shader
    bool storage = true;    // written by a shader
    bool render_target = false;
    std::string debug_name;
};

// What a backend can actually do. Callers check these before relying on a path.
struct BackendCapabilities {
    bool supports_compute = false;
    bool supports_storage_textures = false;
    bool supports_float_textures = false;
    bool supports_parallel_dispatch = false;
    bool supports_timestamps = false;
    bool is_hardware = false;  // false for the CPU and software rasterizers
    u32 max_texture_dimension = 0;
    u32 max_workgroup_size = 0;
    u64 max_memory_bytes = 0;

    // The device shares the CPU's physical memory (Apple silicon, most
    // integrated GPUs). When true there is nothing to copy between host and
    // device, so the cost model charges no transfer and the renderer can map a
    // texture instead of uploading into it.
    bool unified_memory = false;

    // True when map_texture() is supported, i.e. the backend can hand out a
    // host pointer to a texture's storage. Always true for a unified backend
    // with host-visible storage; false for a discrete GPU whose textures live
    // only in device memory.
    bool supports_host_mapping = false;
};

// A host-visible view of a texture's storage. The pointer is valid only until
// unmap_texture() is called. `row_pitch` is the byte stride between rows and
// may exceed width*bytes_per_pixel when the backend pads rows.
struct MappedTexture {
    void* data = nullptr;
    u32 width = 0;
    u32 height = 0;
    u64 row_pitch = 0;
    bool valid() const { return data != nullptr; }
};

struct BackendInfo {
    BackendKind kind = BackendKind::Cpu;
    std::string device_name;
    std::string driver_name;
    std::string api_version;
    BackendCapabilities capabilities;
    Maturity maturity = Maturity::Implemented;
};

// Heuristic used by the API backends to tell a real GPU from a software
// rasterizer that merely implements the same API (Mesa lavapipe/llvmpipe/
// softpipe, Windows WARP, SwiftShader). The names are the ones those
// implementations actually report; a driver that reports none of them is taken
// at its word.
//
// This exists because "the Vulkan API is present" and "this machine has a GPU
// that can run Vulkan" are different facts. A host without Vulkan drivers can
// still expose a software ICD, and reporting that as hardware acceleration would
// mislead every placement decision downstream.
inline bool looks_like_software_rasterizer(const std::string& device_or_driver_name) {
    std::string lower;
    lower.reserve(device_or_driver_name.size());
    for (char c : device_or_driver_name) {
        lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    for (const char* needle : {"llvmpipe", "softpipe", "swrast", "lavapipe", "swiftshader",
                               "warp", "software rasterizer", "microsoft basic render"}) {
        if (lower.find(needle) != std::string::npos) return true;
    }
    return false;
}

// True when the process runs under Wine. Wine synthesises a DXGI/Vulkan adapter
// from a built-in card database, so the adapter name and even the PCI vendor id
// describe a card that is not present while the calls execute on the host's
// software rasterizer. There is no way to tell from the adapter description
// alone, so the environment is the signal.
inline bool running_under_wine() {
#if defined(_WIN32)
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll == nullptr) return false;
    return GetProcAddress(ntdll, "wine_get_version") != nullptr;
#else
    const char* wine = std::getenv("WINEPREFIX");
    const char* wine_loader = std::getenv("WINELOADER");
    return (wine != nullptr && *wine != '\0') || (wine_loader != nullptr && *wine_loader != '\0');
#endif
}

// An opaque handle to a GPU/CPU resource. Implementations cast this to their
// own type. Kept as a small value type so it can live in vectors.
struct ResourceHandle {
    u64 id = 0;
    bool valid() const { return id != 0; }
    static constexpr u64 kInvalid = 0;
};

// A single compute/render pass description handed to the backend.
struct DispatchDesc {
    u32 groups_x = 1;
    u32 groups_y = 1;
    u32 groups_z = 1;
    std::string kernel_name;  // must match a compiled shader entry point
};

// A host-side kernel for the CPU backend: takes the push-constant block and
// texture bindings and performs the work directly.
class CpuKernel {
public:
    using Fn = Status (*)(const void* push_constants, u32 push_size,
                          const std::vector<ResourceHandle>& textures, void* user_data);
    CpuKernel() = default;
    CpuKernel(Fn fn, void* user_data = nullptr) : fn_(fn), user_data_(user_data) {}
    bool valid() const { return fn_ != nullptr; }
    Status invoke(const void* push_constants, u32 push_size,
                  const std::vector<ResourceHandle>& textures) const {
        if (!fn_) return Status::error(ErrorCode::InvalidArgument, "null CPU kernel");
        return fn_(push_constants, push_size, textures, user_data_);
    }

private:
    Fn fn_ = nullptr;
    void* user_data_ = nullptr;
};

// A recorded sequence of work, submitted as a unit. The CPU backend executes it
// eagerly; a GPU backend records commands and submits them in one batch.
class CommandEncoder {
public:
    virtual ~CommandEncoder() = default;
    virtual Status bind_pipeline(const std::string& pipeline_name) = 0;
    virtual Status bind_texture(u32 slot, ResourceHandle texture) = 0;
    virtual Status bind_storage_texture(u32 slot, ResourceHandle texture) = 0;
    virtual Status set_push_constants(const void* data, u32 bytes) = 0;
    virtual Status dispatch(const DispatchDesc& desc) = 0;
};

// A backend instance. Created via create_backend, one per logical device.
class Backend {
public:
    virtual ~Backend() = default;

    virtual const BackendInfo& info() const = 0;
    virtual const BackendCapabilities& capabilities() const = 0;

    // --- Resources ---
    virtual Result<ResourceHandle> create_texture(const TextureDesc& desc) = 0;
    virtual Status destroy_texture(ResourceHandle handle) = 0;

    // Uploads host pixels into a texture. `row_pitch` is bytes per row of the
    // source; 0 means tightly packed.
    virtual Status upload_texture(ResourceHandle handle, const void* data, u64 bytes,
                                  u64 row_pitch = 0) = 0;

    // Downloads a texture back to host memory. Returns the number of bytes
    // written; the buffer must be at least width*height*bytes_per_pixel.
    virtual Result<u64> download_texture(ResourceHandle handle, void* out, u64 out_bytes,
                                         u64 row_pitch = 0) = 0;

    // Maps a texture's storage for direct host access. On a unified-memory
    // backend this is the zero-copy path: the returned pointer *is* the
    // texture, so writing through it is the upload. Backends without host
    // mapping return NotSupported, and callers must fall back to
    // upload_texture/download_texture.
    virtual Result<MappedTexture> map_texture(ResourceHandle handle) {
        (void)handle;
        return Status::error(ErrorCode::NotSupported,
                             "backend does not support host mapping");
    }
    virtual Status unmap_texture(ResourceHandle handle) {
        (void)handle;
        return Status::error(ErrorCode::NotSupported,
                             "backend does not support host mapping");
    }

    // --- Shaders / pipelines ---
    // Registers SPIR-V (Vulkan) or equivalent bytecode under a name. The CPU
    // backend registers a host function instead and ignores `bytecode`.
    virtual Status register_shader(const std::string& name, const std::vector<u32>& bytecode) = 0;

    // Registers a shader from source text in the backend's native language:
    // Metal Shading Language for Metal, GLSL for a GL backend. Backends that
    // only accept pre-compiled bytecode (the CPU reference backend, a future
    // SPIR-V-only Vulkan backend) return NotSupported, so the caller can decide
    // whether to pre-compile instead.
    virtual Status register_shader_source(const std::string& name, const std::string& source) {
        (void)name;
        (void)source;
        return Status::error(ErrorCode::NotSupported,
                             "backend does not accept shader source text");
    }

    virtual Status register_cpu_kernel(const std::string& name, class CpuKernel kernel) = 0;
    virtual Status create_pipeline(const std::string& name, const std::string& shader_name) = 0;

    // Returns the backend's built-in shader source for a filter pass, in the
    // backend's own language (GLSL, HLSL, ...), or nullptr when the backend has
    // no built-in for that name. The renderer uses this to run a filter chain on
    // the GPU without the caller having to know which shading language the
    // backend speaks. A backend that runs filters some other way (the CPU
    // reference backend) returns nullptr.
    virtual const char* builtin_shader_source(const std::string& shader_name) const {
        (void)shader_name;
        return nullptr;
    }

    // --- Execution ---
    virtual Result<std::unique_ptr<CommandEncoder>> begin_commands() = 0;
    virtual Status submit(CommandEncoder* encoder) = 0;
    virtual Status wait_idle() = 0;

    // Blocks until prior work on `handle` is visible to the host.
    virtual Status sync() = 0;

    // --- Diagnostics ---
    virtual u64 allocated_bytes() const = 0;
};

// Describes how a backend should be created.
struct BackendCreateInfo {
    BackendKind preferred = BackendKind::Cpu;
    bool allow_validation = false;
    std::string preferred_device;  // empty => default
    u64 memory_budget_bytes = 0;   // 0 => unbounded
    bool headless = true;          // no window/swapchain required
};

// Creates a backend of the requested kind via the registry. GPU backends return
// BackendUnavailable when the API or a suitable device is missing, which lets
// the caller fall back deliberately rather than crash.
Result<std::unique_ptr<Backend>> create_backend(const BackendCreateInfo& info);

// Lists the backend kinds compiled into this build (registered at load time).
std::vector<BackendKind> available_backend_kinds();

}  // namespace lx::backends
