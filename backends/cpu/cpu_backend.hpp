// LoucuraXXXX GPU - CPU reference backend.
//
// The CPU backend is a complete, real implementation of the Backend interface:
// it allocates textures in host memory, runs registered host kernels, and
// supports upload/download. It is not a stub. It is the universal fallback that
// guarantees the pipeline produces correct output even with no GPU, and it is
// the reference against which GPU backends are validated in tests.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "backends/backend.hpp"
#include "backends/backend_registry.hpp"

namespace lx::backends {

class CpuBackend : public Backend {
public:
    explicit CpuBackend(const BackendCreateInfo& info);
    ~CpuBackend() override;

    const BackendInfo& info() const override { return info_; }
    const BackendCapabilities& capabilities() const override { return info_.capabilities; }

    Result<ResourceHandle> create_texture(const TextureDesc& desc) override;
    Status destroy_texture(ResourceHandle handle) override;
    Status upload_texture(ResourceHandle handle, const void* data, u64 bytes,
                          u64 row_pitch = 0) override;
    Result<u64> download_texture(ResourceHandle handle, void* out, u64 out_bytes,
                                 u64 row_pitch = 0) override;

    // Host memory is the texture, so mapping is exact and zero-copy.
    Result<MappedTexture> map_texture(ResourceHandle handle) override;
    Status unmap_texture(ResourceHandle handle) override;

    Status register_shader(const std::string& name,
                           const std::vector<u32>& bytecode) override;
    Status register_cpu_kernel(const std::string& name, CpuKernel kernel) override;
    Status create_pipeline(const std::string& name,
                           const std::string& shader_name) override;

    Result<std::unique_ptr<CommandEncoder>> begin_commands() override;
    Status submit(CommandEncoder* encoder) override;
    Status wait_idle() override;
    Status sync() override;

    u64 allocated_bytes() const override;

    // Direct host access to a texture's pixels. Used by the renderer and by
    // filters that run on the CPU path. Returns nullptr for an unknown handle.
    u8* texture_data(ResourceHandle handle, u32* width = nullptr, u32* height = nullptr);

    // Factory + probe used by the registry.
    static BackendProbeResult probe();
    static Result<std::unique_ptr<Backend>> create(const BackendCreateInfo& info);

private:
    struct TextureRecord {
        TextureDesc desc;
        std::vector<u8> pixels;
    };

    struct PipelineRecord {
        std::string shader_name;
    };

    class Encoder;

    BackendInfo info_;
    mutable std::mutex mutex_;
    std::unordered_map<u64, TextureRecord> textures_;
    std::unordered_map<std::string, CpuKernel> kernels_;
    std::unordered_map<std::string, PipelineRecord> pipelines_;
    std::unordered_map<std::string, std::vector<u32>> shaders_;
    u64 next_id_ = 1;
    u64 allocated_bytes_ = 0;
};

}  // namespace lx::backends
