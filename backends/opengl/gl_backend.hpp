// LoucuraXXXX GPU - OpenGL 3.3 core backend.
//
// This is the fallback real GPU path: it runs on any OpenGL 3.3 core driver,
// which every GPU shipped in the last decade exposes, including on machines
// where Vulkan is absent. It is the API the project targets when Vulkan is not
// available (see the selection ladder in driver/driver.cpp).
//
// What is real here, and what is not:
//  * Real GL textures, framebuffer objects, shader compilation and drawing.
//  * Passes are full-screen fragment-shader passes into an FBO. OpenGL 3.3 core
//    has no compute shaders (those are 4.3), so the backend reports
//    supports_compute = false and does not pretend otherwise. A "dispatch" is
//    executed as a fragment pass over the target texture, which is how a 3.3
//    driver runs a per-pixel kernel.
//  * Push constants do not exist in GL 3.3; the backend emulates them with a
//    std140 uniform buffer, and the pass structs are laid out to match.
//  * The device is created without a window (a surfaceless EGL context when the
//    platform supports it), so the backend works headless and never needs a
//    display server.
//
// The context is current only on the thread that created the backend; GL is
// single-threaded by contract, so the backend serialises every call under its
// own mutex.
#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "backends/backend.hpp"
#include "backends/backend_registry.hpp"
#include "backends/opengl/gl_loader.hpp"

namespace lx::backends {

class GlBackend : public Backend {
public:
    explicit GlBackend(const BackendCreateInfo& info);
    ~GlBackend() override;

    const BackendInfo& info() const override { return info_; }
    const BackendCapabilities& capabilities() const override { return info_.capabilities; }

    Result<ResourceHandle> create_texture(const TextureDesc& desc) override;
    Status destroy_texture(ResourceHandle handle) override;
    Status upload_texture(ResourceHandle handle, const void* data, u64 bytes,
                          u64 row_pitch = 0) override;
    Result<u64> download_texture(ResourceHandle handle, void* out, u64 out_bytes,
                                 u64 row_pitch = 0) override;

    Status register_shader(const std::string& name,
                           const std::vector<u32>& bytecode) override;
    Status register_shader_source(const std::string& name,
                                  const std::string& source) override;
    Status register_cpu_kernel(const std::string& name, CpuKernel kernel) override;
    Status create_pipeline(const std::string& name,
                           const std::string& shader_name) override;

    const char* builtin_shader_source(const std::string& shader_name) const override {
        return builtin_fragment_source(shader_name);
    }

    Result<std::unique_ptr<CommandEncoder>> begin_commands() override;
    Status submit(CommandEncoder* encoder) override;
    Status wait_idle() override;
    Status sync() override;

    u64 allocated_bytes() const override;

    // Factory + probe used by the registry.
    static BackendProbeResult probe();
    static Result<std::unique_ptr<Backend>> create(const BackendCreateInfo& info);

    // The GLSL 3.30 fragment source built into the backend for `shader_name`, or
    // empty when the backend has no built-in for it. Exposed so a caller can see
    // exactly what the GPU path compiles, and so tests can register it.
    static const char* builtin_fragment_source(const std::string& shader_name);

private:
    struct TextureRecord {
        TextureDesc desc;
        gl::GLuint texture = 0;
        gl::GLuint fbo = 0;  // colour attachment target for a render pass
        bool is_target = false;
        // True once glTexImage2D has sized the storage. Re-uploading with
        // TexImage2D every frame reallocates the whole image; TexSubImage2D
        // reuses it, which is the difference between a per-frame realloc and a
        // straight memcpy into driver-owned storage.
        bool allocated = false;
    };

    struct PipelineRecord {
        std::string shader_name;
        gl::GLuint program = 0;
    };

    class Encoder;

    Status compile_pipeline(PipelineRecord& pipeline, const std::string& source);
    Status ensure_vao();
    TextureRecord* find_texture(ResourceHandle handle);

    // Points the shader's input samplers at the texture units the encoder bound.
    // u_image -> unit 0, u_glow -> unit 1. Locations are cached per program.
    void bind_input_samplers(gl::GLuint program, u32 input_count);

    BackendInfo info_;
    gl::GlApi gl_{};
    gl::EglApi egl_{};
    gl::EGLDisplay display_ = nullptr;
    gl::EGLContext context_ = nullptr;
    gl::GLuint vao_ = 0;
    gl::GLuint ubo_ = 0;

    mutable std::mutex mutex_;
    std::unordered_map<u64, TextureRecord> textures_;
    std::unordered_map<std::string, PipelineRecord> pipelines_;
    std::unordered_map<std::string, std::string> shader_sources_;
    std::unordered_map<std::string, CpuKernel> kernels_;
    // Cached sampler uniform locations, keyed by program. Keeps the per-pass
    // cost to a lookup rather than a glGetUniformLocation round trip.
    std::unordered_map<gl::GLuint, gl::GLint> image_sampler_locations_;
    std::unordered_map<gl::GLuint, gl::GLint> glow_sampler_locations_;
    u64 next_id_ = 1;
    u64 allocated_bytes_ = 0;
};

}  // namespace lx::backends
