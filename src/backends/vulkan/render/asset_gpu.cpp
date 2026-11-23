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

#include "backends/vulkan/render/asset_gpu.h"

#include "core/math.h"

#include <limits>

namespace vulkan
{
namespace
{
} // namespace

void AssetGpu::Create(Gpu& gpu, const GltfAsset& asset, bool enableSkinning)
{
    CreateBuffers(gpu, asset);
    CreateTextures(gpu, asset);
    CreateMaterials(gpu, asset);
    BuildMeshes(asset, enableSkinning);
}

void AssetGpu::CreateBuffers(Gpu& gpu, const GltfAsset& asset)
{
    constexpr VkBufferUsageFlags kUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    m_buffers.resize(asset.buffers.size());
    m_bufferSrvs.resize(asset.buffers.size());
    for (size_t i = 0; i < asset.buffers.size(); ++i)
    {
        if (!asset.buffers[i]->pData)
        {
            Fatal("glTF buffer %zu has no loaded data.", i);
        }
        std::vector<uint8_t> bytes = asset.buffers[i]->pData->data;
        bytes.resize(static_cast<size_t>(AlignUp(bytes.size(), sizeof(uint32_t))), 0);
        m_buffers[i] = gpu.CreateDefaultBuffer(bytes.data(), bytes.size(), kUsage, "glTF Buffer");
        m_bufferSrvs[i] = gpu.AddStorageBuffer(Bindless::ByteBuffer, m_buffers[i]);
    }

    const std::vector<AccessorLayout> layouts = BuildAccessorLayouts(asset, m_buffers.size());
    m_accessors.resize(layouts.size());
    for (size_t i = 0; i < layouts.size(); ++i)
    {
        static_cast<AccessorLayout&>(m_accessors[i]) = layouts[i];
        m_accessors[i].bufferSrv = m_bufferSrvs[layouts[i].bufferIndex];
        m_accessors[i].bufferAddress = m_buffers[layouts[i].bufferIndex].Address();
        m_accessorIndices.emplace(asset.accessors[i].get(), i);
    }
}

void AssetGpu::CreateTextures(Gpu& gpu, const GltfAsset& asset)
{
    m_textures.resize(asset.textures.size());
    m_textureSrvs.assign(asset.textures.size(), 0xFFFFFFFFu);

    for (size_t i = 0; i < asset.textures.size(); ++i)
    {
        const Gltf::Image* source = asset.textures[i]->pSource;
        if (!source || !source->pData)
        {
            continue;
        }
        const Gltf::ImageData& image = *source->pData;
        if (image.width <= 0 || image.height <= 0 || image.data.empty())
        {
            Fatal("glTF texture %zu has invalid decoded image data.", i);
        }

        // Base colour textures are authored in sRGB; let the sampler do the conversion.
        m_textures[i] = gpu.CreateImage2D(VK_FORMAT_R8G8B8A8_SRGB, static_cast<uint32_t>(image.width),
                                          static_cast<uint32_t>(image.height),
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "BaseColor");
        gpu.UploadTexture2D(m_textures[i], image.data.data(), Gltf::ImageData::kChannels);
        m_textureSrvs[i] = gpu.AddTexture(m_textures[i].view);
    }
}

void AssetGpu::CreateMaterials(Gpu& gpu, const GltfAsset& asset)
{
    std::vector<MaterialInfo> materials(asset.materials.size() + 1);
    for (size_t i = 0; i < asset.materials.size(); ++i)
    {
        const Gltf::Material& source = *asset.materials[i];
        MaterialInfo& material = materials[i];
        const Gltf::PbrMetallicRoughness& pbr = source.pbrMetallicRoughness;
        material.baseColorFactor = pbr.baseColorFactor;
        material.metallic = pbr.metallicFactor;
        material.roughness = pbr.roughnessFactor;
        const Gltf::Texture* texture = pbr.baseColorTexture.pTexture;
        if (texture)
        {
            const size_t textureIndex = Gltf::PointerIndex(asset.textures, texture, "texture");
            material.baseColorTexture = m_textureSrvs[textureIndex];
        }
    }
    // Trailing entry is the fallback for primitives without a material.
    materials.back() = MaterialInfo{};

    m_materials = gpu.CreateDefaultBuffer(materials.data(), materials.size() * sizeof(MaterialInfo),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "Materials");
    m_materialTableIndex = gpu.AddStorageBuffer(Bindless::MaterialTable, m_materials);
}

} // namespace vulkan
