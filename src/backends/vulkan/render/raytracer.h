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

#include "backends/vulkan/core/gpu.h"
#include "render/gpu_types.h"

namespace vulkan
{
// KHR ray tracing pipeline: raygen, miss and one triangle hit group. Set 0 is the shared bindless
// set and set 1 carries per-frame bindings (constants, TLAS, output image, geometry table).
class RayTracer
{
public:
    RayTracer() = default;
    RayTracer(const RayTracer&) = delete;
    RayTracer& operator=(const RayTracer&) = delete;
    ~RayTracer() { Destroy(); }

    void Create(Gpu& gpu);
    void Destroy();
    void Render(Gpu& gpu, VkCommandBuffer commandBuffer, const FrameConstants& constants,
                VkAccelerationStructureKHR tlas, const Buffer& geometryTable, VkImageView output, uint32_t width,
                uint32_t height);

    static constexpr uint32_t kTimingHistorySize = 64;
    using Timing = RollingAverage<kTimingHistorySize>;
    float TraceRaysMilliseconds() const { return m_traceRaysMs; }
    const Timing& TraceRaysTiming() const { return m_traceRaysAverage; }

private:
    void CreateFrameSets(Gpu& gpu);
    void CreatePipeline(Gpu& gpu);
    void CreateShaderTable(Gpu& gpu);

    static constexpr uint32_t kTimestampsPerFrame = 2;

    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_frameLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_framePool = VK_NULL_HANDLE;
    VkDescriptorSet m_frameSets[Gpu::kFrameCount]{};
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    Buffer m_shaderTable;
    VkStridedDeviceAddressRegionKHR m_rayGenerationRegion{};
    VkStridedDeviceAddressRegionKHR m_missRegion{};
    VkStridedDeviceAddressRegionKHR m_hitGroupRegion{};
    VkStridedDeviceAddressRegionKHR m_callableRegion{};
    Buffer m_constants[Gpu::kFrameCount];

    VkQueryPool m_timestampPool = VK_NULL_HANDLE;
    double m_timestampPeriodNs = 1.0;
    uint32_t m_timestampFrame = 0;
    float m_traceRaysMs = 0.0f;
    Timing m_traceRaysAverage;
};
} // namespace vulkan
