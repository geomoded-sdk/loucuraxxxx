// Tests for the Vulkan ICD: the manifest logic (pure, runs anywhere) and, when
// the ICD library was built, a real loader round trip that proves the device is
// enumerable and that its memory works.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "backends/vulkan/icd/icd_manifest.hpp"
#include "core/files.hpp"
#include "core/json.hpp"
#include "test_framework.hpp"

#if defined(LX_TEST_HAVE_ICD)
#include <vulkan/vulkan.h>
#endif

using namespace lx;

namespace {

std::string temp_dir(const std::string& name) {
    const char* base = std::getenv("TMPDIR");
    const std::string root = base != nullptr ? base : "/tmp";
    return root + "/lx_icd_test_" + name;
}

backends::icd::AdapterIdentity identity() { return backends::icd::AdapterIdentity(); }

}  // namespace

LX_TEST(icd, manifest_declares_file_format_and_icd_object) {
    const std::string text =
        backends::icd::manifest_json(identity(), "/opt/lx/liblxc_icd.so", "1.1.0");
    auto parsed = Json::parse(text);
    LX_CHECK_OK(parsed);
    if (!parsed.ok()) return;
    LX_CHECK_EQ(parsed.value().at("file_format_version").as_string(), "1.0.0");
    LX_CHECK(parsed.value().has("ICD"));
    LX_CHECK_EQ(parsed.value().at("ICD").at("library_path").as_string(),
               "/opt/lx/liblxc_icd.so");
    LX_CHECK_EQ(parsed.value().at("ICD").at("api_version").as_string(), "1.1.0");
}

LX_TEST(icd, manifest_records_the_api_version_it_is_given) {
    // The claim has to come from the caller, so it cannot silently drift away
    // from what the ICD actually implements.
    const std::string text =
        backends::icd::manifest_json(identity(), "/x/liblxc_icd.so", "1.0.0");
    auto parsed = Json::parse(text);
    LX_CHECK_OK(parsed);
    if (parsed.ok()) {
        LX_CHECK_EQ(parsed.value().at("ICD").at("api_version").as_string(), "1.0.0");
    }
}

LX_TEST(icd, install_rejects_relative_library_path) {
    // The loader ignores relative paths, so installing one would create a
    // manifest that can never work.
    const std::string dir = temp_dir("relative");
    auto result = backends::icd::install_manifest(identity(), "liblxc_icd.so", dir, nullptr);
    LX_CHECK(!result.ok());
    LX_CHECK_EQ(static_cast<int>(result.code()),
                static_cast<int>(ErrorCode::InvalidArgument));
}

LX_TEST(icd, install_rejects_missing_library) {
    const std::string dir = temp_dir("missing");
    auto result = backends::icd::install_manifest(identity(), "/nonexistent/liblxc_icd.so",
                                                 dir, nullptr);
    LX_CHECK(!result.ok());
    LX_CHECK_EQ(static_cast<int>(result.code()), static_cast<int>(ErrorCode::IoError));
}

LX_TEST(icd, install_writes_manifest_then_removes_it) {
    const std::string dir = temp_dir("roundtrip");
    // The library file only has to exist for the check; its contents do not
    // matter to the manifest writer.
    const std::string library = dir + "/liblxc_icd.so";
    LX_CHECK_OK(make_directories(dir));
    LX_CHECK_OK(write_file(library, "not a real library"));

    backends::icd::InstallResult result;
    auto installed = backends::icd::install_manifest(identity(), library, dir, &result);
    LX_CHECK_OK(installed);
    LX_CHECK(result.installed);
    LX_CHECK(file_exists(result.manifest_path));
    LX_CHECK(backends::icd::is_our_manifest(result.manifest_path));

    // The notes have to say this is a CPU device, because a user reading the
    // install output should not conclude they gained hardware.
    bool mentions_cpu = false;
    for (const auto& note : result.notes) {
        if (note.find("CPU") != std::string::npos) mentions_cpu = true;
    }
    LX_CHECK(mentions_cpu);

    LX_CHECK_OK(backends::icd::remove_manifest(result.manifest_path));
    LX_CHECK(!file_exists(result.manifest_path));
    // Removing twice is not an error.
    LX_CHECK_OK(backends::icd::remove_manifest(result.manifest_path));
}

LX_TEST(icd, removal_refuses_a_foreign_manifest) {
    const std::string dir = temp_dir("foreign");
    LX_CHECK_OK(make_directories(dir));
    const std::string path = dir + "/someone_else.json";
    LX_CHECK_OK(write_file(path, "{\"ICD\":{\"library_path\":\"/x/libother.so\"}}"));
    auto removed = backends::icd::remove_manifest(path);
    LX_CHECK(!removed.ok());
    LX_CHECK(file_exists(path));
}

LX_TEST(icd, user_directory_is_absolute_when_available) {
    const std::string dir = backends::icd::user_manifest_directory();
    // Either the environment provides a home (an absolute path) or it does not,
    // in which case the caller is told there is nowhere to install.
    if (!dir.empty()) LX_CHECK_EQ(dir.front(), '/');
}

LX_TEST(icd, search_paths_put_the_user_location_first) {
    const auto paths = backends::icd::manifest_search_paths();
    LX_CHECK(!paths.empty());
    const std::string user = backends::icd::user_manifest_directory();
    if (!user.empty() && !paths.empty()) LX_CHECK_EQ(paths.front(), user);
}

LX_TEST(icd, override_variable_is_the_loaders_own) {
    LX_CHECK_EQ(std::string(backends::icd::loader_override_variable()), "VK_ICD_FILENAMES");
}

#if defined(LX_TEST_HAVE_ICD)
// --- integration with the real loader ------------------------------------
//
// These run only when the ICD shared library and the loader are both present.
// They are the tests that prove the adapter exists, rather than that the
// manifest text is well formed.

namespace {

// Points the loader at the ICD built in this tree, so the test does not depend
// on anything having been installed into a system directory.
struct LoaderEnv {
    LoaderEnv() {
        setenv("VK_ICD_FILENAMES", LX_TEST_ICD_MANIFEST, 1);
        setenv("VK_LOADER_DEBUG", "error", 1);
    }
};

struct DeviceFixture {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;

    ~DeviceFixture() {
        if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }

    bool create() {
        VkInstanceCreateInfo instance_info{};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) return false;

        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS) return false;
        if (count == 0) return false;
        if (vkEnumeratePhysicalDevices(instance, &count, &physical) != VK_SUCCESS) return false;

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = 0;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        if (vkCreateDevice(physical, &device_info, nullptr, &device) != VK_SUCCESS) return false;
        vkGetDeviceQueue(device, 0, 0, &queue);
        return true;
    }
};

}  // namespace

LX_TEST(icd, loader_enumerates_the_virtual_device) {
    LoaderEnv env;
    DeviceFixture fixture;
    LX_CHECK(fixture.create());
    if (fixture.physical == VK_NULL_HANDLE) return;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(fixture.physical, &props);
    LX_CHECK_EQ(std::string(props.deviceName),
                std::string("LoucuraXXXX GPU (virtual, CPU-backed)"));
    // It must report itself as a CPU device. Claiming to be discrete hardware is
    // exactly the fake identification this project forbids.
    LX_CHECK_EQ(static_cast<int>(props.deviceType),
                static_cast<int>(VK_PHYSICAL_DEVICE_TYPE_CPU));
}

LX_TEST(icd, virtual_device_exposes_a_real_host_memory_heap) {
    LoaderEnv env;
    DeviceFixture fixture;
    LX_CHECK(fixture.create());
    if (fixture.physical == VK_NULL_HANDLE) return;

    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(fixture.physical, &memory);
    LX_CHECK(memory.memoryHeapCount >= 1);
    LX_CHECK(memory.memoryTypeCount >= 1);
    if (memory.memoryTypeCount >= 1) {
        // Host visible and coherent, which is what host memory actually is.
        LX_CHECK((memory.memoryTypes[0].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0);
        LX_CHECK((memory.memoryTypes[0].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0);
    }
}

LX_TEST(icd, mapped_memory_is_genuinely_writable) {
    LoaderEnv env;
    DeviceFixture fixture;
    LX_CHECK(fixture.create());
    if (fixture.device == VK_NULL_HANDLE) return;

    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = 4096;
    allocate.memoryTypeIndex = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    LX_CHECK_EQ(static_cast<int>(vkAllocateMemory(fixture.device, &allocate, nullptr, &memory)),
                static_cast<int>(VK_SUCCESS));
    if (memory == VK_NULL_HANDLE) return;

    void* mapped = nullptr;
    LX_CHECK_EQ(
        static_cast<int>(vkMapMemory(fixture.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped)),
        static_cast<int>(VK_SUCCESS));
    LX_CHECK(mapped != nullptr);
    if (mapped != nullptr) {
        // Writing through the mapping and reading it back is the difference
        // between real memory and a pointer to nothing.
        auto* bytes = static_cast<unsigned char*>(mapped);
        bytes[0] = 0x5A;
        bytes[4095] = 0xA5;
        LX_CHECK_EQ(static_cast<int>(bytes[0]), 0x5A);
        LX_CHECK_EQ(static_cast<int>(bytes[4095]), 0xA5);
    }
    vkUnmapMemory(fixture.device, memory);
    vkFreeMemory(fixture.device, memory, nullptr);
}

LX_TEST(icd, copy_command_executes_and_transfers_data) {
    LoaderEnv env;
    DeviceFixture fixture;
    LX_CHECK(fixture.create());
    if (fixture.device == VK_NULL_HANDLE) return;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = 256;
    buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer source = VK_NULL_HANDLE;
    VkBuffer target = VK_NULL_HANDLE;
    LX_CHECK_EQ(static_cast<int>(vkCreateBuffer(fixture.device, &buffer_info, nullptr, &source)),
                static_cast<int>(VK_SUCCESS));
    LX_CHECK_EQ(static_cast<int>(vkCreateBuffer(fixture.device, &buffer_info, nullptr, &target)),
                static_cast<int>(VK_SUCCESS));
    if (source == VK_NULL_HANDLE || target == VK_NULL_HANDLE) return;

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(fixture.device, source, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = 0;
    VkDeviceMemory source_memory = VK_NULL_HANDLE;
    VkDeviceMemory target_memory = VK_NULL_HANDLE;
    LX_CHECK_EQ(
        static_cast<int>(vkAllocateMemory(fixture.device, &allocate, nullptr, &source_memory)),
        static_cast<int>(VK_SUCCESS));
    LX_CHECK_EQ(
        static_cast<int>(vkAllocateMemory(fixture.device, &allocate, nullptr, &target_memory)),
        static_cast<int>(VK_SUCCESS));
    LX_CHECK_EQ(static_cast<int>(vkBindBufferMemory(fixture.device, source, source_memory, 0)),
                static_cast<int>(VK_SUCCESS));
    LX_CHECK_EQ(static_cast<int>(vkBindBufferMemory(fixture.device, target, target_memory, 0)),
                static_cast<int>(VK_SUCCESS));

    void* mapped = nullptr;
    vkMapMemory(fixture.device, source_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (mapped == nullptr) return;
    auto* bytes = static_cast<unsigned char*>(mapped);
    for (int i = 0; i < 256; ++i) bytes[i] = static_cast<unsigned char>(i);
    vkUnmapMemory(fixture.device, source_memory);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    LX_CHECK_EQ(static_cast<int>(vkCreateCommandPool(fixture.device, &pool_info, nullptr, &pool)),
                static_cast<int>(VK_SUCCESS));
    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    LX_CHECK_EQ(
        static_cast<int>(vkAllocateCommandBuffers(fixture.device, &command_info, &command)),
        static_cast<int>(VK_SUCCESS));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(command, &begin);
    VkBufferCopy region{};
    region.size = 256;
    vkCmdCopyBuffer(command, source, target, 1, &region);
    LX_CHECK_EQ(static_cast<int>(vkEndCommandBuffer(command)), static_cast<int>(VK_SUCCESS));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    LX_CHECK_EQ(static_cast<int>(vkQueueSubmit(fixture.queue, 1, &submit, VK_NULL_HANDLE)),
                static_cast<int>(VK_SUCCESS));

    void* result = nullptr;
    vkMapMemory(fixture.device, target_memory, 0, VK_WHOLE_SIZE, 0, &result);
    LX_CHECK(result != nullptr);
    if (result != nullptr) {
        auto* copied = static_cast<unsigned char*>(result);
        bool identical = true;
        for (int i = 0; i < 256; ++i) {
            if (copied[i] != static_cast<unsigned char>(i)) identical = false;
        }
        LX_CHECK(identical);
        vkUnmapMemory(fixture.device, target_memory);
    }

    vkDestroyCommandPool(fixture.device, pool, nullptr);
    vkFreeMemory(fixture.device, source_memory, nullptr);
    vkFreeMemory(fixture.device, target_memory, nullptr);
    vkDestroyBuffer(fixture.device, source, nullptr);
    vkDestroyBuffer(fixture.device, target, nullptr);
}

LX_TEST(icd, dispatch_is_refused_instead_of_silently_ignored) {
    LoaderEnv env;
    DeviceFixture fixture;
    LX_CHECK(fixture.create());
    if (fixture.device == VK_NULL_HANDLE) return;

    // Shader execution is not implemented. Submitting a dispatch must fail, so a
    // caller cannot believe work ran and read memory this driver never wrote.
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(fixture.device, &pool_info, nullptr, &pool);
    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(fixture.device, &command_info, &command);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(command, &begin);
    vkCmdDispatch(command, 1, 1, 1);
    vkEndCommandBuffer(command);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    const VkResult result = vkQueueSubmit(fixture.queue, 1, &submit, VK_NULL_HANDLE);
    LX_CHECK_EQ(static_cast<int>(result), static_cast<int>(VK_ERROR_FEATURE_NOT_PRESENT));

    vkDestroyCommandPool(fixture.device, pool, nullptr);
}

LX_TEST(icd, queue_family_advertises_compute_only) {
    LoaderEnv env;
    DeviceFixture fixture;
    LX_CHECK(fixture.create());
    if (fixture.physical == VK_NULL_HANDLE) return;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(fixture.physical, &count, nullptr);
    LX_CHECK(count >= 1);
    if (count == 0) return;
    VkQueueFamilyProperties props{};
    vkGetPhysicalDeviceQueueFamilyProperties(fixture.physical, &count, &props);
    LX_CHECK((props.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0);
    // Graphics and presentation are not implemented, so they must not be
    // advertised: an application would queue work that has to be refused.
    LX_CHECK((props.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0);
}

LX_TEST(icd, no_extensions_are_claimed) {
    LoaderEnv env;
    DeviceFixture fixture;
    LX_CHECK(fixture.create());
    if (fixture.physical == VK_NULL_HANDLE) return;

    uint32_t count = 0;
    LX_CHECK_EQ(static_cast<int>(vkEnumerateDeviceExtensionProperties(
                    fixture.physical, nullptr, &count, nullptr)),
                static_cast<int>(VK_SUCCESS));
    // Nothing is implemented beyond core memory operations, so nothing is offered.
    LX_CHECK_EQ(static_cast<int>(count), 0);
}
#endif  // LX_TEST_HAVE_ICD

LX_TEST_MAIN("vulkan_icd")
