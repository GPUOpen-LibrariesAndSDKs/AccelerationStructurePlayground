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

#include "backends/vulkan/render/deform.h"

#include <utility>

namespace vulkan
{
namespace
{
} // namespace

Deformer& Deformer::operator=(Deformer&& other) noexcept
{
    if (this != &other)
    {
        DestroyPipeline();
        m_assetGpu = other.m_assetGpu;
        m_device = other.m_device;
        m_pipelineLayout = std::exchange(other.m_pipelineLayout, VK_NULL_HANDLE);
        m_pipeline = std::exchange(other.m_pipeline, VK_NULL_HANDLE);
        m_targets = std::move(other.m_targets);
        m_jointLayout = std::move(other.m_jointLayout);
        for (uint32_t i = 0; i < Gpu::kFrameCount; ++i)
        {
            m_jointMatrices[i] = std::move(other.m_jointMatrices[i]);
            m_jointMatrixSrv[i] = other.m_jointMatrixSrv[i];
        }
    }
    return *this;
}

void Deformer::DestroyPipeline()
{
    if (m_pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
}

void Deformer::Create(Gpu& gpu, const GltfAsset& asset, const AssetGpu& assetGpu)
{
    m_assetGpu = &assetGpu;
    m_device = gpu.Device();

    for (const auto& nodeOwner : asset.nodes)
    {
        const Gltf::Node& node = *nodeOwner;
        if (!node.pMesh || !node.pSkin)
        {
            continue;
        }

        for (const auto& source : node.pMesh->primitives)
        {
            const GpuPrimitive& primitive = assetGpu.PrimitiveFor(source.get());
            if (!primitive.HasSkinData())
            {
                continue;
            }

            Target target;
            target.node = &node;
            target.primitive = source.get();
            target.skin = node.pSkin;
            target.deformedPositions =
                gpu.CreateBuffer(primitive.vertexCount * sizeof(float3),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                                 MemoryKind::DeviceLocal, "DeformedPositions");
            target.deformedAttributes =
                gpu.CreateBuffer(primitive.vertexCount * sizeof(VertexAttributes), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 MemoryKind::DeviceLocal, "DeformedAttributes");
            target.deformedPositionUav = gpu.AddStorageBuffer(Bindless::DeformedPositions, target.deformedPositions);
            target.deformedPositionSrv = gpu.AddStorageBuffer(Bindless::ByteBuffer, target.deformedPositions);
            target.deformedAttributeUav = gpu.AddStorageBuffer(Bindless::DeformedAttributes, target.deformedAttributes);
            target.deformedAttributeSrv = gpu.AddStorageBuffer(Bindless::ByteBuffer, target.deformedAttributes);
            m_targets.push_back(std::move(target));
        }
    }

    if (m_targets.empty())
    {
        return;
    }

    m_jointLayout = BuildJointMatrixLayout(asset);

    for (uint32_t i = 0; i < Gpu::kFrameCount; ++i)
    {
        m_jointMatrices[i] = gpu.CreateUploadBuffer(nullptr, m_jointLayout.totalMatrices * sizeof(JointMatrix),
                                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "JointMatrices");
        m_jointMatrixSrv[i] = gpu.AddStorageBuffer(Bindless::JointMatrices, m_jointMatrices[i]);
    }

    const VkDescriptorSetLayout setLayout = gpu.BindlessLayout();
    const VkPushConstantRange pushConstants{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DeformConstants)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstants;
    VK_CHECK(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout));

    const VkShaderModule shader = gpu.LoadShaderModule("deform.spv");
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader;
    pipelineInfo.stage.pName = "DeformMain";
    pipelineInfo.layout = m_pipelineLayout;
    const VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
    vkDestroyShaderModule(m_device, shader, nullptr);
    VK_CHECK(result);
}

uint32_t Deformer::VertexCount() const
{
    uint32_t total = 0;
    for (const Target& target : m_targets)
    {
        total += m_assetGpu->PrimitiveFor(target.primitive).vertexCount;
    }
    return total;
}

const Deformer::Target& Deformer::FindTarget(const Gltf::Node* node, const Gltf::Primitive* primitive) const
{
    return FindDeformTarget(m_targets, node, primitive);
}

const Buffer& Deformer::VertexBuffer(const Gltf::Node* node, const Gltf::Primitive* primitive) const
{
    return FindTarget(node, primitive).deformedPositions;
}

uint32_t Deformer::PositionSrv(const Gltf::Node* node, const Gltf::Primitive* primitive) const
{
    return FindTarget(node, primitive).deformedPositionSrv;
}

uint32_t Deformer::AttributeSrv(const Gltf::Node* node, const Gltf::Primitive* primitive) const
{
    return FindTarget(node, primitive).deformedAttributeSrv;
}

void Deformer::Update(Gpu& gpu, VkCommandBuffer commandBuffer, const GltfAsset& asset, const GltfState& state)
{
    if (m_targets.empty())
    {
        return;
    }

    // Indexed by the GPU frame slot, whose previous use Gpu::Begin() already waited for.
    const uint32_t ring = gpu.FrameIndex();
    auto* jointMatrices = static_cast<JointMatrix*>(m_jointMatrices[ring].mapped);
    WriteJointMatrices(asset, state, m_jointLayout, jointMatrices);

    // The previous frame's BLAS builds and rays may still read the deformed buffers.
    Gpu::Barrier(commandBuffer,
                 VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                     VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                 VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    const VkDescriptorSet set = gpu.BindlessSet();
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &set, 0, nullptr);

    for (const Target& target : m_targets)
    {
        const GpuPrimitive& primitive = m_assetGpu->PrimitiveFor(target.primitive);
        const DeformConstants constants{
            ToAccessorInfo(primitive.positions), ToAccessorInfo(primitive.normals), ToAccessorInfo(primitive.uvs),
            ToAccessorInfo(primitive.joints),    ToAccessorInfo(primitive.weights), m_jointMatrixSrv[ring],
            target.deformedPositionUav,          target.deformedAttributeUav,       primitive.vertexCount,
            m_jointLayout.OffsetOf(target.skin)};
        vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                           &constants);
        vkCmdDispatch(commandBuffer, (primitive.vertexCount + kDeformThreadGroupSize - 1) / kDeformThreadGroupSize, 1,
                      1);
    }

    Gpu::Barrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                     VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                 VK_ACCESS_2_SHADER_READ_BIT);
}
} // namespace vulkan
