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

#include "render/accel_struct_plan.h"

#include "core/error.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>

namespace
{

// Which policy produced a BLAS group. Two policies can arrive at the same asset, mesh and skin
// while meaning different things, so the policy is part of the key that separates their buckets.
// The values are arbitrary; only their relative order matters, since it feeds operator< below.
enum class BlasGroupingPolicy : uint32_t
{
    RigidPerMesh,    // one BLAS per mesh, positioned by TLAS instances
    SkinnedPerSkin,  // one BLAS per mesh and skin pair
    SkinnedPerAsset, // hybrid grouping: all skinned geometry of an asset shares a BLAS
};

struct BlasGroupKey
{
    BlasGroupingPolicy policy = BlasGroupingPolicy::RigidPerMesh;
    // AssetId and PlaybackGroupId values rather than the ids themselves, so the whole key is one
    // comparable tuple. UINT32_MAX means "this policy does not distinguish by playback group".
    uint32_t assetId = 0;
    uint32_t playbackGroup = UINT32_MAX;
    const Gltf::Mesh* pMesh = nullptr;
    const Gltf::Skin* pSkin = nullptr;

    bool operator<(const BlasGroupKey& other) const
    {
        return std::tie(policy, assetId, playbackGroup, pMesh, pSkin) <
               std::tie(other.policy, other.assetId, other.playbackGroup, other.pMesh, other.pSkin);
    }
};

// Marks an entry of SceneNodeHierarchy::heights that has not been computed yet.
constexpr uint32_t kHeightNotComputed = UINT32_MAX;

// The scene-node hierarchy, which scene grouping walks to decide where BLAS and MLAS nodes
// are anchored. Only external-asset nodes have a parent; everything else is a root.
struct SceneNodeHierarchy
{
    std::vector<std::vector<SceneNodeId>> sceneNodes;
    std::vector<uint32_t> rootDepth; // distance from this node's root
    std::vector<uint32_t> roots;
    std::vector<uint32_t> heights; // distance to this node's deepest descendant

    bool Selects(uint32_t index, SceneLevel level) const
    {
        switch (level.origin)
        {
        case SceneLevelOrigin::FromLeaves:
            return heights[index] == level.distance;
        case SceneLevelOrigin::FromRoot:
            return rootDepth[index] == level.distance;
        }
        return false;
    }
};

// Distance from `index` down to its deepest descendant, memoized into hierarchy.heights.
uint32_t ComputeSceneNodeHeight(SceneNodeHierarchy& hierarchy, uint32_t index)
{
    uint32_t& result = hierarchy.heights[index];
    if (result != kHeightNotComputed)
    {
        return result;
    }
    result = 0;
    for (SceneNodeId sceneNode : hierarchy.sceneNodes[index])
    {
        result = std::max(result, ComputeSceneNodeHeight(hierarchy, sceneNode.value) + 1);
    }
    return result;
}

SceneNodeHierarchy BuildSceneNodeHierarchy(const Scene& scene)
{
    const uint32_t nodeCount = static_cast<uint32_t>(scene.SceneNodes().size());
    SceneNodeHierarchy hierarchy;
    hierarchy.sceneNodes.resize(nodeCount);
    hierarchy.rootDepth.assign(nodeCount, 0);
    hierarchy.heights.assign(nodeCount, kHeightNotComputed);

    for (uint32_t index = 0; index < nodeCount; ++index)
    {
        const SceneNode& sceneNode = scene.SceneNodes()[index];
        if (sceneNode.IsExternalAsset() && sceneNode.ParentId().value < nodeCount)
        {
            hierarchy.sceneNodes[sceneNode.ParentId().value].push_back(SceneNodeId{index});
            hierarchy.rootDepth[index] = hierarchy.rootDepth[sceneNode.ParentId().value] + 1;
        }
        else
        {
            hierarchy.roots.push_back(index);
        }
    }

    for (uint32_t root : hierarchy.roots)
    {
        ComputeSceneNodeHeight(hierarchy, root);
    }
    return hierarchy;
}

// Which BLAS a mesh node's geometry goes into, for the per-asset strategies (Meshes and Hybrid).
// Rigid meshes share one BLAS per mesh across every node and placement, since TLAS instances
// position them. Skinned meshes are posed per playback group, so their BLASes are per group too.
BlasGroupKey AssetBlasGroupKey(AccelStructGroupingTarget target, AssetId assetId, PlaybackGroupId playbackGroup,
                               const Gltf::Node& node)
{
    if (!node.pSkin)
    {
        return {BlasGroupingPolicy::RigidPerMesh, assetId.value, UINT32_MAX, node.pMesh};
    }
    if (target == AccelStructGroupingTarget::Hybrid)
    {
        return {BlasGroupingPolicy::SkinnedPerAsset, assetId.value, playbackGroup.value};
    }
    return {BlasGroupingPolicy::SkinnedPerSkin, assetId.value, playbackGroup.value, node.pMesh, node.pSkin};
}

// The node whose transform positions the TLAS instance of a mesh node's BLAS. Skinned geometry
// is already in its final space after deformation, so it is instanced with identity.
const Gltf::Node* AssetBlasInstanceNode(const Gltf::Node& node)
{
    return node.pSkin ? nullptr : &node;
}

// Memoized into `depths`, where 0 means "not computed yet"; a real depth is always at least 1.
uint32_t MlasDepth(const AccelStructPlan& plan, uint32_t index, std::vector<uint32_t>& depths)
{
    if (depths[index] == 0)
    {
        uint32_t depth = 1;
        for (const AccelStructInstancePlan& instance : plan.mlases[index].instances)
        {
            if (instance.accelStruct.kind == AccelStructRefKind::Mlas)
            {
                depth = std::max(depth, 1 + MlasDepth(plan, instance.accelStruct.value, depths));
            }
        }
        depths[index] = depth;
    }
    return depths[index];
}

// Two MLASes are interchangeable when they play back together and hold the same children under
// the same transforms, so that is exactly what the key records. Instances are sorted before
// comparison, because the order they were appended in carries no meaning.
using MlasTransformKey = std::pair<uint32_t, const Gltf::Node*>;
using MlasInstanceKey =
    std::tuple<AccelStructRefKind, uint32_t, const Gltf::Node*, uint32_t, std::vector<MlasTransformKey>>;
using MlasContentsKey = std::pair<uint32_t, std::vector<MlasInstanceKey>>;

// Working state of the deduplication walk below.
struct MlasDeduplication
{
    std::map<MlasContentsKey, uint32_t> byContents;
    std::vector<MlasPlan> unique;
    std::vector<uint32_t> remap; // old MLAS index -> new one; UINT32_MAX until canonicalized
};

// Rewrites one acceleration structure reference to point into the deduplicated list,
// canonicalizing everything beneath it first: two MLASes can only be recognized as equal once
// their own children have been reduced to canonical indices.
AccelStructRef CanonicalizeAccelStructRef(const AccelStructPlan& plan, AccelStructRef accelStruct,
                                          MlasDeduplication& state)
{
    if (accelStruct.kind != AccelStructRefKind::Mlas)
    {
        return accelStruct;
    }
    if (accelStruct.value >= plan.mlases.size())
    {
        Fatal("Cannot deduplicate an invalid MLAS child reference.");
    }
    if (state.remap[accelStruct.value] == UINT32_MAX)
    {
        MlasPlan mlas = plan.mlases[accelStruct.value];
        std::vector<MlasInstanceKey> instances;
        instances.reserve(mlas.instances.size());
        for (AccelStructInstancePlan& instance : mlas.instances)
        {
            instance.accelStruct = CanonicalizeAccelStructRef(plan, instance.accelStruct, state);
            std::vector<MlasTransformKey> path;
            path.reserve(instance.transformPath.size());
            for (const AccelStructTransformStep& step : instance.transformPath)
            {
                path.emplace_back(step.playbackGroup.value, step.pGltfNode);
            }
            instances.emplace_back(instance.accelStruct.kind, instance.accelStruct.value, instance.pGltfNode,
                                   instance.playbackGroup.value, std::move(path));
        }
        std::sort(instances.begin(), instances.end());
        MlasContentsKey key{mlas.playbackGroup.value, std::move(instances)};
        const auto [entry, created] =
            state.byContents.try_emplace(std::move(key), static_cast<uint32_t>(state.unique.size()));
        if (created)
        {
            state.unique.push_back(std::move(mlas));
        }
        state.remap[accelStruct.value] = entry->second;
    }
    return AccelStructRef::OfMlas(MlasPlanId{state.remap[accelStruct.value]});
}

// Collapses MLASes with identical contents onto one, so a repeated subtree is built once and
// instanced several times rather than rebuilt per placement.
void DeduplicateMlases(AccelStructPlan& plan)
{
    MlasDeduplication state;
    state.remap.assign(plan.mlases.size(), UINT32_MAX);

    for (AccelStructInstancePlan& instance : plan.instances)
    {
        instance.accelStruct = CanonicalizeAccelStructRef(plan, instance.accelStruct, state);
    }
    plan.mlases = std::move(state.unique);
}

// Everything the recursive walk in ApplySceneMlasGrouping needs. Gathered into one struct
// so the walk can be an ordinary function with a name, rather than a closure over eight locals.
struct SceneMlasGrouping
{
    AccelStructPlan& plan;
    const Scene& scene;
    const SceneNodeHierarchy& hierarchy;
    // Instances anchored directly at each scene node, indexed by SceneNodeId.
    const std::vector<std::vector<AccelStructInstancePlan>>& directInstances;
    SceneLevel level;

    bool Selects(uint32_t index) const { return hierarchy.Selects(index, level); }

    // Appends a new MLAS holding `instances`, owned by `ownerId`, and returns a reference to it.
    MlasPlanId AddMlas(std::vector<AccelStructInstancePlan> instances, SceneNodeId ownerId)
    {
        MlasPlan mlas;
        mlas.instances = std::move(instances);
        mlas.playbackGroup = scene.SceneNodes()[ownerId.value].playbackGroup;
        mlas.ownerSceneNode = ownerId;
        const MlasPlanId mlasId{static_cast<uint32_t>(plan.mlases.size())};
        plan.mlases.push_back(std::move(mlas));
        return mlasId;
    }
};

// Everything that belongs under one scene node: its own instances, plus each child subtree
// either wrapped in an MLAS of its own (when the grouping level selects that child) or folded
// into this level with the child's anchor appended to each instance's transform path.
std::vector<AccelStructInstancePlan> BuildSceneNodeContents(SceneMlasGrouping& grouping, SceneNodeId sceneNodeId)
{
    const SceneNode& sceneNode = grouping.scene.SceneNodes()[sceneNodeId.value];
    std::vector<AccelStructInstancePlan> contents = grouping.directInstances[sceneNodeId.value];
    for (SceneNodeId otherSceneNodeId : grouping.hierarchy.sceneNodes[sceneNodeId.value])
    {
        const SceneNode& otherSceneNode = grouping.scene.SceneNodes()[otherSceneNodeId.value];
        std::vector<AccelStructInstancePlan> instances = BuildSceneNodeContents(grouping, otherSceneNodeId);
        if (grouping.Selects(otherSceneNodeId.value) && !instances.empty())
        {
            const MlasPlanId mlasId = grouping.AddMlas(std::move(instances), otherSceneNodeId);
            contents.push_back(
                {AccelStructRef::OfMlas(mlasId), otherSceneNode.ParentAnchor(), otherSceneNodeId, sceneNode.playbackGroup});
        }
        else
        {
            for (AccelStructInstancePlan& instance : instances)
            {
                instance.transformPath.push_back({sceneNode.playbackGroup, otherSceneNode.ParentAnchor()});
                contents.push_back(std::move(instance));
            }
        }
    }
    return contents;
}

void ApplySceneMlasGrouping(AccelStructPlan& plan, const Scene& scene, SceneLevel level)
{
    std::vector<std::vector<AccelStructInstancePlan>> directInstances(scene.SceneNodes().size());
    for (const AccelStructInstancePlan& instance : plan.instances)
    {
        directInstances[instance.sceneNode.value].push_back(instance);
    }
    const SceneNodeHierarchy hierarchy = BuildSceneNodeHierarchy(scene);
    SceneMlasGrouping context{plan, scene, hierarchy, directInstances, level};
    const auto selected = [&context](uint32_t index) { return context.Selects(index); };

    std::vector<AccelStructInstancePlan> tlasInstances;
    for (uint32_t rootIndex : hierarchy.roots)
    {
        const SceneNodeId rootId{rootIndex};
        std::vector<AccelStructInstancePlan> contents = BuildSceneNodeContents(context, rootId);
        if (selected(rootIndex) && !contents.empty())
        {
            // A root MLAS carries no anchor node: the scene placement positions it instead.
            const MlasPlanId mlasId = context.AddMlas(std::move(contents), rootId);
            tlasInstances.push_back(
                {AccelStructRef::OfMlas(mlasId), nullptr, rootId, scene.SceneNodes()[rootIndex].playbackGroup});
        }
        else
        {
            // Nothing wraps this root, so its contents enter the TLAS directly and the paths
            // that were relative to it are dropped.
            for (AccelStructInstancePlan& instance : contents)
            {
                instance.transformPath.clear();
                if (instance.accelStruct.kind == AccelStructRefKind::Mlas)
                {
                    instance.pGltfNode = nullptr;
                    instance.playbackGroup = scene.SceneNodes()[instance.sceneNode.value].playbackGroup;
                }
                tlasInstances.push_back(std::move(instance));
            }
        }
    }
    plan.instances = std::move(tlasInstances);
}

// Grouping within an asset (Meshes and Hybrid). Each mesh node's primitives go into the BLAS that
// AssetBlasGroupKey picks, untransformed, and TLAS instances place the result. Scenes grouping is
// different in kind - it bakes geometry across placements into one structure - and has its own
// builder below.
AccelStructPlan BuildAssetGroupedPlan(const Scene& scene, DeformMode deformMode, AccelStructGroupingTarget target)
{
    AccelStructPlan plan;
    std::map<BlasGroupKey, BlasPlanId> keyToBlas;
    // The (playback group, node) that first created each BLAS. A rigid mesh's BLAS is shared, so
    // only its creator contributes geometry; every other user of the mesh just instances it.
    std::map<BlasGroupKey, std::pair<uint32_t, const Gltf::Node*>> keyOwner;

    for (uint32_t groupIndex = 0; groupIndex < scene.PlaybackGroups().size(); ++groupIndex)
    {
        const PlaybackGroup& group = scene.PlaybackGroups()[groupIndex];
        const GltfAsset& asset = scene.Asset(group.assetId);

        for (const auto& nodeOwner : asset.nodes)
        {
            const Gltf::Node& node = *nodeOwner;
            if (!node.pMesh)
            {
                continue;
            }
            const bool skinned = node.pSkin != nullptr;
            const BlasGroupKey key = AssetBlasGroupKey(target, group.assetId, PlaybackGroupId{groupIndex}, node);
            for (const auto& primitive : node.pMesh->primitives)
            {
                auto [entry, created] =
                    keyToBlas.try_emplace(key, BlasPlanId{static_cast<uint32_t>(plan.blases.size())});
                if (created)
                {
                    BlasPlan blas;
                    blas.playbackGroup = PlaybackGroupId{groupIndex};
                    plan.blases.push_back(std::move(blas));
                    keyOwner[key] = {groupIndex, &node};
                }

                const bool isOwner = keyOwner[key] == std::pair<uint32_t, const Gltf::Node*>{groupIndex, &node};
                if (skinned || isOwner)
                {
                    const bool deformed = skinned && deformMode != DeformMode::Transform && primitive->HasSkinAttributes();
                    BlasPlan& blas = plan.blases[entry->second.value];
                    blas.geometries.push_back(GeometrySourceId{static_cast<uint32_t>(plan.geometrySources.size())});
                    plan.geometrySources.push_back({&node, node.pMesh, primitive.get(), GeometryTransformMode::Identity,
                                                    deformed, group.assetId, PlaybackGroupId{groupIndex}});
                    if (deformed)
                    {
                        blas.updateMode = BlasUpdateMode::Refit;
                    }
                }
            }
        }
    }

    std::set<std::tuple<uint32_t, uint32_t, const Gltf::Node*>> seenInstances;
    for (uint32_t sceneNodeIndex = 0; sceneNodeIndex < scene.SceneNodes().size(); ++sceneNodeIndex)
    {
        const SceneNode& sceneNode = scene.SceneNodes()[sceneNodeIndex];
        const GltfAsset& asset = scene.Asset(sceneNode.assetId);
        for (const auto& nodeOwner : asset.nodes)
        {
            const Gltf::Node& node = *nodeOwner;
            if (!node.pMesh || node.pMesh->primitives.empty())
            {
                continue;
            }
            const BlasPlanId blas =
                keyToBlas.at(AssetBlasGroupKey(target, sceneNode.assetId, sceneNode.playbackGroup, node));
            const Gltf::Node* pInstanceNode = AssetBlasInstanceNode(node);
            if (seenInstances.emplace(sceneNodeIndex, blas.value, pInstanceNode).second)
            {
                plan.instances.push_back({AccelStructRef::OfBlas(blas), pInstanceNode, SceneNodeId{sceneNodeIndex},
                                          sceneNode.playbackGroup});
            }
        }
    }

    return plan;
}

// Scene grouping cuts the scene-node tree into segments. A segment is one *owner* scene node
// plus every descendant that the grouping level did not select as an owner of its own, and all
// the geometry in it is baked into a single BLAS.
//
// Two segments that hold the same geometry, reached through the same anchors and played back
// the same way, are interchangeable and share one BLAS. These keys say exactly that: an anchor
// step is (parent playback group, anchor node in the parent, child playback group).
using SceneAnchorKey = std::tuple<uint32_t, const Gltf::Node*, uint32_t>;
using SceneGeometryKey = std::tuple<uint32_t, uint32_t, const Gltf::Node*, const Gltf::Mesh*,
                                    const Gltf::Primitive*, bool, std::vector<SceneAnchorKey>>;
using SceneBlasKey = std::pair<uint32_t, std::vector<SceneGeometryKey>>;

struct SceneBlasGrouping
{
    AccelStructPlan& plan;
    const Scene& scene;
    const SceneNodeHierarchy& hierarchy;
    DeformMode deformMode;
    SceneLevel level;
    std::map<SceneBlasKey, BlasPlanId> blasByContents;

    bool Selects(uint32_t index) const { return hierarchy.Selects(index, level); }
};

// The anchor steps from `sceneNodeId` up to the owner of its segment.
std::vector<SceneAnchorKey> AnchorPathToOwner(const Scene& scene, SceneNodeId sceneNodeId, SceneNodeId ownerId)
{
    std::vector<SceneAnchorKey> path;
    for (SceneNodeId pathId = sceneNodeId; pathId != ownerId;)
    {
        const SceneNode& placement = scene.SceneNodes()[pathId.value];
        const SceneNode& parent = scene.SceneNodes()[placement.ParentId().value];
        path.emplace_back(parent.playbackGroup.value, placement.ParentAnchor(), placement.playbackGroup.value);
        pathId = placement.ParentId();
    }
    return path;
}

// Whether any anchor on that path is animated, which makes the whole baked BLAS need refitting.
bool AnchorPathIsAnimated(const Scene& scene, SceneNodeId sceneNodeId, SceneNodeId ownerId)
{
    for (SceneNodeId pathId = sceneNodeId; pathId != ownerId;)
    {
        const SceneNode& placement = scene.SceneNodes()[pathId.value];
        if (placement.ParentAnchor()->info.localRigidAnimation)
        {
            return true;
        }
        pathId = placement.ParentId();
    }
    return false;
}

struct SegmentGeometry
{
    std::vector<std::pair<SceneGeometryKey, GeometrySource>> entries;
    bool animated = false;
};

// Every mesh primitive reachable from the segment's scene nodes, each tagged with the key that
// decides whether another segment could reuse the resulting BLAS.
SegmentGeometry CollectSegmentGeometry(const SceneBlasGrouping& grouping, const std::vector<SceneNodeId>& segment,
                                       SceneNodeId ownerId)
{
    SegmentGeometry collected;
    for (SceneNodeId sceneNodeId : segment)
    {
        const SceneNode& sceneNode = grouping.scene.SceneNodes()[sceneNodeId.value];
        const GltfAsset& asset = grouping.scene.Asset(sceneNode.assetId);
        // Both depend only on where this placement sits relative to the owner, not on the
        // primitive, so they are computed once per placement rather than once per primitive.
        const std::vector<SceneAnchorKey> anchorPath = AnchorPathToOwner(grouping.scene, sceneNodeId, ownerId);
        const bool anchorPathAnimated = AnchorPathIsAnimated(grouping.scene, sceneNodeId, ownerId);

        for (const auto& nodeOwner : asset.nodes)
        {
            const Gltf::Node& node = *nodeOwner;
            if (!node.pMesh)
            {
                continue;
            }
            const bool skinned = node.pSkin != nullptr;
            for (const auto& primitive : node.pMesh->primitives)
            {
                const bool deformed =
                    skinned && grouping.deformMode != DeformMode::Transform && primitive->HasSkinAttributes();
                SceneGeometryKey key{sceneNode.assetId.value, sceneNode.playbackGroup.value, &node,     node.pMesh,
                                     primitive.get(),       deformed,                      anchorPath};
                collected.entries.emplace_back(std::move(key),
                                               GeometrySource{&node, node.pMesh, primitive.get(),
                                                              GeometryTransformMode::BakedFromNode, deformed,
                                                              sceneNode.assetId, sceneNode.playbackGroup, sceneNodeId});
                collected.animated =
                    collected.animated || deformed || node.info.localRigidAnimation || anchorPathAnimated;
            }
        }
    }
    return collected;
}

void BuildSegmentBlas(SceneBlasGrouping& grouping, SceneNodeId ownerId);

// Walks down from `sceneNodeId`, adding scene nodes to the current segment until it reaches one the
// grouping level selects - that one starts a segment of its own instead.
void CollectOwnerSegment(SceneBlasGrouping& grouping, SceneNodeId sceneNodeId, std::vector<SceneNodeId>& segment)
{
    segment.push_back(sceneNodeId);
    for (SceneNodeId childId : grouping.hierarchy.sceneNodes[sceneNodeId.value])
    {
        if (grouping.Selects(childId.value))
        {
            BuildSegmentBlas(grouping, childId);
        }
        else
        {
            CollectOwnerSegment(grouping, childId, segment);
        }
    }
}

// Builds the one BLAS for the segment owned by `ownerId`, reusing an existing BLAS if an earlier
// segment had identical contents, and adds the TLAS instance that places it.
void BuildSegmentBlas(SceneBlasGrouping& grouping, SceneNodeId ownerId)
{
    std::vector<SceneNodeId> segment;
    CollectOwnerSegment(grouping, ownerId, segment);

    SegmentGeometry collected = CollectSegmentGeometry(grouping, segment, ownerId);
    if (collected.entries.empty())
    {
        return;
    }

    AccelStructPlan& plan = grouping.plan;
    const SceneNode& owner = grouping.scene.SceneNodes()[ownerId.value];
    std::vector<std::pair<SceneGeometryKey, GeometrySource>>& geometries = collected.entries;
    std::sort(geometries.begin(), geometries.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });

    SceneBlasKey key;
    key.first = owner.playbackGroup.value;
    for (const auto& geometry : geometries)
    {
        key.second.push_back(geometry.first);
    }
    const auto [entry, created] =
        grouping.blasByContents.try_emplace(std::move(key), BlasPlanId{static_cast<uint32_t>(plan.blases.size())});
    if (created)
    {
        BlasPlan blas;
        blas.playbackGroup = owner.playbackGroup;
        blas.sceneNode = ownerId;
        blas.hasBakeTransforms = true;
        blas.updateMode = collected.animated ? BlasUpdateMode::Refit : BlasUpdateMode::Static;
        for (const auto& [geometryKey, source] : geometries)
        {
            (void)geometryKey;
            blas.geometries.push_back(GeometrySourceId{static_cast<uint32_t>(plan.geometrySources.size())});
            plan.geometrySources.push_back(source);
        }
        plan.blases.push_back(std::move(blas));
    }
    plan.instances.push_back({AccelStructRef::OfBlas(entry->second), nullptr, ownerId, owner.playbackGroup});
}

AccelStructPlan BuildSceneGroupedPlan(const Scene& scene, DeformMode deformMode, SceneLevel level)
{
    const SceneNodeHierarchy hierarchy = BuildSceneNodeHierarchy(scene);
    AccelStructPlan plan;
    SceneBlasGrouping context{plan, scene, hierarchy, deformMode, level, {}};
    for (uint32_t rootIndex : hierarchy.roots)
    {
        BuildSegmentBlas(context, SceneNodeId{rootIndex});
    }
    return plan;
}
} // namespace

std::vector<uint32_t> ComputeMlasDepths(const AccelStructPlan& plan)
{
    std::vector<uint32_t> depths(plan.mlases.size(), 0);
    for (uint32_t index = 0; index < static_cast<uint32_t>(plan.mlases.size()); ++index)
    {
        MlasDepth(plan, index, depths);
    }
    return depths;
}

AccelStructPlan BuildAccelStructPlan(const Scene& scene, DeformMode deformMode,
                                     const AccelStructHighLevelPlan& highLevelPlan)
{
    if (highLevelPlan.mlasLevels.size() > kMaxMlasLevels)
    {
        Fatal("The high-level acceleration plan exceeds the supported MLAS level count.");
    }

    // Step 1: group geometry into BLASes, either per asset or per scene segment.
    const AccelStructGrouping& blasLevel = highLevelPlan.blasLevel;
    AccelStructPlan plan = blasLevel.target == AccelStructGroupingTarget::Scenes
                               ? BuildSceneGroupedPlan(scene, deformMode, blasLevel.sceneLevel)
                               : BuildAssetGroupedPlan(scene, deformMode, blasLevel.target);

    // Step 2: wrap the instances produced above in MLAS levels, in the order requested.
    for (SceneLevel level : highLevelPlan.mlasLevels)
    {
        ApplySceneMlasGrouping(plan, scene, level);
    }
    const std::vector<uint32_t> mlasDepths = ComputeMlasDepths(plan);
    if (!mlasDepths.empty() && *std::max_element(mlasDepths.begin(), mlasDepths.end()) > kMaxMlasLevels)
    {
        Fatal("The high-level acceleration plan produces an MLAS hierarchy deeper than supported.");
    }
    DeduplicateMlases(plan);
    // The only validation on this path. It walks the finished plan once, including a
    // per-primitive coverage count, so the half-built plans the stages above hand to each other
    // are deliberately not re-checked; a defect in any of them still surfaces here.
    ValidateAccelStructPlan(plan, scene);
    return plan;
}
