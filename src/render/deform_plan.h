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
#include "assets/gltf_state.h"
#include "core/error.h"
#include "render/gpu_types.h"
#include "render/asset_layout.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

// The parts of compute-shader vertex deformation that are not graphics API work: which
// primitives need deforming, where each skin's matrices sit in the flat joint buffer, and how
// many thread groups a dispatch needs.

// Must match [numthreads(...)] in shaders/deform.hlsl.
constexpr uint32_t kDeformThreadGroupSize = 64;

constexpr uint32_t DeformGroupCount(uint32_t vertexCount)
{
    return (vertexCount + kDeformThreadGroupSize - 1) / kDeformThreadGroupSize;
}

// Push constants for the deform shader. Must match DeformConstants in shaders/deform.hlsl.
struct DeformConstants
{
    AccessorInfo positions;
    AccessorInfo normals;
    AccessorInfo uvs;
    AccessorInfo joints;
    AccessorInfo weights;
    uint32_t jointMatrixIndex;
    uint32_t outPositionIndex;
    uint32_t outAttributeIndex;
    uint32_t vertexCount;
    uint32_t jointOffset;
};

// Identifies one primitive that the deform pass writes. Backends extend this with the buffers
// the pass writes into.
struct DeformTarget
{
    const Gltf::Node* node = nullptr;
    const Gltf::Primitive* primitive = nullptr;
    const Gltf::Skin* skin = nullptr;
};

// Every skin's joint matrices live in one flat buffer; this records where each one starts.
struct JointMatrixLayout
{
    std::unordered_map<const Gltf::Skin*, uint32_t> skinOffsets;
    uint32_t totalMatrices = 0;

    uint32_t OffsetOf(const Gltf::Skin* skin) const { return skinOffsets.at(skin); }
};

JointMatrixLayout BuildJointMatrixLayout(const GltfAsset& asset);

// Evaluates every skin against the current pose and writes the result into a flat destination
// sized for layout.totalMatrices.
void WriteJointMatrices(const GltfAsset& asset, const GltfState& state, const JointMatrixLayout& layout,
                        JointMatrix* destination);

template<typename Target>
const Target& FindDeformTarget(const std::vector<Target>& targets, const Gltf::Node* node,
                               const Gltf::Primitive* primitive)
{
    for (const Target& target : targets)
    {
        if (target.node == node && target.primitive == primitive)
        {
            return target;
        }
    }
    Fatal("Missing deformation target for the requested node and primitive.");
}
