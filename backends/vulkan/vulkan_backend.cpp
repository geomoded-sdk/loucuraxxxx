// LoucuraXXXX GPU - Vulkan backend implementation.
//
// See vulkan_backend.hpp for the contract. Implementation notes:
//  * Headless: an instance with no surface extensions, one compute-capable
//    queue, and no swapchain. A pass is a compute dispatch.
//  * Images live in device-local memory and stay in VK_IMAGE_LAYOUT_GENERAL so
//    the same image can be sampled and written as a storage image. Every pass
//    is a separate submit that waits on a fence, with a shader-write ->
//    shader-read barrier afterwards, which keeps passes ordered without a
//    command-graph rewrite.
//  * Upload/download go through host-visible staging buffers, honouring the
//    caller's row pitch and the driver's.
//  * Shaders are GLSL compute sources compiled to SPIR-V at runtime when
//    shaderc is present; register_shader() also accepts pre-compiled SPIR-V.
#include "backends/vulkan/vulkan_backend.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <string>

#include "backends/vulkan/vk_shader_compiler.hpp"
#include "core/log.hpp"
#include "backends/device_select.hpp"

#if defined(LX_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif

namespace lx::backends {

#if defined(LX_HAS_VULKAN)

namespace {

constexpr u32 kMaxInputs = 4;
constexpr VkDeviceSize kPushConstantBytes = 64;
constexpr VkDeviceSize kStagingBytes = 1 << 20;  // one page per staging buffer

// Compute pass shader: read the sampled input, write the storage image. The
// per-shader body mirrors the CPU filter exactly, the same maths the GL and
// D3D11 backends use, so all four executors agree.
const char* kComputeHeader = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2D u_image;
layout(binding = 1) uniform sampler2D u_glow;
layout(binding = 2, rgba8) uniform image2D out_image;
layout(push_constant) uniform Push {
)";

const char* kComputeFooter = R"(} pc;

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
)";

const char* kToneBody = R"(
void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(out_image);
    if (coord.x >= size.x || coord.y >= size.y) return;
    vec4 src = texelFetch(u_image, coord, 0);
    vec3 rgb = src.rgb * 255.0;
    rgb *= pow(2.0, pc.exposure_stops);
    rgb = (rgb - 128.0) * (1.0 + pc.contrast) + 128.0;
    float luma = dot(clamp(rgb, 0.0, 255.0), kLuma);
    rgb = vec3(luma) + (rgb - vec3(luma)) * pc.saturation;
    vec3 blended = mix(src.rgb * 255.0, rgb, pc.intensity);
    imageStore(out_image, coord, vec4(clamp(blended, 0.0, 255.0) / 255.0, src.a));
}
)";

const char* kVignetteBody = R"(
void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(out_image);
    if (coord.x >= size.x || coord.y >= size.y) return;
    vec4 src = texelFetch(u_image, coord, 0);
    vec2 p = vec2(coord);
    vec2 center = pc.image_size * 0.5;
    float max_radius = length(center);
    float radius = length(p - center) / max_radius;
    float t = clamp((radius - pc.inner) / max(pc.outer - pc.inner, 1e-3), 0.0, 1.0);
    float falloff = t * t * (3.0 - 2.0 * t);
    float factor = 1.0 - falloff * pc.intensity;
    imageStore(out_image, coord, vec4(src.rgb * factor, src.a));
}
)";

const char* kSharpenBody = R"(
void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(out_image);
    if (coord.x >= size.x || coord.y >= size.y) return;
    ivec2 hi = ivec2(pc.image_size) - 1;
    vec3 avg = vec3(0.0);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            ivec2 s = clamp(coord + ivec2(dx, dy), ivec2(0), hi);
            avg += texelFetch(u_image, s, 0).rgb;
        }
    avg /= 9.0;
    vec4 src = texelFetch(u_image, coord, 0);
    vec3 sharpened = src.rgb + pc.strength * (src.rgb - avg);
    imageStore(out_image, coord, vec4(clamp(sharpened, 0.0, 1.0), src.a));
}
)";

const char* kBlurBody = R"(
void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(out_image);
    if (coord.x >= size.x || coord.y >= size.y) return;
    ivec2 hi = ivec2(pc.image_size) - 1;
    int radius = pc.radius;
    float denom = 2.0 * pc.sigma * pc.sigma;
    vec3 accum = vec3(0.0);
    float weight_sum = 0.0;
    for (int k = -radius; k <= radius; ++k) {
        ivec2 s = coord;
        if (pc.axis == 0) s.x = clamp(coord.x + k, 0, hi.x);
        else              s.y = clamp(coord.y + k, 0, hi.y);
        float w = exp(-float(k * k) / denom);
        accum += texelFetch(u_image, s, 0).rgb * w;
        weight_sum += w;
    }
    vec3 blurred = accum / weight_sum;
    vec4 src = texelFetch(u_image, coord, 0);
    imageStore(out_image, coord, vec4(mix(src.rgb, blurred, pc.intensity), src.a));
}
)";

const char* kCopyBody = R"(
void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(out_image);
    if (coord.x >= size.x || coord.y >= size.y) return;
    imageStore(out_image, coord, texelFetch(u_image, coord, 0));
}
)";

const char* kBloomBrightBody = R"(
void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(out_image);
    if (coord.x >= size.x || coord.y >= size.y) return;
    vec4 src = texelFetch(u_image, coord, 0);
    float luma = dot(src.rgb * 255.0, kLuma);
    imageStore(out_image, coord, (luma > pc.threshold) ? src : vec4(0.0, 0.0, 0.0, src.a));
}
)";

const char* kBloomBlurBody = R"(
void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(out_image);
    if (coord.x >= size.x || coord.y >= size.y) return;
    ivec2 hi = ivec2(pc.image_size) - 1;
    int radius = pc.radius;
    vec3 accum = vec3(0.0);
    float count = 0.0;
    for (int k = -radius; k <= radius; ++k) {
        ivec2 s = coord;
        if (pc.axis == 0) s.x = clamp(coord.x + k, 0, hi.x);
        else              s.y = clamp(coord.y + k, 0, hi.y);
        accum += texelFetch(u_image, s, 0).rgb;
        count += 1.0;
    }
    vec4 src = texelFetch(u_image, coord, 0);
    imageStore(out_image, coord, vec4(accum / count, src.a));
}
)";

const char* kBloomCombineBody = R"(
void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(out_image);
    if (coord.x >= size.x || coord.y >= size.y) return;
    vec4 base = texelFetch(u_image, coord, 0);
    vec4 glow = texelFetch(u_glow, coord, 0);
    imageStore(out_image, coord,
               vec4(clamp(base.rgb + glow.rgb * pc.intensity, 0.0, 1.0), base.a));
}
)";

// Push-constant field declarations. The field order matches filters/gpu_plan.hpp
// field for field, so one C++ push struct fills every backend's block.
const char* kToneUniforms = R"(
    float exposure_stops;
    float contrast;
    float saturation;
    float intensity;
)";
const char* kVignetteUniforms = R"(
    float inner;
    float outer;
    float intensity;
    float _pad0;
    vec2  image_size;
    vec2  _pad1;
)";
const char* kSharpenUniforms = R"(
    float strength;
    vec2  image_size;
    vec2  _pad;
)";
const char* kBlurUniforms = R"(
    float sigma;
    float intensity;
    vec2  image_size;
    int   axis;
    int   radius;
    int   _pad0;
    int   _pad1;
)";
const char* kBloomBrightUniforms = R"(
    float threshold;
    float _pad0;
    vec2  _pad1;
)";
const char* kBloomBlurUniforms = R"(
    int   radius;
    int   axis;
    vec2  image_size;
)";
const char* kBloomCombineUniforms = R"(
    float intensity;
    float _pad0;
    vec2  _pad1;
)";
const char* kCopyUniforms = R"(
    float _pad0;
    float _pad1;
    vec2  _pad2;
)";

std::string build_compute_source(const char* uniforms, const char* body) {
    std::string source = kComputeHeader;
    source += uniforms;
    source += kComputeFooter;
    source += body;
    return source;
}

}  // namespace

const char* VulkanBackend::builtin_compute_source(const std::string& shader_name) {
    static std::unordered_map<std::string, std::string> cache;
    auto it = cache.find(shader_name);
    if (it != cache.end()) return it->second.c_str();

    std::string source;
    if (shader_name == "tone") {
        source = build_compute_source(kToneUniforms, kToneBody);
    } else if (shader_name == "vignette") {
        source = build_compute_source(kVignetteUniforms, kVignetteBody);
    } else if (shader_name == "sharpen") {
        source = build_compute_source(kSharpenUniforms, kSharpenBody);
    } else if (shader_name == "blur_horizontal" || shader_name == "blur_vertical") {
        source = build_compute_source(kBlurUniforms, kBlurBody);
    } else if (shader_name == "bloom_bright") {
        source = build_compute_source(kBloomBrightUniforms, kBloomBrightBody);
    } else if (shader_name == "bloom_blur_horizontal" ||
               shader_name == "bloom_blur_vertical") {
        source = build_compute_source(kBloomBlurUniforms, kBloomBlurBody);
    } else if (shader_name == "bloom_combine") {
        source = build_compute_source(kBloomCombineUniforms, kBloomCombineBody);
    } else if (shader_name == "copy") {
        source = build_compute_source(kCopyUniforms, kCopyBody);
    } else {
        return nullptr;
    }
    auto inserted = cache.emplace(shader_name, std::move(source));
    return inserted.first->second.c_str();
}

namespace {

// VkImage / VkImageView / VkDeviceMemory are opaque pointers to us, so these
// helpers keep the casts in one place.
VkImage as_image(void* p) { return reinterpret_cast<VkImage>(p); }
VkImageView as_view(void* p) { return reinterpret_cast<VkImageView>(p); }
VkDeviceMemory as_memory(void* p) { return reinterpret_cast<VkDeviceMemory>(p); }
VkBuffer as_buffer(void* p) { return reinterpret_cast<VkBuffer>(p); }

u32 find_memory_type(VkPhysicalDevice physical, u32 type_bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    for (u32 i = 0; i < mem.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

}  // namespace

// Records one compute pass. Because a pass is a compute dispatch, the encoder
// captures the pipeline, the input/output textures and the push constants, and
// submit() records and executes them.
class VulkanBackend::Encoder : public CommandEncoder {
public:
    explicit Encoder(VulkanBackend& backend) : backend_(backend) {}

    Status bind_pipeline(const std::string& pipeline_name) override {
        auto it = backend_.pipelines_.find(pipeline_name);
        if (it == backend_.pipelines_.end()) {
            return Status::error(ErrorCode::InvalidArgument,
                                 "unknown pipeline '" + pipeline_name + "'");
        }
        pipeline_ = it->second;
        have_pipeline_ = true;
        return Status::success();
    }

    Status bind_texture(u32 slot, ResourceHandle texture) override {
        if (slot >= kMaxInputs) {
            return Status::error(ErrorCode::InvalidArgument,
                                 "Vulkan backend binds at most " +
                                     std::to_string(kMaxInputs) + " input textures");
        }
        inputs_[slot] = texture;
        input_count_ = std::max(input_count_, slot + 1);
        return Status::success();
    }

    Status bind_storage_texture(u32 slot, ResourceHandle texture) override {
        if (slot != 0) {
            return Status::error(ErrorCode::InvalidArgument,
                                 "Vulkan backend binds the output at slot 0");
        }
        output_ = texture;
        return Status::success();
    }

    Status set_push_constants(const void* data, u32 bytes) override {
        push_.assign(static_cast<const u8*>(data),
                     static_cast<const u8*>(data) + bytes);
        return Status::success();
    }

    Status dispatch(const DispatchDesc&) override { return Status::success(); }

    Status execute();

private:
    VulkanBackend& backend_;
    PipelineRecord pipeline_;
    bool have_pipeline_ = false;
    std::array<ResourceHandle, kMaxInputs> inputs_{};
    u32 input_count_ = 0;
    ResourceHandle output_{};
    std::vector<u8> push_;
};

Status VulkanBackend::Encoder::execute() {
    if (!have_pipeline_ || input_count_ == 0 || !output_.valid()) {
        return Status::error(ErrorCode::InvalidArgument,
                             "pass needs a pipeline, an input and an output");
    }
    auto* dst = backend_.find_texture(output_);
    if (!dst) {
        return Status::error(ErrorCode::InvalidArgument, "pass references a dead texture");
    }
    for (u32 i = 0; i < input_count_; ++i) {
        auto* src = backend_.find_texture(inputs_[i]);
        if (!src) {
            return Status::error(ErrorCode::InvalidArgument,
                                 "pass references a dead input texture");
        }
        if (src->desc.width != dst->desc.width || src->desc.height != dst->desc.height) {
            return Status::error(ErrorCode::InvalidArgument,
                                 "input and output textures differ in size");
        }
    }
    if (pipeline_.pipeline == nullptr) {
        return Status::error(ErrorCode::NotImplemented,
                             "pipeline has no shader; register its source first");
    }

    VkDevice device = reinterpret_cast<VkDevice>(backend_.device_);

    // Allocate a descriptor set from the pool for this dispatch.
    VkDescriptorSetLayout set_layout = reinterpret_cast<VkDescriptorSetLayout>(backend_.set_layout_);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = reinterpret_cast<VkDescriptorPool>(backend_.descriptor_pool_);
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &alloc, &set) != VK_SUCCESS) {
        // Pool exhausted: reset and retry once.
        vkResetDescriptorPool(device, reinterpret_cast<VkDescriptorPool>(backend_.descriptor_pool_), 0);
        if (vkAllocateDescriptorSets(device, &alloc, &set) != VK_SUCCESS) {
            return Status::error(ErrorCode::ResourceBusy, "descriptor set allocation failed");
        }
    }

    auto* src0 = backend_.find_texture(inputs_[0]);
    auto* src1 = backend_.find_texture(input_count_ >= 2 ? inputs_[1] : inputs_[0]);
    VkDescriptorImageInfo img0{};
    img0.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    img0.imageView = as_view(src0->view);
    img0.sampler = reinterpret_cast<VkSampler>(backend_.sampler_);
    VkDescriptorImageInfo img1{};
    img1.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    img1.imageView = as_view(src1->view);
    img1.sampler = reinterpret_cast<VkSampler>(backend_.sampler_);
    VkDescriptorImageInfo out{};
    out.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    out.imageView = as_view(dst->view);
    out.sampler = VK_NULL_HANDLE;

    std::array<VkWriteDescriptorSet, 3> writes{};
    for (auto& w : writes) {
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.descriptorCount = 1;
    }
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &img0;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &img1;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &out;
    vkUpdateDescriptorSets(device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);

    // One command buffer per pass; record, submit and wait.
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb.commandPool = reinterpret_cast<VkCommandPool>(backend_.command_pool_);
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &cb, &cmd) != VK_SUCCESS) {
        vkFreeDescriptorSets(device, reinterpret_cast<VkDescriptorPool>(backend_.descriptor_pool_), 1, &set);
        return Status::error(ErrorCode::ResourceBusy, "command buffer allocation failed");
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      reinterpret_cast<VkPipeline>(pipeline_.pipeline));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            reinterpret_cast<VkPipelineLayout>(backend_.pipeline_layout_), 0, 1,
                            &set, 0, nullptr);
    if (!push_.empty()) {
        vkCmdPushConstants(cmd, reinterpret_cast<VkPipelineLayout>(backend_.pipeline_layout_),
                           VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           static_cast<u32>(push_.size()), push_.data());
    }
    const u32 groups_x = (dst->desc.width + 7) / 8;
    const u32 groups_y = (dst->desc.height + 7) / 8;
    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // Make the write visible to the next pass's reads.
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VkFence fence = reinterpret_cast<VkFence>(backend_.fence_);
    vkResetFences(device, 1, &fence);
    Status status = Status::success();
    if (vkQueueSubmit(reinterpret_cast<VkQueue>(backend_.queue_), 1, &submit, fence) != VK_SUCCESS) {
        status = Status::error(ErrorCode::Unknown, "vkQueueSubmit failed");
    } else {
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    }

    vkFreeCommandBuffers(device, reinterpret_cast<VkCommandPool>(backend_.command_pool_), 1, &cmd);
    vkFreeDescriptorSets(device, reinterpret_cast<VkDescriptorPool>(backend_.descriptor_pool_), 1, &set);
    return status;
}

namespace {

// Standard Khronos validation layer. Enabling it is the whole point of
// `allow_validation`, and enabling a layer that is not installed makes
// vkCreateInstance fail, which would turn "please validate" into "no backend".
// So the layer is looked up first and only requested when present.
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

bool validation_layer_available() {
    u32 count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0) {
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
        return false;
    }
    for (const auto& layer : layers) {
        if (std::strcmp(layer.layerName, kValidationLayer) == 0) return true;
    }
    return false;
}

// Fills the instance create info's layer list. Returns false when validation was
// requested but the layer is absent, so the caller can say so instead of quietly
// dropping the request.
bool apply_validation(VkInstanceCreateInfo& ici, const BackendCreateInfo& info,
                      bool* enabled) {
    *enabled = false;
    if (!info.allow_validation) return true;
    if (!validation_layer_available()) return false;
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = &kValidationLayer;
    *enabled = true;
    return true;
}

}  // namespace

VulkanBackend::VulkanBackend(const BackendCreateInfo& info) {
    info_.kind = BackendKind::Vulkan;
    info_.driver_name = "loucuraxxxx-vulkan";
    info_.api_version = "1.0";
    info_.maturity = Maturity::Planned;
    info_.device_name = "Vulkan unavailable";

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "LoucuraXXXX GPU";
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    bool validation_enabled = false;
    if (!apply_validation(ici, info, &validation_enabled)) {
        // The config asked for validation and the layer is not installed. Saying
        // so is the honest outcome; silently running unvalidated would hide it.
        LX_WARN << "Vulkan validation requested but " << kValidationLayer
                << " is not installed; running without it";
    }
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        LX_WARN << "Vulkan instance creation failed";
        return;
    }
    if (validation_enabled) {
        LX_INFO << "Vulkan validation layer enabled";
    }
    instance_ = instance;

    u32 device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (device_count == 0) {
        LX_WARN << "Vulkan: no physical devices";
        return;
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    // Finds a compute queue family on `dev`. Returns false when it has none.
    const auto find_compute_family = [](VkPhysicalDevice dev, u32* out_family) {
        u32 count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
        if (count == 0) return false;
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families.data());
        for (u32 i = 0; i < count; ++i) {
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                *out_family = i;
                return true;
            }
        }
        return false;
    };

    // Only devices with a compute queue are candidates, so the names offered to
    // the selector are exactly the ones we could actually use.
    std::vector<std::string> usable_names;
    std::vector<std::pair<VkPhysicalDevice, u32>> usable;
    for (VkPhysicalDevice dev : devices) {
        u32 candidate = 0;
        if (!find_compute_family(dev, &candidate)) continue;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);
        usable_names.emplace_back(props.deviceName);
        usable.emplace_back(dev, candidate);
    }
    if (usable.empty()) {
        LX_WARN << "Vulkan: no compute-capable queue family";
        return;
    }

    bool preference_honoured = false;
    const std::size_t index = select_device(usable_names, info.preferred_device,
                                            &preference_honoured);
    if (!info.preferred_device.empty() && !preference_honoured) {
        // Say it rather than silently using a different card: a stale config must
        // not take the GPU away, but it must not look satisfied either.
        LX_WARN << "Vulkan: no compute-capable device matches preferred_device '"
                << info.preferred_device << "'; using '" << usable_names[index] << "'";
    }
    const VkPhysicalDevice chosen = usable[index].first;
    const u32 family = usable[index].second;
    physical_ = chosen;
    queue_family_ = family;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(chosen, &dci, nullptr, &device) != VK_SUCCESS) {
        LX_WARN << "Vulkan device creation failed";
        return;
    }
    device_ = device;
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);
    queue_ = queue;

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device, &pci, nullptr, &pool) != VK_SUCCESS) {
        LX_WARN << "Vulkan command pool creation failed";
        return;
    }
    command_pool_ = pool;

    // Descriptor set layout: two sampled images and one storage image.
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1] = bindings[0];
    bindings[1].binding = 1;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = static_cast<u32>(bindings.size());
    sli.pBindings = bindings.data();
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &sli, nullptr, &set_layout) != VK_SUCCESS) {
        LX_WARN << "Vulkan descriptor set layout creation failed";
        return;
    }
    set_layout_ = set_layout;

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.offset = 0;
    range.size = static_cast<u32>(kPushConstantBytes);
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &pli, nullptr, &layout) != VK_SUCCESS) {
        LX_WARN << "Vulkan pipeline layout creation failed";
        return;
    }
    pipeline_layout_ = layout;

    std::array<VkDescriptorPoolSize, 2> pool_sizes{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = 256;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[1].descriptorCount = 128;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 128;
    dpi.poolSizeCount = static_cast<u32>(pool_sizes.size());
    dpi.pPoolSizes = pool_sizes.data();
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device, &dpi, nullptr, &dpool) != VK_SUCCESS) {
        LX_WARN << "Vulkan descriptor pool creation failed";
        return;
    }
    descriptor_pool_ = dpool;

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device, &si, nullptr, &sampler) != VK_SUCCESS) {
        LX_WARN << "Vulkan sampler creation failed";
        return;
    }
    sampler_ = sampler;

    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(device, &fi, nullptr, &fence) != VK_SUCCESS) {
        LX_WARN << "Vulkan fence creation failed";
        return;
    }
    fence_ = fence;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(chosen, &props);
    info_.device_name = props.deviceName;
    info_.api_version =
        std::to_string(VK_VERSION_MAJOR(props.apiVersion)) + "." +
        std::to_string(VK_VERSION_MINOR(props.apiVersion)) + "." +
        std::to_string(VK_VERSION_PATCH(props.apiVersion));
    info_.maturity = Maturity::Implemented;

    info_.capabilities.is_hardware =
        props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU &&
        !looks_like_software_rasterizer(props.deviceName) && !running_under_wine();
    info_.capabilities.supports_compute = true;
    info_.capabilities.supports_storage_textures = true;
    info_.capabilities.supports_float_textures = true;
    info_.capabilities.supports_parallel_dispatch = true;
    info_.capabilities.supports_timestamps = props.limits.timestampComputeAndGraphics == VK_TRUE;
    info_.capabilities.max_texture_dimension = props.limits.maxImageDimension2D;
    info_.capabilities.max_workgroup_size = props.limits.maxComputeWorkGroupInvocations;
    info_.capabilities.max_memory_bytes = info.memory_budget_bytes;
    info_.capabilities.unified_memory = false;
    info_.capabilities.supports_host_mapping = false;

    LX_INFO << "Vulkan backend ready: " << info_.device_name << " (" << info_.api_version << ")";
}

VulkanBackend::~VulkanBackend() {
    if (device_ == nullptr) {
        if (instance_) vkDestroyInstance(reinterpret_cast<VkInstance>(instance_), nullptr);
        return;
    }
    VkDevice device = reinterpret_cast<VkDevice>(device_);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, record] : textures_) destroy_texture_record(record);
    textures_.clear();
    for (auto& [name, pipeline] : pipelines_) {
        if (pipeline.pipeline) vkDestroyPipeline(device, reinterpret_cast<VkPipeline>(pipeline.pipeline), nullptr);
    }
    pipelines_.clear();
    if (fence_) vkDestroyFence(device, reinterpret_cast<VkFence>(fence_), nullptr);
    if (sampler_) vkDestroySampler(device, reinterpret_cast<VkSampler>(sampler_), nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(device, reinterpret_cast<VkDescriptorPool>(descriptor_pool_), nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device, reinterpret_cast<VkPipelineLayout>(pipeline_layout_), nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device, reinterpret_cast<VkDescriptorSetLayout>(set_layout_), nullptr);
    if (command_pool_) vkDestroyCommandPool(device, reinterpret_cast<VkCommandPool>(command_pool_), nullptr);
    vkDestroyDevice(device, nullptr);
    if (instance_) vkDestroyInstance(reinterpret_cast<VkInstance>(instance_), nullptr);
}

VulkanBackend::TextureRecord* VulkanBackend::find_texture(ResourceHandle handle) {
    auto it = textures_.find(handle.id);
    return it == textures_.end() ? nullptr : &it->second;
}

void VulkanBackend::destroy_texture_record(TextureRecord& record) {
    VkDevice device = reinterpret_cast<VkDevice>(device_);
    if (record.view) vkDestroyImageView(device, as_view(record.view), nullptr);
    if (record.image) vkDestroyImage(device, as_image(record.image), nullptr);
    if (record.memory) vkFreeMemory(device, as_memory(record.memory), nullptr);
}

Status VulkanBackend::transition_image(void* image, int old_layout, int new_layout) {
    VkDevice device = reinterpret_cast<VkDevice>(device_);
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb.commandPool = reinterpret_cast<VkCommandPool>(command_pool_);
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &cb, &cmd) != VK_SUCCESS) {
        return Status::error(ErrorCode::ResourceBusy, "command buffer allocation failed");
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = static_cast<VkImageLayout>(old_layout);
    barrier.newLayout = static_cast<VkImageLayout>(new_layout);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = as_image(image);
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VkFence fence = reinterpret_cast<VkFence>(fence_);
    vkResetFences(device, 1, &fence);
    Status status = Status::success();
    if (vkQueueSubmit(reinterpret_cast<VkQueue>(queue_), 1, &submit, fence) != VK_SUCCESS) {
        status = Status::error(ErrorCode::Unknown, "vkQueueSubmit (layout transition) failed");
    } else {
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    vkFreeCommandBuffers(device, reinterpret_cast<VkCommandPool>(command_pool_), 1, &cmd);
    return status;
}

Result<ResourceHandle> VulkanBackend::create_texture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return Status::error(ErrorCode::InvalidArgument,
                             "texture dimensions must be non-zero");
    }
    if (info_.capabilities.max_texture_dimension != 0 &&
        (desc.width > info_.capabilities.max_texture_dimension ||
         desc.height > info_.capabilities.max_texture_dimension)) {
        return Status::error(ErrorCode::OutOfRange, "texture exceeds maximum dimension");
    }
    const u64 bytes = texture_byte_size(desc.width, desc.height, desc.format);
    if (info_.capabilities.max_memory_bytes != 0 &&
        bytes > info_.capabilities.max_memory_bytes - allocated_bytes_) {
        return Status::error(ErrorCode::OutOfMemory, "texture exceeds the memory budget");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    VkDevice device = reinterpret_cast<VkDevice>(device_);

    TextureRecord record;
    record.desc = desc;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.extent.width = desc.width;
    ici.extent.height = desc.height;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device, &ici, nullptr, &image) != VK_SUCCESS) {
        return Status::error(ErrorCode::OutOfMemory, "vkCreateImage failed");
    }
    record.image = image;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, image, &req);
    const u32 type = find_memory_type(reinterpret_cast<VkPhysicalDevice>(physical_),
                                      req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        vkDestroyImage(device, image, nullptr);
        record.image = nullptr;
        return Status::error(ErrorCode::OutOfMemory, "no device-local memory type for image");
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &mai, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyImage(device, image, nullptr);
        record.image = nullptr;
        return Status::error(ErrorCode::OutOfMemory, "vkAllocateMemory for image failed");
    }
    record.memory = memory;
    if (vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
        destroy_texture_record(record);
        return Status::error(ErrorCode::InitializationFailed, "vkBindImageMemory failed");
    }

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ici.format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &vci, nullptr, &view) != VK_SUCCESS) {
        destroy_texture_record(record);
        return Status::error(ErrorCode::InitializationFailed, "vkCreateImageView failed");
    }
    record.view = view;

    // Bring the image into the layout the compute passes use.
    LX_TRY(transition_image(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL));

    const u64 id = next_id_++;
    allocated_bytes_ += bytes;
    textures_.emplace(id, record);
    return ResourceHandle{id};
}

Status VulkanBackend::destroy_texture(ResourceHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle.id);
    if (it == textures_.end()) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    const u64 bytes = texture_byte_size(it->second.desc.width, it->second.desc.height,
                                        it->second.desc.format);
    destroy_texture_record(it->second);
    allocated_bytes_ -= bytes;
    textures_.erase(it);
    return Status::success();
}

Status VulkanBackend::upload_texture(ResourceHandle handle, const void* data, u64 bytes,
                                     u64 row_pitch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* record = find_texture(handle);
    if (!record) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    const u64 tight_row = static_cast<u64>(record->desc.width) * 4;
    const u64 pitch = row_pitch == 0 ? tight_row : row_pitch;
    if (pitch < tight_row) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch smaller than a row");
    }
    if (record->desc.height > 1 &&
        pitch > (std::numeric_limits<u64>::max() - tight_row) / (record->desc.height - 1)) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch is absurd");
    }
    const u64 required = pitch * (record->desc.height - 1) + tight_row;
    if (bytes < required) {
        return Status::error(ErrorCode::InvalidArgument, "source buffer too small");
    }

    VkDevice device = reinterpret_cast<VkDevice>(device_);
    const u64 total = tight_row * record->desc.height;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = total;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(device, &bci, nullptr, &buffer) != VK_SUCCESS) {
        return Status::error(ErrorCode::OutOfMemory, "staging buffer creation failed");
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer, &req);
    const u32 type = find_memory_type(reinterpret_cast<VkPhysicalDevice>(physical_),
                                      req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (type == UINT32_MAX || vkAllocateMemory(device, &mai, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer, nullptr);
        return Status::error(ErrorCode::OutOfMemory, "staging memory allocation failed");
    }
    vkBindBufferMemory(device, buffer, memory, 0);

    void* mapped = nullptr;
    if (vkMapMemory(device, memory, 0, total, 0, &mapped) != VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        return Status::error(ErrorCode::InitializationFailed, "vkMapMemory for upload failed");
    }
    const auto* src = static_cast<const u8*>(data);
    for (u32 y = 0; y < record->desc.height; ++y) {
        std::memcpy(static_cast<u8*>(mapped) + static_cast<std::size_t>(y) * tight_row,
                    src + static_cast<std::size_t>(y) * pitch,
                    static_cast<std::size_t>(tight_row));
    }
    vkUnmapMemory(device, memory);

    Status status = transition_image(record->image, VK_IMAGE_LAYOUT_GENERAL,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (status.ok()) {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cb.commandPool = reinterpret_cast<VkCommandPool>(command_pool_);
        cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &cb, &cmd) != VK_SUCCESS) {
            status = Status::error(ErrorCode::ResourceBusy, "command buffer allocation failed");
        } else {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &begin);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = record->desc.width;
            region.imageExtent.height = record->desc.height;
            region.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(cmd, buffer, as_image(record->image),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            VkFence fence = reinterpret_cast<VkFence>(fence_);
            vkResetFences(device, 1, &fence);
            if (vkQueueSubmit(reinterpret_cast<VkQueue>(queue_), 1, &submit, fence) != VK_SUCCESS) {
                status = Status::error(ErrorCode::Unknown, "vkQueueSubmit (upload) failed");
            } else {
                vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
            }
            vkFreeCommandBuffers(device, reinterpret_cast<VkCommandPool>(command_pool_), 1, &cmd);
        }
        transition_image(record->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_GENERAL);
    }

    vkFreeMemory(device, memory, nullptr);
    vkDestroyBuffer(device, buffer, nullptr);
    return status;
}

Result<u64> VulkanBackend::download_texture(ResourceHandle handle, void* out, u64 out_bytes,
                                            u64 row_pitch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* record = find_texture(handle);
    if (!record) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    const u64 tight_row = static_cast<u64>(record->desc.width) * 4;
    const u64 pitch = row_pitch == 0 ? tight_row : row_pitch;
    if (pitch < tight_row) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch smaller than a row");
    }
    if (record->desc.height > 1 &&
        pitch > (std::numeric_limits<u64>::max() - tight_row) / (record->desc.height - 1)) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch is absurd");
    }
    const u64 required = pitch * (record->desc.height - 1) + tight_row;
    if (out_bytes < required) {
        return Status::error(ErrorCode::InvalidArgument, "destination buffer too small");
    }

    VkDevice device = reinterpret_cast<VkDevice>(device_);
    const u64 total = tight_row * record->desc.height;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = total;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(device, &bci, nullptr, &buffer) != VK_SUCCESS) {
        return Status::error(ErrorCode::OutOfMemory, "staging buffer creation failed");
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer, &req);
    const u32 type = find_memory_type(reinterpret_cast<VkPhysicalDevice>(physical_),
                                      req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (type == UINT32_MAX || vkAllocateMemory(device, &mai, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer, nullptr);
        return Status::error(ErrorCode::OutOfMemory, "staging memory allocation failed");
    }
    vkBindBufferMemory(device, buffer, memory, 0);

    Status status = transition_image(record->image, VK_IMAGE_LAYOUT_GENERAL,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (status.ok()) {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cb.commandPool = reinterpret_cast<VkCommandPool>(command_pool_);
        cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &cb, &cmd) != VK_SUCCESS) {
            status = Status::error(ErrorCode::ResourceBusy, "command buffer allocation failed");
        } else {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &begin);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = record->desc.width;
            region.imageExtent.height = record->desc.height;
            region.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(cmd, as_image(record->image),
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            VkFence fence = reinterpret_cast<VkFence>(fence_);
            vkResetFences(device, 1, &fence);
            if (vkQueueSubmit(reinterpret_cast<VkQueue>(queue_), 1, &submit, fence) != VK_SUCCESS) {
                status = Status::error(ErrorCode::Unknown, "vkQueueSubmit (download) failed");
            } else {
                vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
            }
            vkFreeCommandBuffers(device, reinterpret_cast<VkCommandPool>(command_pool_), 1, &cmd);
        }
        transition_image(record->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_GENERAL);
    }

    if (status.ok()) {
        void* mapped = nullptr;
        if (vkMapMemory(device, memory, 0, total, 0, &mapped) != VK_SUCCESS) {
            status = Status::error(ErrorCode::InitializationFailed, "vkMapMemory for download failed");
        } else {
            auto* dst = static_cast<u8*>(out);
            for (u32 y = 0; y < record->desc.height; ++y) {
                std::memcpy(dst + static_cast<std::size_t>(y) * pitch,
                            static_cast<const u8*>(mapped) + static_cast<std::size_t>(y) * tight_row,
                            static_cast<std::size_t>(tight_row));
            }
            vkUnmapMemory(device, memory);
        }
    }

    vkFreeMemory(device, memory, nullptr);
    vkDestroyBuffer(device, buffer, nullptr);
    return status.ok() ? Result<u64>(required) : Result<u64>(status);
}

Status VulkanBackend::register_shader(const std::string& name,
                                      const std::vector<u32>& bytecode) {
    std::lock_guard<std::mutex> lock(mutex_);
    shader_bytecode_[name] = bytecode;
    return Status::success();
}

Status VulkanBackend::register_shader_source(const std::string& name,
                                             const std::string& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    shader_sources_[name] = source;
    return Status::success();
}

Status VulkanBackend::register_cpu_kernel(const std::string& name, CpuKernel kernel) {
    std::lock_guard<std::mutex> lock(mutex_);
    kernels_[name] = kernel;
    return Status::success();
}

Status VulkanBackend::compile_pipeline(PipelineRecord& pipeline, const std::vector<u32>& spirv,
                                       const std::string& name) {
    VkDevice device = reinterpret_cast<VkDevice>(device_);
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spirv.size() * sizeof(u32);
    smci.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &smci, nullptr, &module) != VK_SUCCESS) {
        return Status::error(ErrorCode::InitializationFailed,
                             "vkCreateShaderModule failed for '" + name + "'");
    }
    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName = "main";
    cpci.layout = reinterpret_cast<VkPipelineLayout>(pipeline_layout_);
    VkPipeline created = VK_NULL_HANDLE;
    const VkResult res =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &created);
    vkDestroyShaderModule(device, module, nullptr);
    if (res != VK_SUCCESS) {
        return Status::error(ErrorCode::InitializationFailed,
                             "vkCreateComputePipelines failed for '" + name + "'");
    }
    if (pipeline.pipeline) {
        vkDestroyPipeline(device, reinterpret_cast<VkPipeline>(pipeline.pipeline), nullptr);
    }
    pipeline.pipeline = created;
    pipeline.spirv = spirv;
    return Status::success();
}

Status VulkanBackend::create_pipeline(const std::string& name,
                                      const std::string& shader_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    PipelineRecord pipeline;
    pipeline.shader_name = shader_name;

    auto bytecode_it = shader_bytecode_.find(shader_name);
    if (bytecode_it != shader_bytecode_.end()) {
        LX_TRY(compile_pipeline(pipeline, bytecode_it->second, shader_name));
    } else {
        // Compile GLSL to SPIR-V: the registered source if there is one, else the
        // backend's own built-in for this pass.
        std::string source;
        auto source_it = shader_sources_.find(shader_name);
        if (source_it != shader_sources_.end()) {
            source = source_it->second;
        } else if (const char* builtin = builtin_compute_source(shader_name)) {
            source = builtin;
        } else {
            pipelines_[name] = pipeline;  // no shader; a pass using it reports NotImplemented
            return Status::success();
        }
        std::vector<u32> spirv;
        std::string error;
        if (!vk::compile_glsl(source, vk::ShaderStage::Compute, shader_name, spirv, error)) {
            return Status::error(ErrorCode::NotSupported,
                                 "cannot compile '" + shader_name + "' to SPIR-V: " + error);
        }
        LX_TRY(compile_pipeline(pipeline, spirv, shader_name));
    }
    pipelines_[name] = pipeline;
    return Status::success();
}

Result<std::unique_ptr<CommandEncoder>> VulkanBackend::begin_commands() {
    return std::unique_ptr<CommandEncoder>(new Encoder(*this));
}

Status VulkanBackend::submit(CommandEncoder* encoder) {
    if (encoder == nullptr) return sync();
    auto* vk_encoder = dynamic_cast<Encoder*>(encoder);
    if (vk_encoder == nullptr) {
        return Status::error(ErrorCode::InvalidArgument,
                             "encoder was not created by this backend");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return vk_encoder->execute();
}

Status VulkanBackend::wait_idle() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_ == nullptr) return Status::success();
    return vkQueueWaitIdle(reinterpret_cast<VkQueue>(queue_)) == VK_SUCCESS
               ? Status::success()
               : Status::error(ErrorCode::Unknown, "vkQueueWaitIdle failed");
}

Status VulkanBackend::sync() { return wait_idle(); }

u64 VulkanBackend::allocated_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocated_bytes_;
}

BackendProbeResult VulkanBackend::probe() {
    BackendProbeResult result;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        result.reason = "vkCreateInstance failed";
        return result;
    }
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) {
        result.reason = "no Vulkan physical devices";
        vkDestroyInstance(instance, nullptr);
        return result;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    bool compute = false;
    bool hardware = false;
    // Scan every device rather than stopping at the first compute-capable one:
    // a host can expose a software ICD alongside a real GPU, and stopping early
    // would report the wrong one. A hardware device wins over software.
    for (VkPhysicalDevice dev : devices) {
        u32 families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &families, nullptr);
        std::vector<VkQueueFamilyProperties> props(families);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &families, props.data());
        bool has_compute = false;
        for (const auto& p : props) {
            if (p.queueFlags & VK_QUEUE_COMPUTE_BIT) {
                has_compute = true;
                break;
            }
        }
        if (!has_compute) continue;
        VkPhysicalDeviceProperties dp{};
        vkGetPhysicalDeviceProperties(dev, &dp);
        const bool device_is_hardware =
            dp.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU &&
            !looks_like_software_rasterizer(dp.deviceName) && !running_under_wine();
        if (!compute || (device_is_hardware && !hardware)) {
            compute = true;
            hardware = device_is_hardware;
            result.device_name = dp.deviceName;
        }
        if (hardware) break;
    }
    vkDestroyInstance(instance, nullptr);
    if (!compute) {
        result.reason = "no compute-capable Vulkan device";
        return result;
    }
    result.available = true;
    result.is_hardware = hardware;
    result.reason = hardware ? "Vulkan compute device available"
                             : "Vulkan compute device available (software rasterizer)";
    return result;
}

Result<std::unique_ptr<Backend>> VulkanBackend::create(const BackendCreateInfo& info) {
    std::unique_ptr<VulkanBackend> backend(new VulkanBackend(info));
    if (backend->device_ == nullptr) {
        return Status::error(ErrorCode::BackendUnavailable, "no Vulkan device");
    }
    return std::unique_ptr<Backend>(backend.release());
}

#endif  // LX_HAS_VULKAN

}  // namespace lx::backends
