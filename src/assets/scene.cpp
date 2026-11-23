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

#include "scene.h"

#include "assets/gltf_state.h"

#include <bit>
#include <cmath>
#include <map>
#include <tuple>

namespace
{
float CanonicalPlaybackFloat(float value, const char* name)
{
    if (!std::isfinite(value))
    {
        Gltf::Fatal("Animation %s must be finite.", name);
    }
    return value == 0.0f ? 0.0f : value;
}

float4x4 IdentityTransform()
{
    return float4x4(1.0f);
}

void AddTransformedBounds(Bounds& destination, const Bounds& source, const float4x4& transform)
{
    if (!source.Valid())
    {
        return;
    }
    for (uint32_t corner = 0; corner < 8; ++corner)
    {
        const float x = (corner & 1) ? source.max.x : source.min.x;
        const float y = (corner & 2) ? source.max.y : source.min.y;
        const float z = (corner & 4) ? source.max.z : source.min.z;
        destination.Add(float3(transform * float4(x, y, z, 1.0f)));
    }
}
} // namespace

PlaybackKey PlaybackKey::From(PlaybackParameters parameters)
{
    parameters.speed = CanonicalPlaybackFloat(parameters.speed, "speed");
    parameters.timeOffset = CanonicalPlaybackFloat(parameters.timeOffset, "time offset");
    return {std::bit_cast<uint32_t>(parameters.speed), std::bit_cast<uint32_t>(parameters.timeOffset)};
}

SceneNodeId SceneNode::ParentId() const
{
    return parent.pParentSceneNode ? parent.pParentSceneNode->id : SceneNodeId{UINT32_MAX};
}

Scene::Scene(const std::string& mainScenePath)
{
    std::error_code error;
    const std::string mainCanonicalPath = std::filesystem::weakly_canonical(mainScenePath, error).string();
    if (error)
    {
        Gltf::Fatal("Failed to resolve scene path '%s': %s", mainScenePath.c_str(), error.message().c_str());
    }

    std::vector<std::string> filesToLoad;
    filesToLoad.push_back(mainCanonicalPath);
    while (!filesToLoad.empty())
    {
        std::string filePath = filesToLoad.back();
        filesToLoad.pop_back();
        if (m_assetsByPath.find(filePath) != m_assetsByPath.end())
        {
            // file already loaded
            continue;
        }

        m_assetsByPath[filePath] = std::make_unique<GltfAsset>(GltfAsset::Load(filePath));
        for (const auto& f : m_assetsByPath[filePath]->files)
        {
            if (m_assetsByPath.find(f->canonicalFileName) == m_assetsByPath.end())
            {
                filesToLoad.push_back(f->canonicalFileName);
            }
        }
    }

    // Initialize m_sceneNodes with a node for the top level glTF file
    {
        m_sceneNodes.emplace_back();
        SceneNode& mainSceneNode = m_sceneNodes.back();
        mainSceneNode.id = SceneNodeId{0};
        mainSceneNode.pAsset = m_assetsByPath[mainCanonicalPath].get();
    }

    std::vector<SceneNode*> sceneNodesToPopulateChildren = {&m_sceneNodes[0]};
    while (!sceneNodesToPopulateChildren.empty())
    {
        SceneNode* pCurrentNode = sceneNodesToPopulateChildren.back();
        sceneNodesToPopulateChildren.pop_back();

        const GltfAsset* pAsset = pCurrentNode->pAsset;

        const std::vector<Gltf::Node*>& sceneRoots =
            pAsset->scenes.empty() ? pAsset->GetAllRootNodes() : pAsset->GetSceneRootNodes();
        std::vector<const Gltf::Node*> gltfNodesToVisit;
        gltfNodesToVisit.reserve(sceneRoots.size());
        for (auto root = sceneRoots.rbegin(); root != sceneRoots.rend(); ++root)
        {
            gltfNodesToVisit.push_back(*root);
        }
        while (!gltfNodesToVisit.empty())
        {
            const Gltf::Node* gltfNode = gltfNodesToVisit.back();
            gltfNodesToVisit.pop_back();
            for (auto child = gltfNode->children.rbegin(); child != gltfNode->children.rend(); ++child)
            {
                gltfNodesToVisit.push_back(*child);
            }
            if (!gltfNode->pExternalAsset)
            {
                continue;
            }

            const Gltf::Node* externalAssetNode = gltfNode;
            const Gltf::ExternalAsset& externalAsset = *externalAssetNode->pExternalAsset;
            const std::string& filename = externalAsset.pFile->canonicalFileName;

            for (const SceneNode* ancestor = pCurrentNode; ancestor != nullptr;
                 ancestor = ancestor->parent.pParentSceneNode)
            {
                if (ancestor->pAsset->sourcePath == filename)
                {
                    Gltf::Fatal("External asset cycle detected at '%s'.", filename.c_str());
                }
            }

            m_sceneNodes.emplace_back();
            SceneNode& childNode = m_sceneNodes.back();
            childNode.id = SceneNodeId{static_cast<uint32_t>(m_sceneNodes.size() - 1)};
            childNode.pAsset = m_assetsByPath[filename].get();
            childNode.parent = {pCurrentNode, externalAssetNode};
            childNode.globalPlaybackParameters = externalAssetNode->info.localPlaybackParameters;
            childNode.globalPlaybackParameters.UpdateWithParent(pCurrentNode->globalPlaybackParameters);

            pCurrentNode->children.push_back({&childNode, externalAssetNode});

            sceneNodesToPopulateChildren.push_back(&childNode);
        }
    }

    std::unordered_map<const GltfAsset*, AssetId> assetIds;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, PlaybackGroupId> playbackGroups;
    for (SceneNode& sceneNode : m_sceneNodes)
    {
        auto [assetIt, inserted] =
            assetIds.try_emplace(sceneNode.pAsset, AssetId{static_cast<uint32_t>(m_assets.size())});
        if (inserted)
        {
            m_assets.push_back(sceneNode.pAsset);
        }
        sceneNode.assetId = assetIt->second;

        PlaybackParameters playback = sceneNode.globalPlaybackParameters;
        playback.speed = CanonicalPlaybackFloat(playback.speed, "speed");
        playback.timeOffset = CanonicalPlaybackFloat(playback.timeOffset, "time offset");
        const PlaybackKey key = PlaybackKey::From(playback);
        const auto [groupIt, groupInserted] =
            playbackGroups.try_emplace(std::make_tuple(sceneNode.assetId.value, key.speedBits, key.timeOffsetBits),
                                       PlaybackGroupId{static_cast<uint32_t>(m_playbackGroups.size())});
        if (groupInserted)
        {
            m_playbackGroups.emplace_back(sceneNode.assetId, *sceneNode.pAsset, playback);
        }
        sceneNode.playbackGroup = groupIt->second;
    }
    SetAnimationTime(0.0f);
}

Scene::~Scene() = default;
Scene::Scene(Scene&&) noexcept = default;
Scene& Scene::operator=(Scene&&) noexcept = default;

const GltfState& Scene::State(SceneNodeId id) const
{
    if (id.value >= m_sceneNodes.size())
    {
        Gltf::Fatal("Scene node ID is out of range.");
    }
    return State(&m_sceneNodes[id.value]);
}

const GltfState& Scene::State(const SceneNode* sceneNode) const
{
    // Scene nodes are stored in id order, so a node belongs to this scene exactly when it sits at
    // its own id. That is the whole ownership check the old side table was performing.
    if (!sceneNode || sceneNode->id.value >= m_sceneNodes.size() || &m_sceneNodes[sceneNode->id.value] != sceneNode)
    {
        Gltf::Fatal("Scene node does not belong to this scene.");
    }
    return Group(sceneNode->playbackGroup).State();
}

float4x4 Scene::NodeWorldTransform(const SceneNode* sceneNode, const Gltf::Node* gltfNode) const
{
    return sceneNode->placement * State(sceneNode).Node(gltfNode).world;
}

void Scene::SetAnimationTime(float baseTime)
{
    for (PlaybackGroup& group : m_playbackGroups)
    {
        group.SetAnimationTime(baseTime);
    }
    for (SceneNode& sceneNode : m_sceneNodes)
    {
        if (!sceneNode.parent.pParentSceneNode)
        {
            sceneNode.placement = IdentityTransform();
            continue;
        }
        const SceneNode& parent = *sceneNode.parent.pParentSceneNode;
        const Gltf::Node* anchor = sceneNode.ParentAnchor();
        if (!anchor)
        {
            Gltf::Fatal("Child scene node has no parent anchor.");
        }
        sceneNode.placement = parent.placement * State(&parent).Node(anchor).world;
    }
}

void PlaybackGroup::SetAnimationTime(float baseTime)
{
    m_state.SetAnimationTime(baseTime);
    m_boundsValid = false;
}

const Bounds& PlaybackGroup::LocalBounds() const
{
    if (!m_boundsValid)
    {
        m_localBounds = m_state.LocalBounds();
        m_boundsValid = true;
    }
    return m_localBounds;
}

Bounds Scene::WorldBounds() const
{
    Bounds bounds;
    for (const SceneNode& sceneNode : m_sceneNodes)
    {
        AddTransformedBounds(bounds, Group(sceneNode.playbackGroup).LocalBounds(), sceneNode.placement);
    }
    return bounds;
}

const GltfAsset& Scene::Asset(AssetId id) const
{
    if (id.value >= m_assets.size())
    {
        Gltf::Fatal("Scene asset ID is out of range.");
    }
    return *m_assets[id.value];
}

const PlaybackGroup& Scene::Group(PlaybackGroupId id) const
{
    if (id.value >= m_playbackGroups.size())
    {
        Gltf::Fatal("Playback group ID is out of range.");
    }
    return m_playbackGroups[id.value];
}

PlaybackGroup& Scene::Group(PlaybackGroupId id)
{
    if (id.value >= m_playbackGroups.size())
    {
        Gltf::Fatal("Playback group ID is out of range.");
    }
    return m_playbackGroups[id.value];
}
