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
#include "core/error.h"
#include "render/gpu_types.h"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

// Flattening a glTF asset for the GPU is entirely API-neutral: it is arithmetic over accessors
// and buffer views. Only attaching the result to a native buffer handle is backend work, so that
// is all the backend does with what is declared here.

// Bit set in AccessorInfo::format for accessors whose integer components are normalized.
// shaders/common.hlsli tests the same bit when decoding, so the two must agree.
constexpr uint32_t kAccessorNormalizedFlag = 0x10000u;

// Sentinel for "no bindless descriptor", matching AccessorInfo's default bufferIndex.
constexpr uint32_t kInvalidBindlessIndex = 0xFFFFFFFFu;

// One glTF accessor resolved to a flat GPU description. `bufferIndex` selects the uploaded
// buffer; `bufferSrv` is the bindless index the backend assigns for it and is the only field
// BuildAccessorLayouts leaves unset.
struct AccessorLayout
{
    uint32_t bufferIndex = 0;
    uint32_t bufferSrv = kInvalidBindlessIndex;
    uint32_t byteOffset = 0;
    uint32_t byteStride = 0;
    uint32_t count = 0;
    uint32_t componentType = 0;
    uint32_t components = 0;
    bool normalized = false;

    explicit operator bool() const { return bufferSrv != kInvalidBindlessIndex; }
};

uint32_t ComponentSize(Gltf::AccessorComponentType componentType);

// Resolves every accessor in the asset against its buffer view. `bufferCount` is how many
// buffers the backend uploaded, used to reject out-of-range references.
std::vector<AccessorLayout> BuildAccessorLayouts(const GltfAsset& asset, size_t bufferCount);

// Packs the shader-visible view of an accessor.
inline AccessorInfo ToAccessorInfo(const AccessorLayout& accessor)
{
    return {accessor.bufferSrv, accessor.byteOffset, accessor.byteStride,
            accessor.componentType | (accessor.normalized ? kAccessorNormalizedFlag : 0u)};
}

// The templates below are parameterized on the backend's accessor type, which carries the native
// buffer handle. Everything about the shape is API-neutral.

template<typename Accessor>
struct GpuPrimitiveT
{
    Accessor positions;
    Accessor normals;
    Accessor uvs;
    Accessor joints;
    Accessor weights;
    Accessor indices;

    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t materialIndex = 0;

    bool HasSkinData() const { return joints && weights; }
};

template<typename Accessor>
struct GpuMeshT
{
    std::vector<GpuPrimitiveT<Accessor>> primitives;
};

// The part of a GPU asset mirror that involves no graphics API: the accessor table and the
// mesh/primitive tree resolved from it. Backends derive from this and add their own buffers,
// textures and material table.
template<typename Accessor>
class AssetGeometry
{
public:
    using Primitive = GpuPrimitiveT<Accessor>;
    using Mesh = GpuMeshT<Accessor>;

    const Primitive& PrimitiveFor(const Gltf::Primitive* primitive) const
    {
        const auto found = m_primitiveLocations.find(primitive);
        if (found == m_primitiveLocations.end())
        {
            Fatal("Primitive does not belong to this GPU asset.");
        }
        const auto [mesh, index] = found->second;
        return m_meshes[mesh].primitives[index];
    }

protected:
    // Resolves every mesh primitive in the asset to its accessors. Requires m_accessors and
    // m_accessorIndices to be populated first.
    void BuildMeshes(const GltfAsset& asset, bool enableSkinning);

    std::vector<Mesh> m_meshes;
    std::unordered_map<const Gltf::Primitive*, std::pair<size_t, size_t>> m_primitiveLocations;
    std::vector<Accessor> m_accessors;
    std::unordered_map<const Gltf::Accessor*, size_t> m_accessorIndices;
};

template<typename Accessor>
void AssetGeometry<Accessor>::BuildMeshes(const GltfAsset& asset, bool enableSkinning)
{
    // Primitives without a material fall back to a default appended past the authored ones.
    const uint32_t defaultMaterial = static_cast<uint32_t>(asset.materials.size());
    m_meshes.resize(asset.meshes.size());

    for (size_t m = 0; m < asset.meshes.size(); ++m)
    {
        const Gltf::Mesh& sourceMesh = *asset.meshes[m];
        Mesh& mesh = m_meshes[m];
        mesh.primitives.resize(sourceMesh.primitives.size());

        for (size_t p = 0; p < sourceMesh.primitives.size(); ++p)
        {
            const Gltf::Primitive& source = *sourceMesh.primitives[p];
            Primitive& primitive = mesh.primitives[p];
            m_primitiveLocations.emplace(&source, std::pair{m, p});

            auto accessor = [&](const Gltf::Accessor* sourceAccessor, const char* semantic) -> Accessor {
                const auto index = m_accessorIndices.find(sourceAccessor);
                if (index == m_accessorIndices.end())
                {
                    Fatal("Primitive is missing its source %s accessor.", semantic);
                }
                return m_accessors[index->second];
            };
            auto optionalAccessor = [&](const Gltf::Accessor* sourceAccessor, const char* semantic) -> Accessor {
                return sourceAccessor ? accessor(sourceAccessor, semantic) : Accessor{};
            };

            primitive.vertexCount =
                source.pPosition ? static_cast<uint32_t>(accessor(source.pPosition, "POSITION").count) : 0;
            primitive.indexCount =
                source.pIndices ? static_cast<uint32_t>(accessor(source.pIndices, "indices").count) : 0;
            primitive.materialIndex =
                source.pMaterial
                    ? static_cast<uint32_t>(Gltf::PointerIndex(asset.materials, source.pMaterial, "material"))
                    : defaultMaterial;

            primitive.positions = accessor(source.pPosition, "POSITION");
            primitive.normals = optionalAccessor(source.pNormal, "NORMAL");
            primitive.uvs = optionalAccessor(source.pTexcoord0, "TEXCOORD_0");
            primitive.indices = optionalAccessor(source.pIndices, "indices");

            if (!enableSkinning || !source.HasSkinAttributes())
            {
                continue;
            }
            primitive.joints = accessor(source.pJoints0, "JOINTS_0");
            primitive.weights = accessor(source.pWeights0, "WEIGHTS_0");
        }
    }
}
