//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "backends/vulkan/core/gpu.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace vulkan
{
namespace
{
struct BindlessBindingDesc
{
    Bindless binding;
    VkDescriptorType type;
    uint32_t count;
};

constexpr BindlessBindingDesc kBindlessBindings[] = {
    {Bindless::ByteBuffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384},
    {Bindless::MaterialTable, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024},
    {Bindless::Texture, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384},
    {Bindless::JointMatrices, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024},
    {Bindless::DeformedPositions, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192},
    {Bindless::DeformedAttributes, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192},
    {Bindless::Sampler, VK_DESCRIPTOR_TYPE_SAMPLER, 1},
};

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    const char* label = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : "WARNING";
    printf("[vulkan %s] %s\n", label, data->pMessage);
    fflush(stdout);
    return VK_FALSE;
}

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    for (const VkExtensionProperties& extension : extensions)
    {
        if (strcmp(extension.extensionName, name) == 0)
        {
            return true;
        }
    }
    return false;
}

std::vector<const char*> RequiredDeviceExtensions(bool enablePresentation, bool enableMlas = false)
{
    std::vector<const char*> extensions{
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    };
    if (enableMlas)
    {
        extensions.push_back(VK_AMDX_MULTI_LEVEL_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    }
    if (enablePresentation)
    {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    return extensions;
}

// pNext chain of every feature struct the device is queried for and created with. The MLAS struct is only linked
// when the extension is present, since chaining an unsupported extension's struct is invalid.
struct DeviceFeatures
{
    VkPhysicalDeviceMultiLevelAccelerationStructureFeaturesAMDX mlas{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_LEVEL_ACCELERATION_STRUCTURE_FEATURES_AMDX};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracing{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelStruct{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features vulkan13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features vulkan12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

    explicit DeviceFeatures(bool includeMlas)
    {
        rayQuery.pNext = includeMlas ? &mlas : nullptr;
        rayTracing.pNext = &rayQuery;
        accelStruct.pNext = &rayTracing;
        vulkan13.pNext = &accelStruct;
        vulkan12.pNext = &vulkan13;
        features2.pNext = &vulkan12;
    }

    // The chain points into this object, so it must not be copied or moved.
    DeviceFeatures(const DeviceFeatures&) = delete;
    DeviceFeatures& operator=(const DeviceFeatures&) = delete;
};

// Calls visit(featureBit, name) for every feature the renderer requires.
template <typename Visitor>
void ForEachRequiredFeature(DeviceFeatures& features, Visitor&& visit)
{
#define ASP_VISIT_FEATURE(member, feature) visit(features.member.feature, #feature)
    ASP_VISIT_FEATURE(vulkan12, bufferDeviceAddress);
    ASP_VISIT_FEATURE(vulkan12, descriptorIndexing);
    ASP_VISIT_FEATURE(vulkan12, runtimeDescriptorArray);
    ASP_VISIT_FEATURE(vulkan12, shaderStorageBufferArrayNonUniformIndexing);
    ASP_VISIT_FEATURE(vulkan12, shaderSampledImageArrayNonUniformIndexing);
    ASP_VISIT_FEATURE(vulkan12, descriptorBindingPartiallyBound);
    ASP_VISIT_FEATURE(vulkan12, descriptorBindingStorageBufferUpdateAfterBind);
    ASP_VISIT_FEATURE(vulkan12, descriptorBindingSampledImageUpdateAfterBind);
    ASP_VISIT_FEATURE(vulkan12, descriptorBindingUpdateUnusedWhilePending);
    ASP_VISIT_FEATURE(vulkan12, scalarBlockLayout);
    ASP_VISIT_FEATURE(vulkan13, synchronization2);
    ASP_VISIT_FEATURE(vulkan13, dynamicRendering);
    ASP_VISIT_FEATURE(accelStruct, accelerationStructure);
    ASP_VISIT_FEATURE(rayTracing, rayTracingPipeline);
    ASP_VISIT_FEATURE(rayQuery, rayQuery);
#undef ASP_VISIT_FEATURE
}

std::filesystem::path ShaderDirectory()
{
#ifdef ASP_SPIRV_DIR
    const std::filesystem::path buildDirectory = ASP_SPIRV_DIR;
    if (std::filesystem::exists(buildDirectory))
    {
        return buildDirectory;
    }
#endif
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    return std::filesystem::path(modulePath).parent_path() / "shaders";
}
} // namespace

void CheckVk(VkResult result, const char* expression, const char* file, int line)
{
    if (result < 0)
    {
        Fatal("%s failed with VkResult %d at %s:%d", expression, static_cast<int>(result), file, line);
    }
}

Buffer& Buffer::operator=(Buffer&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        allocator = other.allocator;
        buffer = other.buffer;
        allocation = other.allocation;
        address = other.address;
        size = other.size;
        mapped = other.mapped;
        other.buffer = VK_NULL_HANDLE;
        other.allocation = nullptr;
        other.address = 0;
        other.size = 0;
        other.mapped = nullptr;
    }
    return *this;
}

void Buffer::Reset()
{
    if (buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(allocator, buffer, allocation);
    }
    buffer = VK_NULL_HANDLE;
    allocation = nullptr;
    address = 0;
    size = 0;
    mapped = nullptr;
}

Image& Image::operator=(Image&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        allocator = other.allocator;
        device = other.device;
        image = other.image;
        allocation = other.allocation;
        view = other.view;
        format = other.format;
        width = other.width;
        height = other.height;
        other.image = VK_NULL_HANDLE;
        other.allocation = nullptr;
        other.view = VK_NULL_HANDLE;
    }
    return *this;
}

void Image::Reset()
{
    if (view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, view, nullptr);
    }
    if (image != VK_NULL_HANDLE)
    {
        vmaDestroyImage(allocator, image, allocation);
    }
    view = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    allocation = nullptr;
}

AccelerationStructure& AccelerationStructure::operator=(AccelerationStructure&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        device = other.device;
        handle = other.handle;
        address = other.address;
        storage = std::move(other.storage);
        other.handle = VK_NULL_HANDLE;
        other.address = 0;
    }
    return *this;
}

void AccelerationStructure::Reset()
{
    if (handle != VK_NULL_HANDLE)
    {
        vkDestroyAccelerationStructureKHR(device, handle, nullptr);
    }
    handle = VK_NULL_HANDLE;
    address = 0;
    storage.Reset();
}

void Gpu::Create(bool enableValidation, bool enablePresentation)
{
    VK_CHECK(volkInitialize());
    CreateInstance(enableValidation, enablePresentation);
    PickPhysicalDevice(enablePresentation);
    CreateDevice(enablePresentation);
    CreateAllocator();

    for (Frame& frame : m_frames)
    {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = m_queueFamily;
        VK_CHECK(vkCreateCommandPool(m_device, &poolInfo, nullptr, &frame.pool));

        VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocateInfo.commandPool = frame.pool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(m_device, &allocateInfo, &frame.commandBuffer));

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(m_device, &fenceInfo, nullptr, &frame.fence));
    }

    CreateBindlessSet();
}

void Gpu::CreateInstance(bool enableValidation, bool enablePresentation)
{
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3)
    {
        Fatal("The Vulkan loader does not support Vulkan 1.3.");
    }

    uint32_t count = 0;
    VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> available(count);
    VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()));

    std::vector<const char*> extensions;
    if (enablePresentation)
    {
        for (const char* name : {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME})
        {
            if (!HasExtension(available, name))
            {
                Fatal("Required Vulkan instance extension %s is not available.", name);
            }
            extensions.push_back(name);
        }
    }

    std::vector<const char*> layers;
    if (enableValidation)
    {
        uint32_t layerCount = 0;
        VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount, nullptr));
        std::vector<VkLayerProperties> availableLayers(layerCount);
        VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data()));
        constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
        const bool hasValidation =
            std::any_of(availableLayers.begin(), availableLayers.end(),
                        [&](const VkLayerProperties& layer) { return strcmp(layer.layerName, kValidationLayer) == 0; });
        if (hasValidation)
        {
            layers.push_back(kValidationLayer);
        }
        else
        {
            fprintf(stderr, "Warning: %s is not installed; continuing without validation.\n", kValidationLayer);
        }
        if (HasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            m_debugUtils = true;
        }
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "AccelerationStructurePlayground";
    application.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    VK_CHECK(vkCreateInstance(&info, nullptr, &m_instance));
    volkLoadInstance(m_instance);

    if (m_debugUtils)
    {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messengerInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = DebugCallback;
        VK_CHECK(vkCreateDebugUtilsMessengerEXT(m_instance, &messengerInfo, nullptr, &m_messenger));
    }
}

void Gpu::PickPhysicalDevice(bool enablePresentation)
{
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(m_instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(m_instance, &count, devices.data()));

    const std::vector<const char*> required = RequiredDeviceExtensions(enablePresentation);
    int bestScore = -1;
    for (VkPhysicalDevice device : devices)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3)
        {
            continue;
        }

        uint32_t extensionCount = 0;
        VK_CHECK(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr));
        std::vector<VkExtensionProperties> extensions(extensionCount);
        VK_CHECK(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data()));
        const bool hasExtensions = std::all_of(required.begin(), required.end(),
                                               [&](const char* name) { return HasExtension(extensions, name); });
        if (!hasExtensions)
        {
            continue;
        }

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
        int family = -1;
        for (uint32_t index = 0; index < familyCount; ++index)
        {
            const VkQueueFlags flags = families[index].queueFlags;
            if ((flags & VK_QUEUE_GRAPHICS_BIT) && (flags & VK_QUEUE_COMPUTE_BIT) &&
                families[index].timestampValidBits > 0 &&
                (!enablePresentation || vkGetPhysicalDeviceWin32PresentationSupportKHR(device, index)))
            {
                family = static_cast<int>(index);
                break;
            }
        }
        if (family < 0)
        {
            continue;
        }

        // Query required features and MLAS support in the Vulkan driver
        const bool hasMlasExtension =
            HasExtension(extensions, VK_AMDX_MULTI_LEVEL_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        DeviceFeatures supported(hasMlasExtension);
        vkGetPhysicalDeviceFeatures2(device, &supported.features2);
        const char* missingFeature = nullptr;
        ForEachRequiredFeature(supported, [&](VkBool32 value, const char* name) {
            if (!value && missingFeature == nullptr)
            {
                missingFeature = name;
            }
        });
        if (missingFeature != nullptr)
        {
            fprintf(stderr, "Warning: skipping Vulkan device %s, which does not support required feature %s.\n",
                    properties.deviceName, missingFeature);
            continue;
        }
        const bool supportsMlas = hasMlasExtension && supported.mlas.multiLevelAccelerationStructure;

        const int deviceTypeScore = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 2
                                    : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                                                      : 0;
        const int score = deviceTypeScore * 2 + static_cast<int>(supportsMlas);
        if (score > bestScore)
        {
            bestScore = score;
            m_physicalDevice = device;
            m_queueFamily = static_cast<uint32_t>(family);
            m_mlasEnabled = supportsMlas;
        }
    }

    if (m_physicalDevice == VK_NULL_HANDLE)
    {
        Fatal("No Vulkan 1.3 device with the required ray tracing extensions and features found.");
    }

    m_rayTracingProperties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    m_accelerationStructureProperties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    m_accelerationStructureProperties.pNext = &m_rayTracingProperties;
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &m_accelerationStructureProperties;
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &properties);
    m_accelerationStructureProperties.pNext = nullptr;
    m_rayTracingProperties.pNext = nullptr;
    m_adapterName = properties.properties.deviceName;
    m_timestampPeriod = properties.properties.limits.timestampPeriod;
}

void Gpu::CreateDevice(bool enablePresentation)
{
    // PickPhysicalDevice already verified the required features and determined MLAS support.
    DeviceFeatures enabled(m_mlasEnabled);
    ForEachRequiredFeature(enabled, [](VkBool32& value, const char*) { value = VK_TRUE; });
    enabled.mlas.multiLevelAccelerationStructure = m_mlasEnabled ? VK_TRUE : VK_FALSE;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = m_queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const std::vector<const char*> extensions = RequiredDeviceExtensions(enablePresentation, m_mlasEnabled);
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enabled.features2};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    VK_CHECK(vkCreateDevice(m_physicalDevice, &info, nullptr, &m_device));
    volkLoadDevice(m_device);
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
}

void Gpu::CreateAllocator()
{
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.instance = m_instance;
    info.physicalDevice = m_physicalDevice;
    info.device = m_device;
    info.pVulkanFunctions = &functions;
    VK_CHECK(vmaCreateAllocator(&info, &m_allocator));
}

void Gpu::CreateBindlessSet()
{
    VkPhysicalDeviceVulkan12Properties limits12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &limits12};
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &properties);
    uint32_t storageBuffers = 0;
    uint32_t sampledImages = 0;
    for (const BindlessBindingDesc& desc : kBindlessBindings)
    {
        storageBuffers += desc.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? desc.count : 0;
        sampledImages += desc.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? desc.count : 0;
    }
    if (storageBuffers > limits12.maxPerStageDescriptorUpdateAfterBindStorageBuffers ||
        sampledImages > limits12.maxPerStageDescriptorUpdateAfterBindSampledImages)
    {
        Fatal("Vulkan device %s cannot hold the bindless descriptor set (%u storage buffers, %u sampled images).",
              m_adapterName.c_str(), storageBuffers, sampledImages);
    }

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler));

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<VkDescriptorBindingFlags> flags;
    for (const BindlessBindingDesc& desc : kBindlessBindings)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = static_cast<uint32_t>(desc.binding);
        binding.descriptorType = desc.type;
        binding.descriptorCount = desc.count;
        binding.stageFlags = VK_SHADER_STAGE_ALL;
        const bool sampler = desc.type == VK_DESCRIPTOR_TYPE_SAMPLER;
        binding.pImmutableSamplers = sampler ? &m_sampler : nullptr;
        bindings.push_back(binding);
        flags.push_back(sampler
                            ? 0
                            : VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                  VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT);
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagsInfo.bindingCount = static_cast<uint32_t>(flags.size());
    flagsInfo.pBindingFlags = flags.data();
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flagsInfo};
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_bindlessLayout));

    const VkDescriptorPoolSize poolSizes[]{
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storageBuffers},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampledImages},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_bindlessPool));

    VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = m_bindlessPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &m_bindlessLayout;
    VK_CHECK(vkAllocateDescriptorSets(m_device, &allocateInfo, &m_bindlessSet));

    m_bindlessUsed.assign(std::size(kBindlessBindings), 0);
    m_bindlessCapacity.resize(std::size(kBindlessBindings));
    for (const BindlessBindingDesc& desc : kBindlessBindings)
    {
        m_bindlessCapacity[static_cast<uint32_t>(desc.binding)] = desc.count;
    }
}

void Gpu::Destroy()
{
    if (m_device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(m_device);
        for (Frame& frame : m_frames)
        {
            frame.retained.clear();
            vkDestroyFence(m_device, frame.fence, nullptr);
            vkDestroyCommandPool(m_device, frame.pool, nullptr);
            frame = Frame{};
        }
        vkDestroyDescriptorPool(m_device, m_bindlessPool, nullptr);
        vkDestroyDescriptorSetLayout(m_device, m_bindlessLayout, nullptr);
        vkDestroySampler(m_device, m_sampler, nullptr);
        if (m_allocator)
        {
            vmaDestroyAllocator(m_allocator);
            m_allocator = nullptr;
        }
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    if (m_messenger != VK_NULL_HANDLE)
    {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
        m_messenger = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

uint32_t Gpu::AllocateDescriptor(Bindless binding)
{
    const uint32_t slot = static_cast<uint32_t>(binding);
    if (m_bindlessUsed[slot] >= m_bindlessCapacity[slot])
    {
        Fatal("Bindless descriptor binding %u exhausted (capacity %u).", slot, m_bindlessCapacity[slot]);
    }
    return m_bindlessUsed[slot]++;
}

uint32_t Gpu::AddStorageBuffer(Bindless binding, const Buffer& buffer)
{
    const uint32_t index = AllocateDescriptor(binding);
    VkDescriptorBufferInfo bufferInfo{buffer.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_bindlessSet;
    write.dstBinding = static_cast<uint32_t>(binding);
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    return index;
}

uint32_t Gpu::AddTexture(VkImageView view)
{
    const uint32_t index = AllocateDescriptor(Bindless::Texture);
    VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_bindlessSet;
    write.dstBinding = static_cast<uint32_t>(Bindless::Texture);
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    return index;
}

VkCommandBuffer Gpu::Begin()
{
    if (m_listOpen)
    {
        Fatal("Gpu::Begin called while the command buffer is already open.");
    }
    Frame& frame = m_frames[m_frameIndex];
    if (frame.submitted)
    {
        VK_CHECK(vkWaitForFences(m_device, 1, &frame.fence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(m_device, 1, &frame.fence));
        frame.submitted = false;
    }
    frame.retained.clear();
    VK_CHECK(vkResetCommandPool(m_device, frame.pool, 0));

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo));
    m_listOpen = true;
    return frame.commandBuffer;
}

void Gpu::Cancel()
{
    if (!m_listOpen)
    {
        Fatal("Gpu::Cancel called without a matching Begin.");
    }
    VK_CHECK(vkEndCommandBuffer(m_frames[m_frameIndex].commandBuffer));
    m_listOpen = false;
}

void Gpu::EndAndSubmit(VkSemaphore wait, VkPipelineStageFlags2 waitStage, VkSemaphore signal)
{
    if (!m_listOpen)
    {
        Fatal("Gpu::EndAndSubmit called without a matching Begin.");
    }
    Frame& frame = m_frames[m_frameIndex];
    VK_CHECK(vkEndCommandBuffer(frame.commandBuffer));

    VkCommandBufferSubmitInfo commandBufferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    commandBufferInfo.commandBuffer = frame.commandBuffer;
    VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = wait;
    waitInfo.stageMask = waitStage;
    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = signal;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandBufferInfo;
    submit.waitSemaphoreInfoCount = wait != VK_NULL_HANDLE ? 1 : 0;
    submit.pWaitSemaphoreInfos = &waitInfo;
    submit.signalSemaphoreInfoCount = signal != VK_NULL_HANDLE ? 1 : 0;
    submit.pSignalSemaphoreInfos = &signalInfo;
    VK_CHECK(vkQueueSubmit2(m_queue, 1, &submit, frame.fence));

    frame.submitted = true;
    m_frameIndex = (m_frameIndex + 1) % kFrameCount;
    m_listOpen = false;
}

void Gpu::WaitForGpu()
{
    VK_CHECK(vkQueueWaitIdle(m_queue));
    for (uint32_t index = 0; index < kFrameCount; ++index)
    {
        if (!(m_listOpen && index == m_frameIndex))
        {
            m_frames[index].retained.clear();
        }
    }
}

void Gpu::Retain(Buffer&& buffer)
{
    m_frames[m_frameIndex].retained.push_back(std::move(buffer));
}

void Gpu::SetName(VkObjectType type, uint64_t handle, const char* name) const
{
    if (!m_debugUtils || !name)
    {
        return;
    }
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(m_device, &info);
}

Buffer Gpu::CreateBuffer(uint64_t size, VkBufferUsageFlags usage, MemoryKind memory, const char* name,
                         uint64_t alignment)
{
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<uint64_t>(size, 4);
    info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    switch (memory)
    {
    case MemoryKind::DeviceLocal:
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    case MemoryKind::Upload:
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        break;
    case MemoryKind::Readback:
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        break;
    }

    Buffer buffer;
    buffer.allocator = m_allocator;
    buffer.size = info.size;
    VmaAllocationInfo result{};
    if (alignment > 0)
    {
        VK_CHECK(vmaCreateBufferWithAlignment(m_allocator, &info, &allocationInfo, alignment, &buffer.buffer,
                                              &buffer.allocation, &result));
    }
    else
    {
        VK_CHECK(vmaCreateBuffer(m_allocator, &info, &allocationInfo, &buffer.buffer, &buffer.allocation, &result));
    }
    buffer.mapped = result.pMappedData;
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = buffer.buffer;
    buffer.address = vkGetBufferDeviceAddress(m_device, &addressInfo);
    SetName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(buffer.buffer), name);
    return buffer;
}

Buffer Gpu::CreateUploadBuffer(const void* data, uint64_t size, VkBufferUsageFlags usage, const char* name,
                               uint64_t alignment)
{
    Buffer buffer = CreateBuffer(size, usage, MemoryKind::Upload, name, alignment);
    if (data && size > 0)
    {
        memcpy(buffer.mapped, data, static_cast<size_t>(size));
    }
    return buffer;
}

Buffer Gpu::CreateDefaultBuffer(const void* data, uint64_t size, VkBufferUsageFlags usage, const char* name)
{
    if (!m_listOpen)
    {
        Fatal("CreateDefaultBuffer requires an open command buffer.");
    }
    Buffer buffer = CreateBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryKind::DeviceLocal, name);
    if (size == 0)
    {
        return buffer;
    }
    Buffer staging = CreateUploadBuffer(data, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "Staging");
    const VkCommandBuffer commandBuffer = m_frames[m_frameIndex].commandBuffer;
    const VkBufferCopy region{0, 0, size};
    vkCmdCopyBuffer(commandBuffer, staging.buffer, buffer.buffer, 1, &region);
    Barrier(commandBuffer, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    Retain(std::move(staging));
    return buffer;
}

Image Gpu::CreateImage2D(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage, const char* name)
{
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    Image image;
    image.allocator = m_allocator;
    image.device = m_device;
    image.format = format;
    image.width = width;
    image.height = height;
    VK_CHECK(vmaCreateImage(m_allocator, &info, &allocationInfo, &image.image, &image.allocation, nullptr));
    SetName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(image.image), name);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(m_device, &viewInfo, nullptr, &image.view));
    return image;
}

void Gpu::UploadTexture2D(Image& image, const void* pixels, uint32_t bytesPerPixel)
{
    if (!m_listOpen)
    {
        Fatal("UploadTexture2D requires an open command buffer.");
    }
    const uint64_t size = static_cast<uint64_t>(image.width) * image.height * bytesPerPixel;
    Buffer staging = CreateUploadBuffer(pixels, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "TextureStaging");
    const VkCommandBuffer commandBuffer = m_frames[m_frameIndex].commandBuffer;
    ImageBarrier(commandBuffer, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {image.width, image.height, 1};
    vkCmdCopyBufferToImage(commandBuffer, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);
    ImageBarrier(commandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    Retain(std::move(staging));
}

VkShaderModule Gpu::LoadShaderModule(const char* fileName) const
{
    const std::filesystem::path path = ShaderDirectory() / fileName;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        Fatal("Could not read SPIR-V shader '%s'.", path.string().c_str());
    }
    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> code((size + 3) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(m_device, &info, nullptr, &module));
    return module;
}

void Gpu::Barrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void Gpu::ImageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                       VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                       VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}
} // namespace vulkan
