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

#include "render/deform_plan.h"

#include "assets/gltf_state.h"
#include "backends/vulkan/core/gpu.h"
#include "backends/vulkan/render/asset_gpu.h"

namespace vulkan
{
// Skinned animations using compute shaders
class Deformer
{
public:
    Deformer() = default;
    Deformer(const Deformer&) = delete;
    Deformer& operator=(const Deformer&) = delete;
    Deformer(Deformer&& other) noexcept { *this = std::move(other); }
    Deformer& operator=(Deformer&& other) noexcept;
    ~Deformer() { DestroyPipeline(); }

    void Create(Gpu& gpu, const GltfAsset& asset, const AssetGpu& assetGpu);
    // Uploads joint matrices and dispatches the deformation. Requires an open command buffer.
    void Update(Gpu& gpu, VkCommandBuffer commandBuffer, const GltfAsset& asset, const GltfState& state);

    bool Active() const { return !m_targets.empty(); }
    uint32_t PrimitiveCount() const { return static_cast<uint32_t>(m_targets.size()); }
    uint32_t VertexCount() const;

    const Buffer& VertexBuffer(const Gltf::Node* node, const Gltf::Primitive* primitive) const;
    uint32_t PositionSrv(const Gltf::Node* node, const Gltf::Primitive* primitive) const;
    uint32_t AttributeSrv(const Gltf::Node* node, const Gltf::Primitive* primitive) const;

private:
    struct Target : DeformTarget
    {

        Buffer deformedPositions;
        Buffer deformedAttributes;
        uint32_t deformedPositionUav = 0;
        uint32_t deformedPositionSrv = 0;
        uint32_t deformedAttributeUav = 0;
        uint32_t deformedAttributeSrv = 0;
    };

    const Target& FindTarget(const Gltf::Node* node, const Gltf::Primitive* primitive) const;
    void DestroyPipeline();

    const AssetGpu* m_assetGpu = nullptr;
    VkDevice m_device = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    std::vector<Target> m_targets;
    JointMatrixLayout m_jointLayout;

    Buffer m_jointMatrices[Gpu::kFrameCount];
    uint32_t m_jointMatrixSrv[Gpu::kFrameCount]{};
};
} // namespace vulkan
