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

#pragma once

#include <exception>

#include "backends/vulkan/core/vk.h"

#include <string>
#include <vector>

namespace vulkan
{
enum class MemoryKind
{
    DeviceLocal,
    Upload,   // host-visible, persistently mapped, CPU writes
    Readback, // host-visible, persistently mapped, CPU reads
};

// Move-only VMA buffer. Upload/readback buffers stay persistently mapped.
struct Buffer
{
    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept { *this = std::move(other); }
    Buffer& operator=(Buffer&& other) noexcept;
    ~Buffer() { Reset(); }
    void Reset();

    explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
    VkDeviceAddress Address() const { return address; }

    VmaAllocator allocator = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkDeviceAddress address = 0;
    uint64_t size = 0;
    void* mapped = nullptr;
};

struct Image
{
    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&& other) noexcept { *this = std::move(other); }
    Image& operator=(Image&& other) noexcept;
    ~Image() { Reset(); }
    void Reset();

    explicit operator bool() const { return image != VK_NULL_HANDLE; }

    VmaAllocator allocator = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct AccelerationStructure
{
    AccelerationStructure() = default;
    AccelerationStructure(const AccelerationStructure&) = delete;
    AccelerationStructure& operator=(const AccelerationStructure&) = delete;
    AccelerationStructure(AccelerationStructure&& other) noexcept { *this = std::move(other); }
    AccelerationStructure& operator=(AccelerationStructure&& other) noexcept;
    ~AccelerationStructure() { Reset(); }
    void Reset();

    explicit operator bool() const { return handle != VK_NULL_HANDLE; }

    VkDevice device = VK_NULL_HANDLE;
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    Buffer storage;
};

// Bindings of the bindless descriptor set (set 0). Must match the arrays in shaders/common.hlsli.
enum class Bindless : uint32_t
{
    ByteBuffer = 0,
    MaterialTable = 1,
    Texture = 2,
    JointMatrices = 3,
    DeformedPositions = 4,
    DeformedAttributes = 5,
    Sampler = 6,
};

// Owns the Vulkan instance, device, the single graphics/compute queue, a small command-buffer
// ring and the bindless descriptor set shared by every pipeline.
class Gpu
{
public:
    ~Gpu()
    {
        try
        {
            Destroy();
        }
        catch (const std::exception&)
        {
        }
    }
    static constexpr uint32_t kFrameCount = 2;

    void Create(bool enableValidation, bool enablePresentation);
    void Destroy();

    VkInstance Instance() const { return m_instance; }
    VkPhysicalDevice PhysicalDevice() const { return m_physicalDevice; }
    VkDevice Device() const { return m_device; }
    VkQueue Queue() const { return m_queue; }
    uint32_t QueueFamily() const { return m_queueFamily; }
    VmaAllocator Allocator() const { return m_allocator; }
    const std::string& AdapterName() const { return m_adapterName; }
    bool MlasEnabled() const { return m_mlasEnabled; }
    double TimestampPeriodNs() const { return m_timestampPeriod; }
    const VkPhysicalDeviceAccelerationStructurePropertiesKHR& AccelerationStructureProperties() const
    {
        return m_accelerationStructureProperties;
    }
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& RayTracingProperties() const
    {
        return m_rayTracingProperties;
    }

    VkDescriptorSetLayout BindlessLayout() const { return m_bindlessLayout; }
    VkDescriptorSet BindlessSet() const { return m_bindlessSet; }
    // Writes a storage-buffer descriptor into the given bindless array and returns its index.
    uint32_t AddStorageBuffer(Bindless binding, const Buffer& buffer);
    uint32_t AddTexture(VkImageView view);

    // Opens the command buffer for the next frame, waiting until its resources are free.
    VkCommandBuffer Begin();
    void Cancel();
    void EndAndSubmit(VkSemaphore wait = VK_NULL_HANDLE, VkPipelineStageFlags2 waitStage = 0,
                      VkSemaphore signal = VK_NULL_HANDLE);
    void WaitForGpu();
    // Ring slot of the currently open (or next) frame.
    uint32_t FrameIndex() const { return m_frameIndex; }

    Buffer CreateBuffer(uint64_t size, VkBufferUsageFlags usage, MemoryKind memory, const char* name = nullptr,
                        uint64_t alignment = 0);
    // Persistently mapped host-visible buffer, optionally pre-filled with `data`.
    Buffer CreateUploadBuffer(const void* data, uint64_t size, VkBufferUsageFlags usage, const char* name = nullptr,
                              uint64_t alignment = 0);
    // Device-local buffer filled through a staging copy recorded into the open command buffer.
    Buffer CreateDefaultBuffer(const void* data, uint64_t size, VkBufferUsageFlags usage, const char* name = nullptr);
    Image CreateImage2D(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                        const char* name = nullptr);
    // Uploads tightly packed pixels into mip 0 and leaves the image in SHADER_READ_ONLY_OPTIMAL.
    void UploadTexture2D(Image& image, const void* pixels, uint32_t bytesPerPixel);

    // Keeps a buffer alive until the current frame's work has completed on the GPU.
    void Retain(Buffer&& buffer);
    void SetName(VkObjectType type, uint64_t handle, const char* name) const;
    VkShaderModule LoadShaderModule(const char* fileName) const;

    static void Barrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                        VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);
    static void ImageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout,
                             VkImageLayout newLayout, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

private:
    struct Frame
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool submitted = false;
        std::vector<Buffer> retained;
    };

    void CreateInstance(bool enableValidation, bool enablePresentation);
    void PickPhysicalDevice(bool enablePresentation);
    void CreateDevice(bool enablePresentation);
    void CreateAllocator();
    void CreateBindlessSet();
    uint32_t AllocateDescriptor(Bindless binding);

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    bool m_debugUtils = false;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    bool m_mlasEnabled = false;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VmaAllocator m_allocator = nullptr;
    std::string m_adapterName;
    double m_timestampPeriod = 1.0;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR m_accelerationStructureProperties{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_rayTracingProperties{};

    Frame m_frames[kFrameCount];
    uint32_t m_frameIndex = 0;
    bool m_listOpen = false;

    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_bindlessLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_bindlessPool = VK_NULL_HANDLE;
    VkDescriptorSet m_bindlessSet = VK_NULL_HANDLE;
    std::vector<uint32_t> m_bindlessUsed;
    std::vector<uint32_t> m_bindlessCapacity;
};
} // namespace vulkan
