// LoucuraXXXX GPU - Vulkan backend (opt-in, cross-platform).
//
// Vulkan is the project's preferred cross-platform GPU API, but it is built
// only when the build is configured with -DLX_ENABLE_VULKAN=ON: the primary
// target machine does not expose a Vulkan device, and a backend that is linked
// but cannot run is worse than one that is absent.
//
// What is real here:
//  * A real VkInstance, VkPhysicalDevice and VkDevice created headlessly (no
//    surface, no swapchain), with a compute-capable queue.
//  * Real VkImage resources with sampled/storage/transfer usage, backed by
//    device-local memory, plus host-visible staging buffers for upload and
//    download.
//  * Real compute pipelines. Vulkan has no "push constants into a fragment
//    pass without a render pass" story, and a headless backend has no
//    swapchain to render into, so a pass is a compute dispatch that reads a
//    sampled image and writes a storage image. That is a genuine use of the
//    API, not an emulation, and it is why this backend reports
//    supports_compute = true while the OpenGL 3.3 and Direct3D 11 backends
//    (fragment-pass based) do not.
//  * Push constants are real Vulkan push constants (vkCmdPushConstants).
//
// Shader sources are GLSL compute shaders compiled to SPIR-V at runtime with
// shaderc when the build found it; register_shader() also accepts pre-compiled
// SPIR-V, so the backend works without shaderc.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "backends/backend.hpp"
#include "backends/backend_registry.hpp"

// Vulkan handles are pointers; forward-declare the dispatchable ones so this
// header does not pull in vulkan.h for every translation unit.
struct VkInstance_T;
struct VkPhysicalDevice_T;
struct VkDevice_T;
struct VkQueue_T;
struct VkCommandPool_T;
struct VkDescriptorPool_T;
struct VkSampler_T;
struct VkPipelineLayout_T;
struct VkDescriptorSetLayout_T;

namespace lx::backends {

class VulkanBackend : public Backend {
public:
    explicit VulkanBackend(const BackendCreateInfo& info);
    ~VulkanBackend() override;

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
        return builtin_compute_source(shader_name);
    }

    Result<std::unique_ptr<CommandEncoder>> begin_commands() override;
    Status submit(CommandEncoder* encoder) override;
    Status wait_idle() override;
    Status sync() override;

    u64 allocated_bytes() const override;

    static BackendProbeResult probe();
    static Result<std::unique_ptr<Backend>> create(const BackendCreateInfo& info);

    // The GLSL compute source built into the backend for `shader_name`, or
    // nullptr when there is no built-in. Exposed so a caller can inspect what
    // the GPU path compiles.
    static const char* builtin_compute_source(const std::string& shader_name);

private:
    struct TextureRecord {
        TextureDesc desc;
        void* image = nullptr;       // VkImage
        void* view = nullptr;        // VkImageView
        void* memory = nullptr;      // VkDeviceMemory
    };

    struct PipelineRecord {
        std::string shader_name;
        void* pipeline = nullptr;    // VkPipeline
        std::vector<u32> spirv;
    };

    class Encoder;

    Status compile_pipeline(PipelineRecord& pipeline, const std::vector<u32>& spirv,
                            const std::string& name);
    TextureRecord* find_texture(ResourceHandle handle);
    void destroy_texture_record(TextureRecord& record);

    // Moves an image between layouts with a one-shot command buffer.
    Status transition_image(void* image, int old_layout, int new_layout);

    BackendInfo info_;
    void* instance_ = nullptr;          // VkInstance
    void* physical_ = nullptr;          // VkPhysicalDevice
    void* device_ = nullptr;            // VkDevice
    void* queue_ = nullptr;             // VkQueue
    u32 queue_family_ = 0;
    void* command_pool_ = nullptr;      // VkCommandPool
    void* descriptor_pool_ = nullptr;   // VkDescriptorPool
    void* set_layout_ = nullptr;        // VkDescriptorSetLayout
    void* pipeline_layout_ = nullptr;   // VkPipelineLayout
    void* sampler_ = nullptr;           // VkSampler
    void* fence_ = nullptr;             // VkFence

    mutable std::mutex mutex_;
    std::unordered_map<u64, TextureRecord> textures_;
    std::unordered_map<std::string, PipelineRecord> pipelines_;
    std::unordered_map<std::string, std::vector<u32>> shader_bytecode_;
    std::unordered_map<std::string, std::string> shader_sources_;
    std::unordered_map<std::string, CpuKernel> kernels_;
    u64 next_id_ = 1;
    u64 allocated_bytes_ = 0;
};

}  // namespace lx::backends
