// LoucuraXXXX GPU - Vulkan ICD (Installable Client Driver).
//
// What this is
// ------------
// A shared library the Vulkan loader dlopen()s when a manifest points at it. It
// makes the virtual GPU enumerable as a real VkPhysicalDevice without any
// kernel-mode driver, which is how Mesa's lavapipe and MoltenVK work.
//
// What works, for real
// --------------------
// Instance and device lifecycle, one physical device reporting
// VK_PHYSICAL_DEVICE_TYPE_CPU with one host-visible memory heap, buffers,
// images, allocation, mapping, shader modules, fences, semaphores, command pools
// and command buffers. Memory is malloc'd host memory, so a mapped pointer is
// genuinely writable and what a caller writes is what later reads see.
// vkCmdCopyBuffer, vkCmdFillBuffer and vkCmdUpdateBuffer execute at submit time
// against that memory.
//
// What does not work, and says so
// -------------------------------
// Shader execution. vkCmdDispatch and vkCmdDispatchIndirect are recorded, and
// vkQueueSubmit returns VK_ERROR_FEATURE_NOT_PRESENT without applying anything
// when one is present. A silent no-op would let an application read buffers this
// driver never wrote and produce wrong results, which is worse than a clear
// failure. There is no swapchain, no surface and no presentation: this device
// cannot display a frame.
//
// So this makes the adapter exist and do real memory work. It does not make it a
// gaming GPU, and it will not outrun the physical GPU. The Windows Device
// Manager is a separate matter: only a signed WDDM kernel driver appears there,
// and this project does not ship one.
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace {

const char* const kDeviceName = "LoucuraXXXX GPU (virtual, CPU-backed)";
constexpr uint32_t kDriverVersion = VK_MAKE_VERSION(0, 0, 2);
// 1.1 is what this driver was written against. Claiming more than is
// implemented would be a claim the code cannot keep.
constexpr uint32_t kApiVersion = VK_API_VERSION_1_1;
// One heap of 1 GiB. A virtual device that advertised the whole machine would
// let an application allocate until the OS thrashes.
constexpr VkDeviceSize kHeapBytes = VkDeviceSize(1024) * 1024 * 1024;

// Non-dispatchable handles are pointers on 64-bit and uint64_t on 32-bit, so a
// single integer-keyed table with a memcpy conversion covers both without
// depending on the handle representation.
uint64_t next_handle_value() {
    static std::mutex mutex;
    static uint64_t counter = 0x10000;
    std::lock_guard<std::mutex> lock(mutex);
    const uint64_t value = counter;
    counter += 0x10;
    return value;
}

template <typename Handle>
Handle handle_from(uint64_t value) {
    Handle handle{};
    const size_t bytes = sizeof(handle) < sizeof(value) ? sizeof(handle) : sizeof(value);
    std::memcpy(&handle, &value, bytes);
    return handle;
}

template <typename Handle>
uint64_t handle_to(Handle handle) {
    uint64_t value = 0;
    const size_t bytes = sizeof(handle) < sizeof(value) ? sizeof(handle) : sizeof(value);
    std::memcpy(&value, &handle, bytes);
    return value;
}

template <typename Object>
struct Table {
    std::map<uint64_t, Object> entries;

    template <typename Handle>
    Handle add(Object object) {
        const uint64_t key = next_handle_value();
        entries.emplace(key, std::move(object));
        return handle_from<Handle>(key);
    }
    template <typename Handle>
    Object* find(Handle handle) {
        auto it = entries.find(handle_to(handle));
        return it == entries.end() ? nullptr : &it->second;
    }
    template <typename Handle>
    void remove(Handle handle) {
        entries.erase(handle_to(handle));
    }
};

struct Allocation {
    void* host = nullptr;
    VkDeviceSize size = 0;
};

struct Buffer {
    VkDeviceSize size = 0;
    unsigned char* host = nullptr;
};

struct Image {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{0, 0, 0};
    unsigned char* host = nullptr;
    VkDeviceSize size = 0;
};

// Only memory commands are recorded, because they are the only ones this driver
// can execute.
struct RecordedCommand {
    enum class Kind { CopyBuffer, FillBuffer, UpdateBuffer };
    Kind kind = Kind::CopyBuffer;
    VkBuffer src = VK_NULL_HANDLE;
    VkBuffer dst = VK_NULL_HANDLE;
    VkDeviceSize src_offset = 0;
    VkDeviceSize dst_offset = 0;
    VkDeviceSize size = 0;
    uint32_t data = 0;
    std::vector<unsigned char> bytes;
};

struct Device;
struct CommandPool;

// Dispatchable handles are pointers, and the loader validates them on every
// entry point by reading a magic word from the start of the object. An object
// without that word is rejected as an invalid handle before this driver's code
// runs at all, so the magic has to be the first member of each of these.
struct CommandBuffer {
    VK_LOADER_DATA loader;
    CommandPool* pool = nullptr;
    bool recording = false;
    bool has_dispatch = false;
    std::vector<RecordedCommand> commands;
};

struct CommandPool {
    Device* device = nullptr;
    std::vector<CommandBuffer*> buffers;
};

struct Queue {
    VK_LOADER_DATA loader;
    Device* device = nullptr;
};

struct PhysicalDevice {
    VK_LOADER_DATA loader;
};

struct Instance {
    VK_LOADER_DATA loader;
    PhysicalDevice* physical = nullptr;
};

struct Fence {
    bool signaled = false;
};

struct Device {
    VK_LOADER_DATA loader;
    Queue* queue = nullptr;
    std::mutex mutex;
    Table<Allocation> allocations;
    Table<Buffer> buffers;
    Table<Image> images;
    Table<std::vector<unsigned char>> shaders;
    Table<Fence> fences;
    Table<char> semaphores;
    // Command pools and buffers are heap objects because their handles are the
    // object pointers the loader validates, so these registries only exist for
    // cleanup.
    std::map<uint64_t, CommandPool*> pools;
    std::map<uint64_t, CommandBuffer*> command_buffers;
    Table<char> set_layouts;
    Table<char> pipeline_layouts;
    Table<char> pipelines;
    Table<char> descriptor_pools;
    Table<char> descriptor_sets;
    VkDeviceSize allocated_bytes = 0;
};

void fill_properties(VkPhysicalDeviceProperties& props) {
    std::memset(&props, 0, sizeof(props));
    props.apiVersion = kApiVersion;
    props.driverVersion = kDriverVersion;
    props.vendorID = 0x4C58;  // 'LX'
    props.deviceID = 0x0001;
    // Stated plainly: a CPU device. Claiming DISCRETE_GPU would be the fake
    // identification this project forbids, and applications branch on the type.
    props.deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
    std::strncpy(props.deviceName, kDeviceName, sizeof(props.deviceName) - 1);
    props.limits.maxImageDimension2D = 16384;
    props.limits.maxImageDimension3D = 2048;
    props.limits.maxComputeWorkGroupCount[0] = 65535;
    props.limits.maxComputeWorkGroupCount[1] = 65535;
    props.limits.maxComputeWorkGroupCount[2] = 65535;
    props.limits.maxComputeWorkGroupSize[0] = 1024;
    props.limits.maxComputeWorkGroupSize[1] = 1024;
    props.limits.maxComputeWorkGroupSize[2] = 64;
    props.limits.maxComputeWorkGroupInvocations = 1024;
    props.limits.maxMemoryAllocationCount = 4096;
    props.limits.bufferImageGranularity = 64;
    props.limits.nonCoherentAtomSize = 64;
    props.limits.minMemoryMapAlignment = 64;
    props.limits.minStorageBufferOffsetAlignment = 16;
    props.limits.minUniformBufferOffsetAlignment = 16;
    props.limits.optimalBufferCopyOffsetAlignment = 1;
    props.limits.optimalBufferCopyRowPitchAlignment = 1;
    props.limits.maxDescriptorSetUniformBuffers = 90;
    props.limits.maxDescriptorSetStorageBuffers = 90;
    props.limits.maxDescriptorSetSampledImages = 90;
    props.limits.maxDescriptorSetStorageImages = 90;
    props.limits.maxPerStageDescriptorStorageBuffers = 90;
    props.limits.maxPerStageResources = 200;
}

void fill_memory(VkPhysicalDeviceMemoryProperties& mem) {
    std::memset(&mem, 0, sizeof(mem));
    mem.memoryTypeCount = 1;
    // Host memory is coherent and host visible by nature; device-local is set
    // too because on a CPU device there is no other kind of memory.
    mem.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    mem.memoryTypes[0].heapIndex = 0;
    mem.memoryHeapCount = 1;
    mem.memoryHeaps[0].size = kHeapBytes;
    mem.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}

// --- instance ------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo* create_info,
                                               const VkAllocationCallbacks*, VkInstance* out) {
    if (create_info == nullptr || out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t requested = create_info->pApplicationInfo != nullptr
                                   ? create_info->pApplicationInfo->apiVersion
                                   : VK_API_VERSION_1_0;
    // Refuse an API version this driver was not written against instead of
    // accepting it and misbehaving later.
    if (VK_VERSION_MAJOR(requested) > 1 ||
        (VK_VERSION_MAJOR(requested) == 1 && VK_VERSION_MINOR(requested) > 1)) {
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    auto* instance = new Instance();
    set_loader_magic_value(&instance->loader);
    instance->physical = new PhysicalDevice();
    set_loader_magic_value(&instance->physical->loader);
    *out = reinterpret_cast<VkInstance>(instance);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_instance(VkInstance instance, const VkAllocationCallbacks*) {
    if (instance == VK_NULL_HANDLE) return;
    auto* self = reinterpret_cast<Instance*>(instance);
    delete self->physical;
    delete self;
}

VKAPI_ATTR VkResult VKAPI_CALL enumerate_instance_extensions(const char*, uint32_t* count,
                                                             VkExtensionProperties*) {
    if (count != nullptr) *count = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL enumerate_instance_layers(uint32_t* count, VkLayerProperties*) {
    if (count != nullptr) *count = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL enumerate_instance_version(uint32_t* version) {
    if (version == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    *version = kApiVersion;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL enumerate_physical_devices(VkInstance instance, uint32_t* count,
                                                          VkPhysicalDevice* devices) {
    if (instance == VK_NULL_HANDLE || count == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* self = reinterpret_cast<Instance*>(instance);
    if (devices == nullptr) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count < 1) return VK_INCOMPLETE;
    devices[0] = reinterpret_cast<VkPhysicalDevice>(self->physical);
    *count = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL get_physical_device_properties(VkPhysicalDevice,
                                                          VkPhysicalDeviceProperties* props) {
    if (props != nullptr) fill_properties(*props);
}

VKAPI_ATTR void VKAPI_CALL get_physical_device_properties2(VkPhysicalDevice device,
                                                           VkPhysicalDeviceProperties2* props) {
    if (props != nullptr) get_physical_device_properties(device, &props->properties);
}

VKAPI_ATTR void VKAPI_CALL get_physical_device_features(VkPhysicalDevice,
                                                        VkPhysicalDeviceFeatures* features) {
    // Every feature off. A feature reported here is a promise the driver must
    // keep, and none of them are implemented.
    if (features != nullptr) std::memset(features, 0, sizeof(*features));
}

VKAPI_ATTR void VKAPI_CALL get_physical_device_features2(VkPhysicalDevice device,
                                                         VkPhysicalDeviceFeatures2* features) {
    if (features != nullptr) get_physical_device_features(device, &features->features);
}

VKAPI_ATTR void VKAPI_CALL get_physical_device_memory_properties(
    VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* mem) {
    if (mem != nullptr) fill_memory(*mem);
}

VKAPI_ATTR void VKAPI_CALL get_physical_device_memory_properties2(
    VkPhysicalDevice device, VkPhysicalDeviceMemoryProperties2* mem) {
    if (mem != nullptr) get_physical_device_memory_properties(device, &mem->memoryProperties);
}

VKAPI_ATTR void VKAPI_CALL get_queue_family_properties(VkPhysicalDevice, uint32_t* count,
                                                       VkQueueFamilyProperties* props) {
    if (count == nullptr) return;
    if (props == nullptr) {
        *count = 1;
        return;
    }
    if (*count < 1) return;
    std::memset(&props[0], 0, sizeof(props[0]));
    // Compute only. Graphics and transfer are not advertised because they are
    // not implemented; advertising them would invite work that must be refused.
    props[0].queueFlags = VK_QUEUE_COMPUTE_BIT;
    props[0].queueCount = 1;
    props[0].timestampValidBits = 0;
    props[0].minImageTransferGranularity = {1, 1, 1};
    *count = 1;
}

VKAPI_ATTR void VKAPI_CALL get_queue_family_properties2(VkPhysicalDevice device, uint32_t* count,
                                                        VkQueueFamilyProperties2* props) {
    if (props == nullptr) {
        get_queue_family_properties(device, count, nullptr);
        return;
    }
    if (*count < 1) {
        *count = 0;
        return;
    }
    VkQueueFamilyProperties plain{};
    uint32_t available = 1;
    get_queue_family_properties(device, &available, &plain);
    props[0].queueFamilyProperties = plain;
    *count = 1;
}

VKAPI_ATTR void VKAPI_CALL get_format_properties(VkPhysicalDevice, VkFormat,
                                                 VkFormatProperties* props) {
    // No format claims any capability, because no command consumes one yet.
    if (props != nullptr) std::memset(props, 0, sizeof(*props));
}

VKAPI_ATTR VkResult VKAPI_CALL get_image_format_properties(VkPhysicalDevice, VkFormat, VkImageType,
                                                           VkImageTiling, VkImageUsageFlags,
                                                           VkImageCreateFlags,
                                                           VkImageFormatProperties*) {
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
}

VKAPI_ATTR void VKAPI_CALL get_sparse_image_format_properties(VkPhysicalDevice, VkFormat,
                                                              VkImageType, VkSampleCountFlagBits,
                                                              VkImageUsageFlags, VkImageTiling,
                                                              uint32_t* count,
                                                              VkSparseImageFormatProperties*) {
    if (count != nullptr) *count = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL enumerate_device_extensions(VkPhysicalDevice, const char*,
                                                           uint32_t* count,
                                                           VkExtensionProperties*) {
    if (count != nullptr) *count = 0;
    return VK_SUCCESS;
}

// --- device --------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice, const VkDeviceCreateInfo*,
                                             const VkAllocationCallbacks*, VkDevice* out) {
    if (out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* device = new Device();
    set_loader_magic_value(&device->loader);
    device->queue = new Queue();
    set_loader_magic_value(&device->queue->loader);
    device->queue->device = device;
    *out = reinterpret_cast<VkDevice>(device);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice device, const VkAllocationCallbacks*) {
    if (device == VK_NULL_HANDLE) return;
    auto* self = reinterpret_cast<Device*>(device);
    for (auto& entry : self->allocations.entries) std::free(entry.second.host);
    for (auto& entry : self->command_buffers) delete entry.second;
    for (auto& entry : self->pools) delete entry.second;
    delete self->queue;
    delete self;
}

VKAPI_ATTR void VKAPI_CALL get_device_queue(VkDevice device, uint32_t, uint32_t, VkQueue* queue) {
    if (queue == nullptr) return;
    auto* self = reinterpret_cast<Device*>(device);
    *queue = reinterpret_cast<VkQueue>(self->queue);
}

VKAPI_ATTR VkResult VKAPI_CALL device_wait_idle(VkDevice) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL queue_wait_idle(VkQueue) { return VK_SUCCESS; }

VKAPI_ATTR VkResult VKAPI_CALL allocate_memory(VkDevice device, const VkMemoryAllocateInfo* info,
                                               const VkAllocationCallbacks*,
                                               VkDeviceMemory* out) {
    if (device == VK_NULL_HANDLE || info == nullptr || out == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (info->memoryTypeIndex >= 1 || info->allocationSize == 0) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    if (self->allocated_bytes + info->allocationSize > kHeapBytes) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    void* host = std::calloc(1, static_cast<size_t>(info->allocationSize));
    if (host == nullptr) return VK_ERROR_OUT_OF_HOST_MEMORY;
    Allocation allocation;
    allocation.host = host;
    allocation.size = info->allocationSize;
    self->allocated_bytes += info->allocationSize;
    *out = self->allocations.add<VkDeviceMemory>(std::move(allocation));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice device, VkDeviceMemory memory,
                                       const VkAllocationCallbacks*) {
    if (device == VK_NULL_HANDLE || memory == VK_NULL_HANDLE) return;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    if (Allocation* allocation = self->allocations.find(memory)) {
        std::free(allocation->host);
        self->allocated_bytes -= allocation->size;
        self->allocations.remove(memory);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL map_memory(VkDevice device, VkDeviceMemory memory,
                                          VkDeviceSize offset, VkDeviceSize size,
                                          VkMemoryMapFlags, void** data) {
    if (data == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    Allocation* allocation = self->allocations.find(memory);
    if (allocation == nullptr) return VK_ERROR_MEMORY_MAP_FAILED;
    if (offset > allocation->size) return VK_ERROR_MEMORY_MAP_FAILED;
    if (size != VK_WHOLE_SIZE && size > allocation->size - offset) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    // The pointer is the allocation itself: host memory, writable. That is what
    // makes this device's memory real rather than simulated.
    *data = static_cast<unsigned char*>(allocation->host) + offset;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL unmap_memory(VkDevice, VkDeviceMemory) {
    // Nothing to invalidate: the mapping is plain host memory.
}

VKAPI_ATTR VkResult VKAPI_CALL create_buffer(VkDevice device, const VkBufferCreateInfo* info,
                                             const VkAllocationCallbacks*, VkBuffer* out) {
    if (device == VK_NULL_HANDLE || info == nullptr || out == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto* self = reinterpret_cast<Device*>(device);
    Buffer buffer;
    buffer.size = info->size;
    std::lock_guard<std::mutex> lock(self->mutex);
    *out = self->buffers.add<VkBuffer>(std::move(buffer));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice device, VkBuffer buffer,
                                          const VkAllocationCallbacks*) {
    if (device == VK_NULL_HANDLE || buffer == VK_NULL_HANDLE) return;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    self->buffers.remove(buffer);
}

VKAPI_ATTR void VKAPI_CALL get_buffer_memory_requirements(VkDevice device, VkBuffer buffer,
                                                          VkMemoryRequirements* req) {
    if (req == nullptr) return;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    Buffer* entry = self->buffers.find(buffer);
    req->size = entry != nullptr ? entry->size : 0;
    req->alignment = 64;
    req->memoryTypeBits = 1;
}

VKAPI_ATTR VkResult VKAPI_CALL bind_buffer_memory(VkDevice device, VkBuffer buffer,
                                                  VkDeviceMemory memory, VkDeviceSize offset) {
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    Buffer* entry = self->buffers.find(buffer);
    Allocation* allocation = self->allocations.find(memory);
    if (entry == nullptr || allocation == nullptr) return VK_ERROR_UNKNOWN;
    if (offset > allocation->size || entry->size > allocation->size - offset) {
        return VK_ERROR_UNKNOWN;
    }
    entry->host = static_cast<unsigned char*>(allocation->host) + offset;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL create_image(VkDevice device, const VkImageCreateInfo* info,
                                            const VkAllocationCallbacks*, VkImage* out) {
    if (info == nullptr || out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* self = reinterpret_cast<Device*>(device);
    Image image;
    image.format = info->format;
    image.extent = info->extent;
    // 4 bytes per pixel as an upper bound: the exact per-format size table is
    // not implemented, and over-reserving is safe while under-reserving is not.
    image.size = VkDeviceSize(info->extent.width) * info->extent.height * info->extent.depth * 4;
    std::lock_guard<std::mutex> lock(self->mutex);
    *out = self->images.add<VkImage>(std::move(image));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_image(VkDevice device, VkImage image,
                                         const VkAllocationCallbacks*) {
    if (device == VK_NULL_HANDLE || image == VK_NULL_HANDLE) return;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    self->images.remove(image);
}

VKAPI_ATTR void VKAPI_CALL get_image_memory_requirements(VkDevice device, VkImage image,
                                                         VkMemoryRequirements* req) {
    if (req == nullptr) return;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    Image* entry = self->images.find(image);
    req->size = entry != nullptr ? entry->size : 0;
    req->alignment = 64;
    req->memoryTypeBits = 1;
}

VKAPI_ATTR VkResult VKAPI_CALL bind_image_memory(VkDevice device, VkImage image,
                                                 VkDeviceMemory memory, VkDeviceSize offset) {
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    Image* entry = self->images.find(image);
    Allocation* allocation = self->allocations.find(memory);
    if (entry == nullptr || allocation == nullptr) return VK_ERROR_UNKNOWN;
    if (offset > allocation->size || entry->size > allocation->size - offset) {
        return VK_ERROR_UNKNOWN;
    }
    entry->host = static_cast<unsigned char*>(allocation->host) + offset;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL create_shader_module(VkDevice device,
                                                    const VkShaderModuleCreateInfo* info,
                                                    const VkAllocationCallbacks*,
                                                    VkShaderModule* out) {
    if (info == nullptr || out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (info->codeSize == 0 || info->codeSize % 4 != 0) return VK_ERROR_INVALID_SHADER_NV;
    auto* self = reinterpret_cast<Device*>(device);
    const auto* bytes = reinterpret_cast<const unsigned char*>(info->pCode);
    std::vector<unsigned char> code(bytes, bytes + info->codeSize);
    std::lock_guard<std::mutex> lock(self->mutex);
    *out = self->shaders.add<VkShaderModule>(std::move(code));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_shader_module(VkDevice device, VkShaderModule module,
                                                 const VkAllocationCallbacks*) {
    if (device == VK_NULL_HANDLE || module == VK_NULL_HANDLE) return;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    self->shaders.remove(module);
}

VKAPI_ATTR VkResult VKAPI_CALL create_fence(VkDevice device, const VkFenceCreateInfo* info,
                                            const VkAllocationCallbacks*, VkFence* out) {
    if (out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* self = reinterpret_cast<Device*>(device);
    Fence fence;
    fence.signaled = info != nullptr && (info->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
    std::lock_guard<std::mutex> lock(self->mutex);
    *out = self->fences.add<VkFence>(std::move(fence));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_fence(VkDevice device, VkFence fence,
                                         const VkAllocationCallbacks*) {
    if (device == VK_NULL_HANDLE || fence == VK_NULL_HANDLE) return;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    self->fences.remove(fence);
}

VKAPI_ATTR VkResult VKAPI_CALL get_fence_status(VkDevice device, VkFence fence) {
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    Fence* entry = self->fences.find(fence);
    if (entry == nullptr) return VK_ERROR_UNKNOWN;
    return entry->signaled ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL reset_fences(VkDevice device, uint32_t count,
                                            const VkFence* fences) {
    if (fences == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    for (uint32_t i = 0; i < count; ++i) {
        if (Fence* entry = self->fences.find(fences[i])) entry->signaled = false;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL wait_for_fences(VkDevice device, uint32_t count,
                                               const VkFence* fences, VkBool32, uint64_t) {
    // Nothing is ever in flight: submission either completes synchronously or is
    // refused, so this only has to report fence state truthfully.
    if (fences == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    for (uint32_t i = 0; i < count; ++i) {
        Fence* entry = self->fences.find(fences[i]);
        if (entry == nullptr || !entry->signaled) return VK_TIMEOUT;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL create_semaphore(VkDevice device, const VkSemaphoreCreateInfo*,
                                                const VkAllocationCallbacks*,
                                                VkSemaphore* out) {
    if (out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    *out = self->semaphores.add<VkSemaphore>('\0');
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_semaphore(VkDevice device, VkSemaphore semaphore,
                                             const VkAllocationCallbacks*) {
    if (device == VK_NULL_HANDLE || semaphore == VK_NULL_HANDLE) return;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    self->semaphores.remove(semaphore);
}

VKAPI_ATTR VkResult VKAPI_CALL create_command_pool(VkDevice device,
                                                   const VkCommandPoolCreateInfo*,
                                                   const VkAllocationCallbacks*,
                                                   VkCommandPool* out) {
    if (out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* self = reinterpret_cast<Device*>(device);
    auto* pool = new CommandPool();
    pool->device = self;
    std::lock_guard<std::mutex> lock(self->mutex);
    self->pools.emplace(next_handle_value(), pool);
    *out = reinterpret_cast<VkCommandPool>(pool);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_command_pool(VkDevice device, VkCommandPool pool,
                                                const VkAllocationCallbacks*) {
    if (device == VK_NULL_HANDLE || pool == VK_NULL_HANDLE) return;
    auto* self = reinterpret_cast<Device*>(device);
    auto* target = reinterpret_cast<CommandPool*>(pool);
    std::lock_guard<std::mutex> lock(self->mutex);
    // Only this pool's command buffers are destroyed, which is what the spec
    // requires; buffers from other pools stay alive.
    for (CommandBuffer* buffer : target->buffers) {
        for (auto it = self->command_buffers.begin();
             it != self->command_buffers.end(); ++it) {
            if (it->second == buffer) {
                self->command_buffers.erase(it);
                break;
            }
        }
        delete buffer;
    }
    for (auto it = self->pools.begin(); it != self->pools.end(); ++it) {
        if (it->second == target) {
            self->pools.erase(it);
            break;
        }
    }
    delete target;
}

VKAPI_ATTR VkResult VKAPI_CALL allocate_command_buffers(VkDevice device,
                                                        const VkCommandBufferAllocateInfo* info,
                                                        VkCommandBuffer* out) {
    if (info == nullptr || out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    auto* pool = reinterpret_cast<CommandPool*>(info->commandPool);
    if (pool == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
        auto* buffer = new CommandBuffer();
        set_loader_magic_value(&buffer->loader);
        buffer->pool = pool;
        pool->buffers.push_back(buffer);
        self->command_buffers.emplace(next_handle_value(), buffer);
        out[i] = reinterpret_cast<VkCommandBuffer>(buffer);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL free_command_buffers(VkDevice device, VkCommandPool, uint32_t count,
                                                const VkCommandBuffer* buffers) {
    if (device == VK_NULL_HANDLE || buffers == nullptr) return;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    for (uint32_t i = 0; i < count; ++i) {
        auto* target = reinterpret_cast<CommandBuffer*>(buffers[i]);
        for (auto it = self->command_buffers.begin();
             it != self->command_buffers.end(); ++it) {
            if (it->second == target) {
                self->command_buffers.erase(it);
                break;
            }
        }
        if (target->pool != nullptr) {
            auto& owned = target->pool->buffers;
            for (auto it = owned.begin(); it != owned.end(); ++it) {
                if (*it == target) {
                    owned.erase(it);
                    break;
                }
            }
        }
        delete target;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL begin_command_buffer(VkCommandBuffer buffer,
                                                    const VkCommandBufferBeginInfo*) {
    auto* self = reinterpret_cast<CommandBuffer*>(buffer);
    if (self == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    self->recording = true;
    self->has_dispatch = false;
    self->commands.clear();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL end_command_buffer(VkCommandBuffer buffer) {
    auto* self = reinterpret_cast<CommandBuffer*>(buffer);
    if (self == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    self->recording = false;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL reset_command_buffer(VkCommandBuffer buffer,
                                                    VkCommandBufferResetFlags) {
    auto* self = reinterpret_cast<CommandBuffer*>(buffer);
    if (self == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    self->has_dispatch = false;
    self->commands.clear();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL cmd_dispatch(VkCommandBuffer buffer, uint32_t, uint32_t, uint32_t) {
    // Recorded and refused at submit, so the error surfaces where the caller
    // checks it instead of vanishing here.
    auto* self = reinterpret_cast<CommandBuffer*>(buffer);
    if (self != nullptr) self->has_dispatch = true;
}

VKAPI_ATTR void VKAPI_CALL cmd_dispatch_indirect(VkCommandBuffer buffer, VkBuffer, VkDeviceSize) {
    auto* self = reinterpret_cast<CommandBuffer*>(buffer);
    if (self != nullptr) self->has_dispatch = true;
}

VKAPI_ATTR void VKAPI_CALL cmd_pipeline_barrier(VkCommandBuffer, VkPipelineStageFlags,
                                                VkPipelineStageFlags, VkDependencyFlags,
                                                uint32_t, const VkMemoryBarrier*, uint32_t,
                                                const VkBufferMemoryBarrier*, uint32_t,
                                                const VkImageMemoryBarrier*) {
    // Nothing runs concurrently, so a barrier has nothing to order. Accepted and
    // ignored rather than pretended to be meaningful.
}

VKAPI_ATTR void VKAPI_CALL cmd_fill_buffer(VkCommandBuffer buffer, VkBuffer dst,
                                           VkDeviceSize offset, VkDeviceSize size, uint32_t data) {
    auto* self = reinterpret_cast<CommandBuffer*>(buffer);
    if (self == nullptr) return;
    RecordedCommand command;
    command.kind = RecordedCommand::Kind::FillBuffer;
    command.dst = dst;
    command.dst_offset = offset;
    command.size = size;
    command.data = data;
    self->commands.push_back(std::move(command));
}

VKAPI_ATTR void VKAPI_CALL cmd_copy_buffer(VkCommandBuffer buffer, VkBuffer src, VkBuffer dst,
                                           uint32_t region_count, const VkBufferCopy* regions) {
    auto* self = reinterpret_cast<CommandBuffer*>(buffer);
    if (self == nullptr || regions == nullptr) return;
    for (uint32_t i = 0; i < region_count; ++i) {
        RecordedCommand command;
        command.kind = RecordedCommand::Kind::CopyBuffer;
        command.src = src;
        command.dst = dst;
        command.src_offset = regions[i].srcOffset;
        command.dst_offset = regions[i].dstOffset;
        command.size = regions[i].size;
        self->commands.push_back(std::move(command));
    }
}

VKAPI_ATTR void VKAPI_CALL cmd_update_buffer(VkCommandBuffer buffer, VkBuffer dst,
                                             VkDeviceSize offset, VkDeviceSize size,
                                             const void* data) {
    auto* self = reinterpret_cast<CommandBuffer*>(buffer);
    if (self == nullptr || data == nullptr) return;
    RecordedCommand command;
    command.kind = RecordedCommand::Kind::UpdateBuffer;
    command.dst = dst;
    command.dst_offset = offset;
    command.size = size;
    const auto* bytes = static_cast<const unsigned char*>(data);
    command.bytes.assign(bytes, bytes + size);
    self->commands.push_back(std::move(command));
}

// Runs one recorded command against real memory. Returns false when it cannot be
// executed, so submission refuses instead of leaving half the work applied.
bool execute(Device* device, const RecordedCommand& command) {
    Buffer* dst = device->buffers.find(command.dst);
    if (dst == nullptr || dst->host == nullptr) return false;
    if (command.dst_offset > dst->size || command.size > dst->size - command.dst_offset) {
        return false;
    }
    unsigned char* out = dst->host + command.dst_offset;
    switch (command.kind) {
        case RecordedCommand::Kind::FillBuffer: {
            // vkCmdFillBuffer takes a 32-bit value and fills whole words; a
            // trailing partial word keeps its previous contents per the spec.
            const VkDeviceSize words = command.size / 4;
            auto* target = reinterpret_cast<uint32_t*>(out);
            for (VkDeviceSize i = 0; i < words; ++i) target[i] = command.data;
            return true;
        }
        case RecordedCommand::Kind::UpdateBuffer: {
            if (command.bytes.size() != command.size) return false;
            std::memcpy(out, command.bytes.data(), static_cast<size_t>(command.size));
            return true;
        }
        case RecordedCommand::Kind::CopyBuffer: {
            Buffer* src = device->buffers.find(command.src);
            if (src == nullptr || src->host == nullptr) return false;
            if (command.src_offset > src->size || command.size > src->size - command.src_offset) {
                return false;
            }
            // memmove, not memcpy: overlapping regions are legal in a copy.
            std::memmove(out, src->host + command.src_offset, static_cast<size_t>(command.size));
            return true;
        }
    }
    return false;
}

VKAPI_ATTR VkResult VKAPI_CALL queue_submit(VkQueue queue, uint32_t submit_count,
                                            const VkSubmitInfo* submits, VkFence fence) {
    auto* queue_object = reinterpret_cast<Queue*>(queue);
    if (queue_object == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    Device* device = queue_object->device;
    if (submit_count > 0 && submits == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    std::lock_guard<std::mutex> lock(device->mutex);
    // Check every buffer before running anything: a refusal must not leave some
    // command buffers applied and others not.
    for (uint32_t i = 0; i < submit_count; ++i) {
        for (uint32_t j = 0; j < submits[i].commandBufferCount; ++j) {
            auto* buffer = reinterpret_cast<CommandBuffer*>(submits[i].pCommandBuffers[j]);
            if (buffer != nullptr && buffer->has_dispatch) {
                // Explicit: a caller that got VK_SUCCESS here would read buffers
                // this driver never wrote.
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
        }
    }
    for (uint32_t i = 0; i < submit_count; ++i) {
        for (uint32_t j = 0; j < submits[i].commandBufferCount; ++j) {
            auto* buffer = reinterpret_cast<CommandBuffer*>(submits[i].pCommandBuffers[j]);
            if (buffer == nullptr) continue;
            for (const RecordedCommand& command : buffer->commands) {
                if (!execute(device, command)) return VK_ERROR_UNKNOWN;
            }
        }
    }
    if (fence != VK_NULL_HANDLE) {
        if (Fence* entry = device->fences.find(fence)) entry->signaled = true;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL create_descriptor_set_layout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*,
    VkDescriptorSetLayout* out) {
    if (out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    *out = self->set_layouts.add<VkDescriptorSetLayout>('\0');
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_descriptor_set_layout(VkDevice device,
                                                         VkDescriptorSetLayout layout,
                                                         const VkAllocationCallbacks*) {
    if (device == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) return;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    self->set_layouts.remove(layout);
}

VKAPI_ATTR VkResult VKAPI_CALL create_pipeline_layout(VkDevice device,
                                                      const VkPipelineLayoutCreateInfo*,
                                                      const VkAllocationCallbacks*,
                                                      VkPipelineLayout* out) {
    if (out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    *out = self->pipeline_layouts.add<VkPipelineLayout>('\0');
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_pipeline_layout(VkDevice device, VkPipelineLayout layout,
                                                   const VkAllocationCallbacks*) {
    if (device == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) return;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    self->pipeline_layouts.remove(layout);
}

VKAPI_ATTR VkResult VKAPI_CALL create_compute_pipelines(VkDevice device, VkPipelineCache,
                                                        uint32_t count,
                                                        const VkComputePipelineCreateInfo*,
                                                        const VkAllocationCallbacks*,
                                                        VkPipeline* out) {
    if (out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    for (uint32_t i = 0; i < count; ++i) out[i] = self->pipelines.add<VkPipeline>('\0');
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_pipeline(VkDevice device, VkPipeline pipeline,
                                            const VkAllocationCallbacks*) {
    if (device == VK_NULL_HANDLE || pipeline == VK_NULL_HANDLE) return;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    self->pipelines.remove(pipeline);
}

VKAPI_ATTR VkResult VKAPI_CALL create_descriptor_pool(VkDevice device,
                                                      const VkDescriptorPoolCreateInfo*,
                                                      const VkAllocationCallbacks*,
                                                      VkDescriptorPool* out) {
    if (out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    *out = self->descriptor_pools.add<VkDescriptorPool>('\0');
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL destroy_descriptor_pool(VkDevice device, VkDescriptorPool pool,
                                                   const VkAllocationCallbacks*) {
    if (device == VK_NULL_HANDLE || pool == VK_NULL_HANDLE) return;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    self->descriptor_pools.remove(pool);
}

VKAPI_ATTR VkResult VKAPI_CALL allocate_descriptor_sets(VkDevice device,
                                                        const VkDescriptorSetAllocateInfo* info,
                                                        VkDescriptorSet* out) {
    if (info == nullptr || out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto* self = reinterpret_cast<Device*>(device);
    std::lock_guard<std::mutex> lock(self->mutex);
    if (self->descriptor_pools.find(info->descriptorPool) == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
        out[i] = self->descriptor_sets.add<VkDescriptorSet>('\0');
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL update_descriptor_sets(VkDevice, uint32_t, const VkWriteDescriptorSet*,
                                                  uint32_t, const VkCopyDescriptorSet*) {
    // Stored nowhere, because no shader reads descriptors: there is no shader
    // execution. Accepted so object setup runs to the point where submission
    // reports the real limitation.
}

// --- dispatch ------------------------------------------------------------

template <typename Fn>
PFN_vkVoidFunction cast(Fn fn) {
    return reinterpret_cast<PFN_vkVoidFunction>(fn);
}

struct Entry {
    const char* name;
    PFN_vkVoidFunction function;
};

// Only entry points that exist above. Anything else returns nullptr, which the
// loader reports as unsupported instead of handing out a stub that looks alive.
const Entry kInstanceEntries[] = {
    {"vkCreateInstance", cast(create_instance)},
    {"vkDestroyInstance", cast(destroy_instance)},
    {"vkEnumerateInstanceExtensionProperties", cast(enumerate_instance_extensions)},
    {"vkEnumerateInstanceLayerProperties", cast(enumerate_instance_layers)},
    {"vkEnumerateInstanceVersion", cast(enumerate_instance_version)},
    {"vkEnumeratePhysicalDevices", cast(enumerate_physical_devices)},
    {"vkGetPhysicalDeviceProperties", cast(get_physical_device_properties)},
    {"vkGetPhysicalDeviceProperties2", cast(get_physical_device_properties2)},
    {"vkGetPhysicalDeviceFeatures", cast(get_physical_device_features)},
    {"vkGetPhysicalDeviceFeatures2", cast(get_physical_device_features2)},
    {"vkGetPhysicalDeviceMemoryProperties", cast(get_physical_device_memory_properties)},
    {"vkGetPhysicalDeviceMemoryProperties2", cast(get_physical_device_memory_properties2)},
    {"vkGetPhysicalDeviceQueueFamilyProperties", cast(get_queue_family_properties)},
    {"vkGetPhysicalDeviceQueueFamilyProperties2", cast(get_queue_family_properties2)},
    {"vkGetPhysicalDeviceFormatProperties", cast(get_format_properties)},
    {"vkGetPhysicalDeviceImageFormatProperties", cast(get_image_format_properties)},
    {"vkGetPhysicalDeviceSparseImageFormatProperties", cast(get_sparse_image_format_properties)},
    {"vkEnumerateDeviceExtensionProperties", cast(enumerate_device_extensions)},
    {"vkCreateDevice", cast(create_device)},
};

const Entry kDeviceEntries[] = {
    {"vkDestroyDevice", cast(destroy_device)},
    {"vkGetDeviceQueue", cast(get_device_queue)},
    {"vkDeviceWaitIdle", cast(device_wait_idle)},
    {"vkQueueWaitIdle", cast(queue_wait_idle)},
    {"vkAllocateMemory", cast(allocate_memory)},
    {"vkFreeMemory", cast(free_memory)},
    {"vkMapMemory", cast(map_memory)},
    {"vkUnmapMemory", cast(unmap_memory)},
    {"vkCreateBuffer", cast(create_buffer)},
    {"vkDestroyBuffer", cast(destroy_buffer)},
    {"vkGetBufferMemoryRequirements", cast(get_buffer_memory_requirements)},
    {"vkBindBufferMemory", cast(bind_buffer_memory)},
    {"vkCreateImage", cast(create_image)},
    {"vkDestroyImage", cast(destroy_image)},
    {"vkGetImageMemoryRequirements", cast(get_image_memory_requirements)},
    {"vkBindImageMemory", cast(bind_image_memory)},
    {"vkCreateShaderModule", cast(create_shader_module)},
    {"vkDestroyShaderModule", cast(destroy_shader_module)},
    {"vkCreateFence", cast(create_fence)},
    {"vkDestroyFence", cast(destroy_fence)},
    {"vkGetFenceStatus", cast(get_fence_status)},
    {"vkResetFences", cast(reset_fences)},
    {"vkWaitForFences", cast(wait_for_fences)},
    {"vkCreateSemaphore", cast(create_semaphore)},
    {"vkDestroySemaphore", cast(destroy_semaphore)},
    {"vkCreateCommandPool", cast(create_command_pool)},
    {"vkDestroyCommandPool", cast(destroy_command_pool)},
    {"vkAllocateCommandBuffers", cast(allocate_command_buffers)},
    {"vkFreeCommandBuffers", cast(free_command_buffers)},
    {"vkBeginCommandBuffer", cast(begin_command_buffer)},
    {"vkEndCommandBuffer", cast(end_command_buffer)},
    {"vkResetCommandBuffer", cast(reset_command_buffer)},
    {"vkCmdDispatch", cast(cmd_dispatch)},
    {"vkCmdDispatchIndirect", cast(cmd_dispatch_indirect)},
    {"vkCmdPipelineBarrier", cast(cmd_pipeline_barrier)},
    {"vkCmdFillBuffer", cast(cmd_fill_buffer)},
    {"vkCmdCopyBuffer", cast(cmd_copy_buffer)},
    {"vkCmdUpdateBuffer", cast(cmd_update_buffer)},
    {"vkQueueSubmit", cast(queue_submit)},
    {"vkCreateDescriptorSetLayout", cast(create_descriptor_set_layout)},
    {"vkDestroyDescriptorSetLayout", cast(destroy_descriptor_set_layout)},
    {"vkCreatePipelineLayout", cast(create_pipeline_layout)},
    {"vkDestroyPipelineLayout", cast(destroy_pipeline_layout)},
    {"vkCreateComputePipelines", cast(create_compute_pipelines)},
    {"vkDestroyPipeline", cast(destroy_pipeline)},
    {"vkCreateDescriptorPool", cast(create_descriptor_pool)},
    {"vkDestroyDescriptorPool", cast(destroy_descriptor_pool)},
    {"vkAllocateDescriptorSets", cast(allocate_descriptor_sets)},
    {"vkUpdateDescriptorSets", cast(update_descriptor_sets)},
};

template <size_t N>
PFN_vkVoidFunction find_entry(const Entry (&table)[N], const char* name) {
    if (name == nullptr) return nullptr;
    for (size_t i = 0; i < N; ++i) {
        if (std::strcmp(table[i].name, name) == 0) return table[i].function;
    }
    return nullptr;
}

}  // namespace

// --- exported ICD interface ---------------------------------------------
//
// These symbols are the entire contract between the loader and this library. C
// linkage and default visibility are required.

extern "C" {

// Declared before use: vk_icdGetInstanceProcAddr has to be able to hand back the
// proc-addr functions themselves, which are defined further down.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetDeviceProcAddr(VkDevice device,
                                                                 const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance,
                                                                         const char* name);
VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* version);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                   const char* name) {
    // The loader asks for the proc-addr functions themselves, both with a null
    // instance and with a live one. Answering nullptr for those makes it skip
    // the driver entirely, which is what happened before this check existed.
    if (name != nullptr) {
        if (std::strcmp(name, "vkGetInstanceProcAddr") == 0 ||
            std::strcmp(name, "vk_icdGetInstanceProcAddr") == 0) {
            return cast(vk_icdGetInstanceProcAddr);
        }
        if (std::strcmp(name, "vkGetDeviceProcAddr") == 0 ||
            std::strcmp(name, "vk_icdGetDeviceProcAddr") == 0) {
            return cast(vk_icdGetDeviceProcAddr);
        }
        if (std::strcmp(name, "vk_icdGetPhysicalDeviceProcAddr") == 0) {
            return cast(vk_icdGetPhysicalDeviceProcAddr);
        }
        if (std::strcmp(name, "vk_icdNegotiateLoaderICDInterfaceVersion") == 0) {
            return cast(vk_icdNegotiateLoaderICDInterfaceVersion);
        }
    }
    // Global entry points are answered without an instance; device entry points
    // only once there is one.
    if (instance == VK_NULL_HANDLE) return find_entry(kInstanceEntries, name);
    if (PFN_vkVoidFunction fn = find_entry(kDeviceEntries, name)) return fn;
    return find_entry(kInstanceEntries, name);
}

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* version) {
    if (version == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    // 5 is the current loader interface. Clamping a newer loader down keeps old
    // and new loaders working; below 3 there is no usable entry point.
    if (*version > 5) *version = 5;
    if (*version < 3) return VK_ERROR_INCOMPATIBLE_DRIVER;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance,
                                                                         const char* name) {
    return find_entry(kInstanceEntries, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetDeviceProcAddr(VkDevice, const char* name) {
    return find_entry(kDeviceEntries, name);
}

// The loader also probes the unprefixed names when it loads an ICD directly.
// Exporting them costs nothing and avoids a driver being skipped for a missing
// alias, which is what happened before they existed here.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char* name) {
    return vk_icdGetInstanceProcAddr(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
    return vk_icdGetDeviceProcAddr(device, name);
}

}  // extern "C"
