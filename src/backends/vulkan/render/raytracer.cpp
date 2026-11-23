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

#include "backends/vulkan/render/raytracer.h"

#include "core/math.h"

#include <cstring>

namespace vulkan
{
namespace
{
constexpr VkShaderStageFlags kRayTracingStages =
    VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
} // namespace

void RayTracer::Create(Gpu& gpu)
{
    m_device = gpu.Device();
    m_timestampPeriodNs = gpu.TimestampPeriodNs();
    CreateFrameSets(gpu);
    CreatePipeline(gpu);
    CreateShaderTable(gpu);
    for (Buffer& buffer : m_constants)
    {
        buffer = gpu.CreateUploadBuffer(nullptr, sizeof(FrameConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                        "FrameConstants");
    }

    VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = Gpu::kFrameCount * kTimestampsPerFrame;
    VK_CHECK(vkCreateQueryPool(m_device, &queryInfo, nullptr, &m_timestampPool));
}

void RayTracer::Destroy()
{
    if (m_device == VK_NULL_HANDLE)
    {
        return;
    }
    vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
    vkDestroyPipeline(m_device, m_pipeline, nullptr);
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    vkDestroyDescriptorPool(m_device, m_framePool, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_frameLayout, nullptr);
    m_shaderTable.Reset();
    for (Buffer& buffer : m_constants)
    {
        buffer.Reset();
    }
    m_device = VK_NULL_HANDLE;
}

void RayTracer::CreateFrameSets(Gpu& gpu)
{
    const VkDescriptorSetLayoutBinding bindings[]{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kRayTracingStages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, kRayTracingStages, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kRayTracingStages, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRayTracingStages, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<uint32_t>(std::size(bindings));
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_frameLayout));

    const VkDescriptorPoolSize poolSizes[]{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, Gpu::kFrameCount},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, Gpu::kFrameCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, Gpu::kFrameCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Gpu::kFrameCount},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = Gpu::kFrameCount;
    poolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_framePool));

    VkDescriptorSetLayout layouts[Gpu::kFrameCount];
    std::fill(std::begin(layouts), std::end(layouts), m_frameLayout);
    VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = m_framePool;
    allocateInfo.descriptorSetCount = Gpu::kFrameCount;
    allocateInfo.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(m_device, &allocateInfo, m_frameSets));

    const VkDescriptorSetLayout setLayouts[]{gpu.BindlessLayout(), m_frameLayout};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(std::size(setLayouts));
    pipelineLayoutInfo.pSetLayouts = setLayouts;
    VK_CHECK(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout));
}

void RayTracer::CreatePipeline(Gpu& gpu)
{
    const VkShaderModule module = gpu.LoadShaderModule("raytracing.spv");

    VkPipelineShaderStageCreateInfo stages[3]{};
    const std::pair<VkShaderStageFlagBits, const char*> entryPoints[]{
        {VK_SHADER_STAGE_RAYGEN_BIT_KHR, "RayGeneration"},
        {VK_SHADER_STAGE_MISS_BIT_KHR, "Miss"},
        {VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, "ClosestHit"},
    };
    for (uint32_t index = 0; index < 3; ++index)
    {
        stages[index] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[index].stage = entryPoints[index].first;
        stages[index].module = module;
        stages[index].pName = entryPoints[index].second;
    }

    VkRayTracingShaderGroupCreateInfoKHR groups[3]{};
    for (uint32_t index = 0; index < 3; ++index)
    {
        groups[index] = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        groups[index].generalShader = VK_SHADER_UNUSED_KHR;
        groups[index].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[index].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[index].intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].closestHitShader = 2;

    VkRayTracingPipelineCreateInfoKHR info{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    info.stageCount = static_cast<uint32_t>(std::size(stages));
    info.pStages = stages;
    info.groupCount = static_cast<uint32_t>(std::size(groups));
    info.pGroups = groups;
    // Reflection bounces are chained by looping TraceRay from RayGeneration (see
    // raytracing_vulkan.hlsl), not by recursing through ClosestHit, because AMD's driver only
    // guarantees maxRayRecursionDepth == 1 (the spec's minimum). So the pipeline itself only
    // ever needs depth 1.
    info.maxPipelineRayRecursionDepth = 1;
    info.layout = m_pipelineLayout;
    // Enable MLAS in the ray tracing pipeline
    VkPipelineCreateFlags2CreateInfo flags2{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO};
    flags2.flags =
        gpu.MlasEnabled() ? VK_PIPELINE_CREATE_2_RAY_TRACING_ALLOW_MULTI_LEVEL_ACCELERATION_STRUCTURES_BIT_AMDX : 0;
    info.pNext = gpu.MlasEnabled() ? &flags2 : nullptr;
    const VkResult result =
        vkCreateRayTracingPipelinesKHR(m_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &m_pipeline);
    vkDestroyShaderModule(m_device, module, nullptr);
    VK_CHECK(result);
}

void RayTracer::CreateShaderTable(Gpu& gpu)
{
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& properties = gpu.RayTracingProperties();
    const uint32_t handleSize = properties.shaderGroupHandleSize;
    const uint64_t stride = AlignUp(handleSize, properties.shaderGroupHandleAlignment);
    const uint64_t regionSize = AlignUp(stride, properties.shaderGroupBaseAlignment);
    constexpr uint32_t kGroupCount = 3;

    std::vector<uint8_t> handles(static_cast<size_t>(handleSize) * kGroupCount);
    VK_CHECK(
        vkGetRayTracingShaderGroupHandlesKHR(m_device, m_pipeline, 0, kGroupCount, handles.size(), handles.data()));
    std::vector<uint8_t> table(static_cast<size_t>(regionSize) * kGroupCount, 0);
    for (uint32_t group = 0; group < kGroupCount; ++group)
    {
        memcpy(table.data() + group * regionSize, handles.data() + group * handleSize, handleSize);
    }
    m_shaderTable = gpu.CreateUploadBuffer(table.data(), table.size(), VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR,
                                           "ShaderTable", properties.shaderGroupBaseAlignment);

    const VkDeviceAddress base = m_shaderTable.Address();
    m_rayGenerationRegion = {base, stride, stride};
    m_missRegion = {base + regionSize, stride, stride};
    m_hitGroupRegion = {base + 2 * regionSize, stride, stride};
}

void RayTracer::Render(Gpu& gpu, VkCommandBuffer commandBuffer, const FrameConstants& constants,
                       VkAccelerationStructureKHR tlas, const Buffer& geometryTable, VkImageView output, uint32_t width,
                       uint32_t height)
{
    // Indexed by the GPU frame slot, whose previous use Gpu::Begin() already waited for.
    const uint32_t ring = gpu.FrameIndex();
    memcpy(m_constants[ring].mapped, &constants, sizeof(constants));

    const VkDescriptorBufferInfo constantsInfo{m_constants[ring].buffer, 0, sizeof(FrameConstants)};
    VkWriteDescriptorSetAccelerationStructureKHR tlasInfo{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    tlasInfo.accelerationStructureCount = 1;
    tlasInfo.pAccelerationStructures = &tlas;
    const VkDescriptorImageInfo outputInfo{VK_NULL_HANDLE, output, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo geometryTableInfo{geometryTable.buffer, 0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet writes[4]{};
    for (uint32_t binding = 0; binding < 4; ++binding)
    {
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[binding].dstSet = m_frameSets[ring];
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &constantsInfo;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[1].pNext = &tlasInfo;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &outputInfo;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &geometryTableInfo;
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);

    const VkDescriptorSet sets[]{gpu.BindlessSet(), m_frameSets[ring]};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_pipelineLayout, 0,
                            static_cast<uint32_t>(std::size(sets)), sets, 0, nullptr);

    const uint32_t base = (m_timestampFrame % Gpu::kFrameCount) * kTimestampsPerFrame;
    if (m_timestampFrame >= Gpu::kFrameCount)
    {
        uint64_t timestamps[kTimestampsPerFrame]{};
        if (vkGetQueryPoolResults(m_device, m_timestampPool, base, kTimestampsPerFrame, sizeof(timestamps), timestamps,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        {
            m_traceRaysMs = static_cast<float>((timestamps[1] - timestamps[0]) * m_timestampPeriodNs * 1e-6);
            m_traceRaysAverage.Add(m_traceRaysMs);
        }
    }
    vkCmdResetQueryPool(commandBuffer, m_timestampPool, base, kTimestampsPerFrame);
    vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_timestampPool, base + 0);
    vkCmdTraceRaysKHR(commandBuffer, &m_rayGenerationRegion, &m_missRegion, &m_hitGroupRegion, &m_callableRegion, width,
                      height, 1);
    vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_timestampPool, base + 1);
    ++m_timestampFrame;
}
} // namespace vulkan
