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

#include "assets/gltf_state.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Gltf
{
namespace
{

// Animation values are carried as a float4 regardless of path so that the sampling code below is
// shared; the translation and scale paths simply ignore w.
float4 LoadOutputValue(const GltfAsset& asset, const Accessor* accessor, size_t index, size_t components)
{
    if (!accessor || accessor->componentType != AccessorComponentType::Float)
    {
        Fatal("Animation output accessor must contain floating-point values.");
    }
    if (components == 3 && accessor->type == AccessorType::Vec3)
    {
        const AccessorView<float3> values = asset.GetAccessorView<float3>(accessor);
        if (index >= values.GetSize())
        {
            Fatal("Animation output accessor is too short.");
        }
        return float4(values[index], 0.0f);
    }
    if (components == 4 && accessor->type == AccessorType::Vec4)
    {
        const AccessorView<float4> values = asset.GetAccessorView<float4>(accessor);
        if (index >= values.GetSize())
        {
            Fatal("Animation output accessor is too short.");
        }
        return values[index];
    }
    Fatal("Animation output accessor has an incompatible type.");
}

// glTF stores quaternions xyzw; glm::quat's scalar-first constructor takes w first.
glm::quat AsQuat(const float4& xyzw)
{
    return glm::quat(xyzw.w, xyzw.x, xyzw.y, xyzw.z);
}

float4 AsFloat4(const glm::quat& q)
{
    return float4(q.x, q.y, q.z, q.w);
}

struct KeyFrame
{
    size_t first = 0;
    size_t second = 0;
    float blend = 0.0f;
    float deltaTime = 0.0f;
};

KeyFrame FindKeyFrame(const AccessorView<float>& times, float time)
{
    KeyFrame key;
    const size_t count = times.GetSize();
    if (count < 2)
    {
        return key;
    }
    if (time < times[0])
    {
        return key;
    }
    if (time >= times[count - 1])
    {
        key.first = key.second = count - 1;
        return key;
    }

    // First index whose time is greater than `time`. The keyframe times are validated finite
    // and ascending when the asset loads, so a plain binary search is enough here.
    size_t low = 0;
    size_t high = count;
    while (low < high)
    {
        const size_t mid = low + (high - low) / 2;
        if (times[mid] > time)
        {
            high = mid;
        }
        else
        {
            low = mid + 1;
        }
    }

    key.second = low;
    key.first = key.second - 1;
    key.deltaTime = times[key.second] - times[key.first];
    key.blend = key.deltaTime > 0.0f ? (time - times[key.first]) / key.deltaTime : 0.0f;
    return key;
}

float4 SampleAnimation(const GltfAsset& asset, const AnimationSampler& sampler, float time, AnimationPath path)
{
    if (!sampler.pInput || !sampler.pOutput || sampler.pInput->componentType != AccessorComponentType::Float ||
        sampler.pInput->type != AccessorType::Scalar)
    {
        Fatal("Animation sampler requires scalar float input and a valid output accessor.");
    }

    const AccessorView<float> times = asset.GetAccessorView<float>(sampler.pInput);
    if (times.GetSize() == 0)
    {
        return path == AnimationPath::Rotation ? float4(0.0f, 0.0f, 0.0f, 1.0f) : float4(0.0f);
    }

    const size_t components = path == AnimationPath::Rotation ? 4 : 3;
    const KeyFrame key = FindKeyFrame(times, time);
    const size_t valueSlot = sampler.interpolation == Interpolation::CubicSpline ? 1 : 0;
    const size_t stride = sampler.interpolation == Interpolation::CubicSpline ? 3 : 1;
    const auto value = [&](size_t frame, size_t slot) {
        return LoadOutputValue(asset, sampler.pOutput, frame * stride + slot, components);
    };

    switch (sampler.interpolation)
    {
    case Interpolation::Step:
        return value(key.first, 0);

    case Interpolation::CubicSpline: {
        const float4 value0 = value(key.first, valueSlot);
        if (key.first == key.second)
        {
            return value0;
        }
        const float t = key.blend;
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float4 result = (value0 * (2.0f * t3 - 3.0f * t2 + 1.0f) +
                               value(key.first, 2) * (key.deltaTime * (t3 - 2.0f * t2 + t))) +
                              (value(key.second, valueSlot) * (-2.0f * t3 + 3.0f * t2) +
                               value(key.second, 0) * (key.deltaTime * (t3 - t2)));
        return path == AnimationPath::Rotation ? Normalize(result) : result;
    }

    default: {
        const float4 first = value(key.first, 0);
        if (key.first == key.second)
        {
            return first;
        }
        const float4 second = value(key.second, 0);
        return path == AnimationPath::Rotation
                   ? AsFloat4(glm::slerp(AsQuat(first), AsQuat(second), key.blend))
                   : first + (second - first) * key.blend;
    }
    }
}

const uint8_t* AccessorElement(const Accessor& accessor, size_t index)
{
    if (!accessor.pBufferView || !accessor.pBufferView->pBuffer || !accessor.pBufferView->pBuffer->pData ||
        index >= accessor.count)
    {
        Fatal("Skin accessor has no loaded data or an invalid element index.");
    }
    const BufferView& view = *accessor.pBufferView;
    const size_t componentSize = GetAccessorComponentTypeSize(accessor.componentType);
    const size_t elementSize = componentSize * GetAccessorTypeSize(accessor.type);
    const size_t stride = view.byteStride ? view.byteStride : elementSize;
    return view.pBuffer->pData->data.data() + view.byteOffset + accessor.byteOffset + index * stride;
}

uint32_t ReadJointIndex(const Accessor& accessor, size_t vertex, size_t component)
{
    if (accessor.type != AccessorType::Vec4 || component >= 4)
    {
        Fatal("JOINTS_0 accessor must be VEC4.");
    }
    const uint8_t* data =
        AccessorElement(accessor, vertex) + component * GetAccessorComponentTypeSize(accessor.componentType);
    switch (accessor.componentType)
    {
    case AccessorComponentType::UnsignedByte:
        return *data;
    case AccessorComponentType::UnsignedShort: {
        uint16_t value;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }
    default:
        Fatal("JOINTS_0 must use unsigned byte or unsigned short components.");
    }
}

float ReadJointWeight(const Accessor& accessor, size_t vertex, size_t component)
{
    if (accessor.type != AccessorType::Vec4 || component >= 4)
    {
        Fatal("WEIGHTS_0 accessor must be VEC4.");
    }
    const uint8_t* data =
        AccessorElement(accessor, vertex) + component * GetAccessorComponentTypeSize(accessor.componentType);
    switch (accessor.componentType)
    {
    case AccessorComponentType::UnsignedByte:
        return accessor.normalized ? static_cast<float>(*data) / 255.0f : static_cast<float>(*data);
    case AccessorComponentType::UnsignedShort: {
        uint16_t value;
        std::memcpy(&value, data, sizeof(value));
        return accessor.normalized ? static_cast<float>(value) / 65535.0f : static_cast<float>(value);
    }
    case AccessorComponentType::Float: {
        float value;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }
    default:
        Fatal("WEIGHTS_0 must use float, unsigned byte, or unsigned short components.");
    }
}

} // namespace
} // namespace Gltf

GltfState::GltfState(const GltfAsset& asset, Gltf::PlaybackParameters playback)
    : m_pAsset(&asset), m_playback(playback)
{
    m_nodes.resize(asset.nodes.size());
    for (size_t i = 0; i < asset.nodes.size(); ++i)
    {
        m_nodeIndices.emplace(asset.nodes[i].get(), i);
    }
    ResetPose();
    UpdateWorldTransforms();
}

size_t GltfState::NodeIndex(const Gltf::Node* node) const
{
    const auto index = m_nodeIndices.find(node);
    if (index == m_nodeIndices.end())
    {
        Gltf::Fatal("Node does not belong to this glTF instance.");
    }
    return index->second;
}

const Gltf::NodePose& GltfState::Node(const Gltf::Node* node) const
{
    return m_nodes[NodeIndex(node)];
}

const Gltf::NodePose& GltfState::Node(size_t index) const
{
    if (index >= m_nodes.size())
    {
        Gltf::Fatal("Node index is outside this glTF instance.");
    }
    return m_nodes[index];
}

void GltfState::ResetPose()
{
    const GltfAsset& asset = *m_pAsset;
    for (const auto& node : asset.nodes)
    {
        const Gltf::Node& source = *node;
        Gltf::NodePose& pose = m_nodes[NodeIndex(&source)];
        pose.translation = source.translation;
        pose.rotation = source.rotation;
        pose.scale = source.scale;
    }
}

void GltfState::UpdateWorldTransforms()
{
    const GltfAsset& asset = *m_pAsset;
    std::vector<const Gltf::Node*> stack;
    for (const Gltf::Node* root : asset.GetAllRootNodes())
    {
        stack.push_back(root);
    }

    std::vector<bool> visited(m_nodes.size(), false);
    while (!stack.empty())
    {
        const Gltf::Node* source = stack.back();
        stack.pop_back();
        const size_t index = NodeIndex(source);
        Gltf::NodePose& pose = m_nodes[index];
        const float4x4 local = ComposeTrs(pose.translation, pose.rotation, pose.scale);
        const Gltf::Node* parent = source->info.pParent;
        pose.world = parent ? m_nodes[NodeIndex(parent)].world * local : local;
        visited[index] = true;
        stack.insert(stack.end(), source->children.begin(), source->children.end());
    }

    for (size_t index = 0; index < visited.size(); ++index)
    {
        if (!visited[index])
        {
            Gltf::Fatal("Unreachable glTF node in loaded hierarchy.");
        }
    }
}

void GltfState::SetAnimationTime(float baseTime)
{
    if (!std::isfinite(baseTime) || !std::isfinite(m_playback.speed) || !std::isfinite(m_playback.timeOffset))
    {
        Gltf::Fatal("Animation time and playback parameters must be finite.");
    }

    ResetPose();
    const GltfAsset& asset = *m_pAsset;
    for (const auto& animationPointer : asset.animations)
    {
        const Gltf::Animation& animation = *animationPointer;
        for (const auto& channelPointer : animation.channels)
        {
            const Gltf::AnimationChannel& channel = *channelPointer;
            if (!channel.pNode || !channel.pSampler)
            {
                continue;
            }
            Gltf::PlaybackParameters playback = channel.pNode->info.localPlaybackParameters;
            playback.UpdateWithParent(m_playback);
            const float channelTime = baseTime * playback.speed + playback.timeOffset;
            if (!std::isfinite(playback.speed) || !std::isfinite(playback.timeOffset) || !std::isfinite(channelTime))
            {
                Gltf::Fatal("Animation time and playback parameters must be finite.");
            }
            const Gltf::AnimationSampler& sampler = *channel.pSampler;
            const Gltf::AccessorView<float> input = asset.GetAccessorView<float>(sampler.pInput);
            float localTime = channelTime;
            if (input.GetSize() > 1)
            {
                const float start = input[0];
                const float end = input[input.GetSize() - 1];
                const float span = end - start;
                if (span > 0.0f)
                {
                    localTime = start + std::fmod(std::fmod(channelTime - start, span) + span, span);
                }
                else
                {
                    localTime = start;
                }
            }

            Gltf::NodePose& pose = m_nodes[NodeIndex(channel.pNode)];
            const float4 value = Gltf::SampleAnimation(asset, sampler, localTime, channel.path);
            switch (channel.path)
            {
            case Gltf::AnimationPath::Translation:
                pose.translation = float3(value);
                break;
            case Gltf::AnimationPath::Rotation:
                pose.rotation = Normalize(value);
                break;
            case Gltf::AnimationPath::Scale:
                pose.scale = float3(value);
                break;
            }
        }
    }
    UpdateWorldTransforms();
}

void GltfState::ComputeJointMatrices(const Gltf::Skin* skin, std::vector<float4x4>& out) const
{
    if (!skin)
    {
        Gltf::Fatal("Cannot compute matrices for a null skin.");
    }
    out.resize(skin->joints.size());
    for (size_t i = 0; i < skin->joints.size(); ++i)
    {
        float4x4 inverseBind(1.0f);
        if (skin->pInverseBindMatrices)
        {
            const Gltf::AccessorView<float4x4> inverseBindMatrices =
                m_pAsset->GetAccessorView<float4x4>(skin->pInverseBindMatrices);
            if (i >= inverseBindMatrices.GetSize())
            {
                Gltf::Fatal("Skin inverse bind matrix accessor is too short.");
            }
            inverseBind = inverseBindMatrices[i];
        }
        // The glTF skinning formula, verbatim: jointMatrix = jointGlobal * inverseBindMatrix.
        out[i] = Node(skin->joints[i]).world * inverseBind;
    }
}

Bounds GltfState::LocalBounds() const
{
    Bounds bounds;
    const GltfAsset& asset = *m_pAsset;
    const std::vector<Gltf::Node*>& roots = asset.scenes.empty() ? asset.GetAllRootNodes() : asset.GetSceneRootNodes();
    std::vector<const Gltf::Node*> stack(roots.begin(), roots.end());
    while (!stack.empty())
    {
        const Gltf::Node* node = stack.back();
        stack.pop_back();
        if (node->pMesh)
        {
            const float4x4 world = Node(node).world;
            std::vector<float4x4> jointMatrices;
            if (node->pSkin)
            {
                ComputeJointMatrices(node->pSkin, jointMatrices);
            }
            for (const auto& primitive : node->pMesh->primitives)
            {
                if (!primitive->pPosition)
                {
                    continue;
                }
                const Gltf::AccessorView<float3> positions = asset.GetAccessorView<float3>(primitive->pPosition);
                for (size_t i = 0; i < positions.GetSize(); ++i)
                {
                    float3 position = positions[i];
                    if (node->pSkin && primitive->pJoints0 && primitive->pWeights0)
                    {
                        float3 skinnedPosition(0.0f);
                        float totalWeight = 0.0f;
                        for (size_t influence = 0; influence < 4; ++influence)
                        {
                            const float weight = Gltf::ReadJointWeight(*primitive->pWeights0, i, influence);
                            if (weight <= 0.0f)
                            {
                                continue;
                            }
                            const uint32_t joint = Gltf::ReadJointIndex(*primitive->pJoints0, i, influence);
                            if (joint >= jointMatrices.size())
                            {
                                Gltf::Fatal("Vertex references a joint outside its skin.");
                            }
                            skinnedPosition += float3(jointMatrices[joint] * float4(position, 1.0f)) * weight;
                            totalWeight += weight;
                        }
                        position = totalWeight > 0.0f ? skinnedPosition * (1.0f / totalWeight) : position;
                    }
                    else if (!node->pSkin)
                    {
                        position = float3(world * float4(position, 1.0f));
                    }
                    bounds.Add(position);
                }
            }
        }
        stack.insert(stack.end(), node->children.begin(), node->children.end());
    }
    return bounds;
}
