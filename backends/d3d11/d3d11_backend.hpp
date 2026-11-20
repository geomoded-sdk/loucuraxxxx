// LoucuraXXXX GPU - Direct3D 11 backend (Windows).
//
// This is the Windows real-GPU path the project prefers when Vulkan is absent.
// D3D11 is available on every Windows 7+ machine with a WDDM driver, which is
// exactly the population the target machine belongs to.
//
// What is real here:
//  * A real ID3D11Device/ID3D11DeviceContext (hardware, falling back to WARP).
//  * Real ID3D11Texture2D resources with shader-resource and render-target
//    views, and a staging texture for upload/download.
//  * Real HLSL compiled at run time with D3DCompile.
//  * A pass is a full-screen triangle drawn with a pixel shader, which is the
//    D3D11 equivalent of the OpenGL path. D3D11 has compute shaders (CS 5.0),
//    but the built-in passes are image filters expressed as pixel shaders, so
//    the backend reports supports_compute = false and does not overstate.
//  * Push constants are emulated with a constant buffer bound to b0.
//
// The device is created without a swapchain, so it works headless.
#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "backends/backend.hpp"
#include "backends/backend_registry.hpp"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11Buffer;
struct ID3D11RasterizerState;
struct ID3D11Query;

namespace lx::backends {

class D3D11Backend : public Backend {
public:
    explicit D3D11Backend(const BackendCreateInfo& info);
    ~D3D11Backend() override;

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
        return builtin_pixel_shader_source(shader_name);
    }

    Result<std::unique_ptr<CommandEncoder>> begin_commands() override;
    Status submit(CommandEncoder* encoder) override;
    Status wait_idle() override;
    Status sync() override;

    u64 allocated_bytes() const override;

    static BackendProbeResult probe();
    static Result<std::unique_ptr<Backend>> create(const BackendCreateInfo& info);

    // The HLSL pixel-shader source built into the backend for `shader_name`, or
    // nullptr when there is no built-in. Exposed so a caller can inspect exactly
    // what the GPU path compiles.
    static const char* builtin_pixel_shader_source(const std::string& shader_name);

private:
    struct TextureRecord {
        TextureDesc desc;
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
    };

    struct PipelineRecord {
        std::string shader_name;
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader* ps = nullptr;
    };

    class Encoder;

    Status compile_pipeline(PipelineRecord& pipeline, const std::string& source);
    TextureRecord* find_texture(ResourceHandle handle);

    BackendInfo info_;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Buffer* constant_buffer_ = nullptr;
    ID3D11RasterizerState* rasterizer_state_ = nullptr;
    // Signalled after all queued work completes; see wait_idle(). Created on
    // first use because a query is only valid on the device that made it.
    ID3D11Query* idle_event_ = nullptr;

    mutable std::mutex mutex_;
    std::unordered_map<u64, TextureRecord> textures_;
    std::unordered_map<std::string, PipelineRecord> pipelines_;
    std::unordered_map<std::string, std::string> shader_sources_;
    std::unordered_map<std::string, CpuKernel> kernels_;
    u64 next_id_ = 1;
    u64 allocated_bytes_ = 0;
};

}  // namespace lx::backends
