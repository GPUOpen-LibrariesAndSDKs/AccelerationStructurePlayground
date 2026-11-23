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

#include <unordered_map>

namespace Gltf
{

struct NodePose
{
    float3 translation{0.0f, 0.0f, 0.0f};
    float4 rotation{0.0f, 0.0f, 0.0f, 1.0f};
    float3 scale{1.0f, 1.0f, 1.0f};
    float4x4 world{};
};

} // namespace Gltf

// Poses for one glTF asset played back at one speed and time offset. The pose of a node depends
// only on those three things, never on where the asset sits in the scene graph, so several scene
// nodes that agree on them share a single state.
class GltfState
{
public:
    GltfState(const GltfAsset& asset, Gltf::PlaybackParameters playback);

    const GltfAsset& Asset() const { return *m_pAsset; }
    const Gltf::NodePose& Node(const Gltf::Node* node) const;
    const Gltf::NodePose& Node(size_t index) const;
    const std::vector<Gltf::NodePose>& Nodes() const { return m_nodes; }

    void SetAnimationTime(float baseTime);
    void ComputeJointMatrices(const Gltf::Skin* skin, std::vector<float4x4>& out) const;
    Bounds LocalBounds() const;

private:
    void ResetPose();
    void UpdateWorldTransforms();
    size_t NodeIndex(const Gltf::Node* node) const;

    const GltfAsset* m_pAsset = nullptr;
    Gltf::PlaybackParameters m_playback{};
    std::vector<Gltf::NodePose> m_nodes;
    std::unordered_map<const Gltf::Node*, size_t> m_nodeIndices;
};
