// LoucuraXXXX GPU - Metal backend (macOS / Apple silicon).
//
// This is the real graphics backend for macOS. It talks to the Apple GPU
// through Metal, the only supported low-level graphics API on modern macOS.
// There is no kernel extension and no privileged component: Metal is a
// user-space framework, which is exactly what the platform permits.
//
// Memory model: every texture is backed by a MTLBuffer with
// MTLStorageModeShared. On Apple silicon that storage is the same physical
// memory the CPU uses, so a texture can be handed to the host with no copy -
// this is the unified-memory path the rest of the layer is built to exploit.
// map_texture() therefore returns the buffer's own contents pointer.
//
// Status: EXPERIMENTAL. The code is written against the documented Metal API
// but has not been exercised on a macOS host in this repository's CI (which is
// Linux). It is compiled only on Apple platforms and is never claimed to work
// elsewhere.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "backends/backend.hpp"
#include "backends/backend_registry.hpp"

namespace lx::backends {

class MetalBackend : public Backend {
public:
    explicit MetalBackend(const BackendCreateInfo& info);
    ~MetalBackend() override;

    const BackendInfo& info() const override { return info_; }
    const BackendCapabilities& capabilities() const override { return info_.capabilities; }

    Result<ResourceHandle> create_texture(const TextureDesc& desc) override;
    Status destroy_texture(ResourceHandle handle) override;
    Status upload_texture(ResourceHandle handle, const void* data, u64 bytes,
                          u64 row_pitch = 0) override;
    Result<u64> download_texture(ResourceHandle handle, void* out, u64 out_bytes,
                                 u64 row_pitch = 0) override;

    // Unified memory: the mapped pointer is the texture's own storage.
    Result<MappedTexture> map_texture(ResourceHandle handle) override;
    Status unmap_texture(ResourceHandle handle) override;

    Status register_shader(const std::string& name,
                           const std::vector<u32>& bytecode) override;
    Status register_shader_source(const std::string& name,
                                  const std::string& source) override;
    Status register_cpu_kernel(const std::string& name, CpuKernel kernel) override;
    Status create_pipeline(const std::string& name,
                           const std::string& shader_name) override;

    Result<std::unique_ptr<CommandEncoder>> begin_commands() override;
    Status submit(CommandEncoder* encoder) override;
    Status wait_idle() override;
    Status sync() override;

    u64 allocated_bytes() const override;

    // Factory + probe used by the registry.
    static BackendProbeResult probe();
    static Result<std::unique_ptr<Backend>> create(const BackendCreateInfo& info);

private:
    // Pimpl keeps every Objective-C / Metal type out of this header so it stays
    // includable from ordinary C++ translation units.
    struct Impl;
    std::unique_ptr<Impl> impl_;

    struct TextureRecord {
        TextureDesc desc;
        u64 bytes = 0;
        void* buffer = nullptr;   // id<MTLBuffer>, retained
        void* texture = nullptr;  // id<MTLTexture> view over the buffer, retained
    };
    struct PipelineRecord {
        std::string shader_name;
        void* pipeline_state = nullptr;  // id<MTLComputePipelineState>, retained
    };

    class Encoder;

    BackendInfo info_;
    mutable std::mutex mutex_;
    std::unordered_map<u64, TextureRecord> textures_;
    std::unordered_map<std::string, void*> shader_functions_;  // name -> id<MTLFunction>
    std::unordered_map<std::string, PipelineRecord> pipelines_;
    u64 next_id_ = 1;
    u64 allocated_bytes_ = 0;
};

}  // namespace lx::backends
