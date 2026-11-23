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

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/gltf_state.h"

struct SceneNode;

struct AssetId
{
    uint32_t value = 0;
    bool operator==(const AssetId&) const = default;
};

struct PlaybackGroupId
{
    uint32_t value = 0;
    bool operator==(const PlaybackGroupId&) const = default;
};

struct SceneNodeId
{
    uint32_t value = 0;
    bool operator==(const SceneNodeId&) const = default;
};

using PlaybackParameters = Gltf::PlaybackParameters;

struct PlaybackKey
{
    uint32_t speedBits = 0;
    uint32_t timeOffsetBits = 0;

    static PlaybackKey From(PlaybackParameters parameters);
    bool operator==(const PlaybackKey&) const = default;
};

// Every scene node that includes the same asset at the same speed and offset poses identically,
// so the group owns the one state they all read instead of each node keeping its own copy.
struct PlaybackGroup
{
    PlaybackGroup(AssetId id, const GltfAsset& asset, PlaybackParameters playback)
        : assetId(id), parameters(playback), m_state(asset, playback)
    {
    }

    AssetId assetId;
    PlaybackParameters parameters;

    const GltfState& State() const { return m_state; }
    void SetAnimationTime(float baseTime);

    // Bounds of the group's state in its own space, computed on demand. Evaluating them walks
    // every vertex of every primitive, and nothing in the render loop reads them, so they are
    // only recomputed when something actually asks after the pose changed.
    const Bounds& LocalBounds() const;

private:
    GltfState m_state;
    mutable Bounds m_localBounds;
    mutable bool m_boundsValid = false;
};

struct ParentSceneNodeReference
{
    SceneNode* pParentSceneNode = nullptr;
    const Gltf::Node* gltfNodeInParent = nullptr;
};

struct ChildSceneNodeReference
{
    SceneNode* pChildSceneNode = nullptr;
    const Gltf::Node* gltfNodeInParent = nullptr;
};

// One placement of an asset in the scene graph. Including the same file twice gives two scene
// nodes sharing one asset.
struct SceneNode
{
    // Both name the same asset. The pointer is set while the graph is walked; the id only
    // exists after the walk, when repeated files have been deduplicated into Scene::Assets().
    const GltfAsset* pAsset = nullptr;
    AssetId assetId;
    PlaybackGroupId playbackGroup;
    SceneNodeId id;
    float4x4 placement{};

    ParentSceneNodeReference parent;
    std::vector<ChildSceneNodeReference> children;

    Gltf::PlaybackParameters globalPlaybackParameters{};

    bool IsExternalAsset() const { return parent.pParentSceneNode != nullptr; }
    SceneNodeId ParentId() const;
    const Gltf::Node* ParentAnchor() const { return parent.gltfNodeInParent; }
};

// This class contains information about the entire scene
class Scene
{
public:
    // Constructs scene by loading all included glTF files
    Scene(const std::string& mainScenePath);
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) noexcept;
    Scene& operator=(Scene&&) noexcept;

    // Scene nodes are stored in id order, so SceneNodes()[id.value] is the node with that id and
    // SceneNodes().front() is the root. There is deliberately no second, bounds-checked accessor:
    // one existed, 37 call sites indexed the container directly anyway, and having both invited
    // the question of which one to reach for.
    const std::deque<SceneNode>& SceneNodes() const { return m_sceneNodes; }
    const GltfState& State(SceneNodeId id) const;
    const GltfState& State(const SceneNode* sceneNode) const;
    float4x4 NodeWorldTransform(const SceneNode* sceneNode, const Gltf::Node* gltfNode) const;
    void SetAnimationTime(float baseTime);
    Bounds WorldBounds() const;

    const GltfAsset& Asset(AssetId id) const;
    const std::vector<const GltfAsset*>& Assets() const { return m_assets; }
    const PlaybackGroup& Group(PlaybackGroupId id) const;
    PlaybackGroup& Group(PlaybackGroupId id);
    const std::vector<PlaybackGroup>& PlaybackGroups() const { return m_playbackGroups; }

private:
    // Owns every loaded asset, keyed by canonical path so that a file referenced from several
    // places is loaded once. m_assets indexes the same objects by AssetId.
    std::unordered_map<std::string, std::unique_ptr<GltfAsset>> m_assetsByPath;

    std::deque<SceneNode> m_sceneNodes;
    std::vector<const GltfAsset*> m_assets;
    std::vector<PlaybackGroup> m_playbackGroups;
};
