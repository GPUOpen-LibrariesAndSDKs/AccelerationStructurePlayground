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

#include "assets/gltf_asset.h"
#include "backends/vulkan/core/gpu.h"
#include "render/gpu_types.h"
#include "render/asset_layout.h"

#include <unordered_map>

namespace vulkan
{
struct GpuAccessor : AccessorLayout
{
    VkDeviceAddress bufferAddress = 0;

    VkDeviceAddress Address() const { return bufferAddress + byteOffset; }
};

using GpuPrimitive = GpuPrimitiveT<GpuAccessor>;
using GpuMesh = GpuMeshT<GpuAccessor>;

// GPU mirror of one glTF asset. Source buffers are uploaded once; the accessor table and the
// mesh tree built from it live in the shared AssetGeometry base.
class AssetGpu : public AssetGeometry<GpuAccessor>
{
public:
    // Requires an open command buffer; uploads are recorded into it.
    void Create(Gpu& gpu, const GltfAsset& asset, bool enableSkinning);

    uint32_t MaterialTableIndex() const { return m_materialTableIndex; }

private:
    void CreateBuffers(Gpu& gpu, const GltfAsset& asset);
    void CreateTextures(Gpu& gpu, const GltfAsset& asset);
    void CreateMaterials(Gpu& gpu, const GltfAsset& asset);

    std::vector<Buffer> m_buffers;
    std::vector<uint32_t> m_bufferSrvs;
    std::vector<Image> m_textures;
    std::vector<uint32_t> m_textureSrvs;
    Buffer m_materials;
    uint32_t m_materialTableIndex = 0;
};

} // namespace vulkan
