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
#include <map>
#include <tuple>
#include <vector>

namespace
{

template<typename T>
bool Contains(const std::vector<std::unique_ptr<T>>& objects, const T* object)
{
    return object &&
           std::any_of(objects.begin(), objects.end(), [object](const auto& entry) { return entry.get() == object; });
}

struct PlanTreeUsage
{
    std::vector<bool> blasInstantiated;
    std::vector<bool> mlasInstantiated;
};

void VisitAccelStructRef(const AccelStructPlan& plan, AccelStructRef accelStruct,
                         std::vector<VisitState>& mlasState, PlanTreeUsage& usage)
{
    if (accelStruct.kind == AccelStructRefKind::Blas)
    {
        if (accelStruct.value >= plan.blases.size())
        {
            Fatal("Acceleration-structure plan has an invalid BLAS instance reference.");
        }
        usage.blasInstantiated[accelStruct.value] = true;
        return;
    }

    if (accelStruct.value >= plan.mlases.size())
    {
        Fatal("Acceleration-structure plan has an invalid MLAS instance reference.");
    }
    usage.mlasInstantiated[accelStruct.value] = true;
    if (mlasState[accelStruct.value] == VisitState::InProgress)
    {
        Fatal("Acceleration-structure plan has a cycle in the MLAS tree.");
    }
    if (mlasState[accelStruct.value] == VisitState::Done)
    {
        return;
    }

    mlasState[accelStruct.value] = VisitState::InProgress;
    const MlasPlan& mlas = plan.mlases[accelStruct.value];
    if (mlas.instances.empty())
    {
        Fatal("Acceleration-structure plan has an empty MLAS.");
    }
    for (const AccelStructInstancePlan& instance : mlas.instances)
    {
        VisitAccelStructRef(plan, instance.accelStruct, mlasState, usage);
    }
    mlasState[accelStruct.value] = VisitState::Done;
}

// Walks the acceleration structure tree and reject it if there are any empty or unused acceleration structures or if
// there is a cycle.
PlanTreeUsage ValidatePlanTree(const AccelStructPlan& plan)
{
    PlanTreeUsage usage;
    usage.blasInstantiated.assign(plan.blases.size(), false);
    usage.mlasInstantiated.assign(plan.mlases.size(), false);
    std::vector<VisitState> mlasState(plan.mlases.size(), VisitState::Unvisited);

    for (const AccelStructInstancePlan& instance : plan.instances)
    {
        VisitAccelStructRef(plan, instance.accelStruct, mlasState, usage);
    }

    for (bool value : usage.blasInstantiated)
    {
        if (!value)
        {
            Fatal("Acceleration-structure plan has a BLAS with no instances.");
        }
    }
    for (bool value : usage.mlasInstantiated)
    {
        if (!value)
        {
            Fatal("Acceleration-structure plan has an MLAS with no instances.");
        }
    }
    return usage;
}

void ValidateBlases(const AccelStructPlan& plan, const Scene& scene)
{
    uint64_t geometryTableOffset = 0;
    for (const BlasPlan& blas : plan.blases)
    {
        if (blas.geometries.empty())
        {
            Fatal("Acceleration-structure plan has an empty BLAS.");
        }
        if (geometryTableOffset > 0xFFFFFFu)
        {
            Fatal("Acceleration-structure plan exceeds the 24-bit instance custom index limit.");
        }
        geometryTableOffset += blas.geometries.size();

        bool hasBakeTransforms = false;
        for (GeometrySourceId geometryId : blas.geometries)
        {
            if (geometryId.value >= plan.geometrySources.size())
            {
                Fatal("Acceleration-structure plan has an invalid geometry reference.");
            }
            const GeometrySource& geometry = plan.geometrySources[geometryId.value];
            if (geometry.assetId.value >= scene.Assets().size() ||
                geometry.playbackGroup.value >= scene.PlaybackGroups().size())
            {
                Fatal("Acceleration-structure plan has invalid scene geometry ownership.");
            }
            if (geometry.sceneNode.has_value() && geometry.sceneNode->value >= scene.SceneNodes().size())
            {
                Fatal("Acceleration-structure plan has an invalid geometry scene node.");
            }
            const GltfAsset& asset = scene.Asset(geometry.assetId);
            if (!Contains(asset.nodes, geometry.pGltfNode) || !Contains(asset.meshes, geometry.pMesh) ||
                geometry.pGltfNode->pMesh != geometry.pMesh || !Contains(geometry.pMesh->primitives, geometry.pPrimitive))
            {
                Fatal("Acceleration-structure plan has an invalid scene geometry source pointer.");
            }
            hasBakeTransforms = hasBakeTransforms || geometry.transformMode == GeometryTransformMode::BakedFromNode;
        }
        if (hasBakeTransforms != blas.hasBakeTransforms || blas.playbackGroup.value >= scene.PlaybackGroups().size())
        {
            Fatal("Acceleration-structure plan has inconsistent scene BLAS metadata.");
        }
        if (blas.sceneNode.has_value() && blas.sceneNode->value >= scene.SceneNodes().size())
        {
            Fatal("Acceleration-structure plan has an invalid BLAS owner scene node.");
        }
    }
}

void ValidateMlases(const AccelStructPlan& plan, const Scene& scene)
{
    for (const MlasPlan& mlas : plan.mlases)
    {
        if (mlas.playbackGroup.value >= scene.PlaybackGroups().size())
        {
            Fatal("Acceleration-structure plan has inconsistent scene MLAS metadata.");
        }
        if (mlas.ownerSceneNode.has_value() && mlas.ownerSceneNode->value >= scene.SceneNodes().size())
        {
            Fatal("Acceleration-structure plan has an invalid MLAS owner scene node.");
        }
    }
}

void ValidateInstances(const AccelStructPlan& plan, const Scene& scene)
{
    auto validateInstances = [&scene](const std::vector<AccelStructInstancePlan>& instances) {
        for (const AccelStructInstancePlan& instance : instances)
        {
            if (instance.sceneNode.value >= scene.SceneNodes().size() ||
                instance.playbackGroup.value >= scene.PlaybackGroups().size())
            {
                Fatal("Acceleration-structure plan has an invalid scene node.");
            }

            for (const AccelStructTransformStep& step : instance.transformPath)
            {
                if (step.playbackGroup.value >= scene.PlaybackGroups().size())
                {
                    Fatal("Acceleration-structure plan has an invalid transform-path playback group.");
                }
                const GltfAsset& asset = scene.Asset(scene.Group(step.playbackGroup).assetId);
                if (!Contains(asset.nodes, step.pGltfNode))
                {
                    Fatal("Acceleration-structure plan has an invalid transform-path node.");
                }
            }

            const SceneNode& sceneNode = scene.SceneNodes()[instance.sceneNode.value];
            const GltfAsset& transformAsset = scene.Asset(scene.Group(instance.playbackGroup).assetId);
            if (instance.pGltfNode && !Contains(transformAsset.nodes, instance.pGltfNode))
            {
                Fatal("Acceleration-structure plan has an invalid instance node pointer.");
            }
            if (instance.accelStruct.kind != AccelStructRefKind::Mlas)
            {
                continue;
            }

            // Every MLAS wraps a scene node. A top-level one is positioned by its placement and
            // carries no anchor; a nested one is anchored at its parent anchor node.
            if (!instance.pGltfNode)
            {
                if (instance.playbackGroup != sceneNode.playbackGroup)
                {
                    Fatal("Top-level scene MLAS wrapper has an inconsistent playback group.");
                }
                continue;
            }
            if (!sceneNode.IsExternalAsset() || sceneNode.ParentId().value >= scene.SceneNodes().size() ||
                instance.pGltfNode != sceneNode.ParentAnchor() ||
                instance.playbackGroup != scene.SceneNodes()[sceneNode.ParentId().value].playbackGroup)
            {
                Fatal("Nested scene MLAS wrapper has an invalid parent anchor binding.");
            }
        }
    };

    validateInstances(plan.instances);
    for (const MlasPlan& mlas : plan.mlases)
    {
        validateInstances(mlas.instances);
    }
}

// One use of one primitive by one scene node
using PrimitiveUsage = std::tuple<uint32_t, const Gltf::Mesh*, const Gltf::Primitive*>;
// A scene node identified by where it sits rather than by index: (parent, anchor node in that
// parent, asset, playback group).
using ChildKey = std::tuple<uint32_t, const Gltf::Node*, uint32_t, uint32_t>;

// What the coverage walk needs. A shared MLAS stands in for several placements, so the walk
// carries the placement it is *currently* counting for and maps the plan's scene nodes onto it.
struct CoverageWalk
{
    const AccelStructPlan& plan;
    const Scene& scene;
    const std::map<ChildKey, SceneNodeId>& childrenByPlacement;
    std::map<PrimitiveUsage, uint32_t>& plannedUsage;

    // Translates source beneath sourceRoot, to the matching node beneath targetRoot
    SceneNodeId MapSceneNode(SceneNodeId source, SceneNodeId sourceRoot, SceneNodeId targetRoot) const;
};

SceneNodeId CoverageWalk::MapSceneNode(SceneNodeId source, SceneNodeId sourceRoot, SceneNodeId targetRoot) const
{
    if (sourceRoot == targetRoot)
    {
        return source;
    }
    if (scene.SceneNodes()[sourceRoot.value].assetId != scene.SceneNodes()[targetRoot.value].assetId)
    {
        Fatal("Shared MLAS owners do not reference the same source asset.");
    }
    std::vector<SceneNodeId> path;
    for (SceneNodeId current = source; current != sourceRoot;
         current = scene.SceneNodes()[current.value].ParentId())
    {
        if (current.value >= scene.SceneNodes().size() ||
            scene.SceneNodes()[current.value].ParentId().value >= scene.SceneNodes().size())
        {
            Fatal("Shared MLAS child is not beneath its canonical owner.");
        }
        path.push_back(current);
    }
    SceneNodeId mapped = targetRoot;
    for (auto pathEntry = path.rbegin(); pathEntry != path.rend(); ++pathEntry)
    {
        const SceneNode& sourceChild = scene.SceneNodes()[pathEntry->value];
        const auto match = childrenByPlacement.find(ChildKey{
            mapped.value, sourceChild.ParentAnchor(), sourceChild.assetId.value, sourceChild.playbackGroup.value});
        if (match == childrenByPlacement.end())
        {
            Fatal("Shared MLAS placement has no corresponding child scene node.");
        }
        mapped = match->second;
    }
    return mapped;
}

// Counts every primitive that is included in the specified acceleration structure instance plan
void CountInstancePath(CoverageWalk& walk, const AccelStructInstancePlan& instance, SceneNodeId sourceRoot,
                       SceneNodeId targetRoot)
{
    const bool atTlas = sourceRoot.value == UINT32_MAX;
    if (instance.accelStruct.kind == AccelStructRefKind::Mlas)
    {
        const MlasPlan& mlas = walk.plan.mlases[instance.accelStruct.value];
        const SceneNodeId nextSourceRoot = mlas.ownerSceneNode.value_or(instance.sceneNode);
        const SceneNodeId nextTargetRoot =
            atTlas ? instance.sceneNode : walk.MapSceneNode(instance.sceneNode, sourceRoot, targetRoot);
        for (const AccelStructInstancePlan& child : mlas.instances)
        {
            CountInstancePath(walk, child, nextSourceRoot, nextTargetRoot);
        }
        return;
    }

    const BlasPlan& blas = walk.plan.blases[instance.accelStruct.value];
    const SceneNodeId targetBlasOwner =
        atTlas ? instance.sceneNode : walk.MapSceneNode(instance.sceneNode, sourceRoot, targetRoot);
    for (GeometrySourceId geometryId : blas.geometries)
    {
        const GeometrySource& geometry = walk.plan.geometrySources[geometryId.value];
        if (geometry.assetId != walk.scene.SceneNodes()[instance.sceneNode.value].assetId &&
            !geometry.sceneNode.has_value())
        {
            continue;
        }
        const SceneNodeId geometryInstance =
            geometry.sceneNode.has_value() ? walk.MapSceneNode(*geometry.sceneNode, *blas.sceneNode, targetBlasOwner)
                                           : targetBlasOwner;
        ++walk.plannedUsage[{geometryInstance.value, geometry.pMesh, geometry.pPrimitive}];
    }
}

// Makes sure all the primitives are included in plan
void ValidatePrimitiveCoverage(const AccelStructPlan& plan, const Scene& scene)
{
    std::map<PrimitiveUsage, uint32_t> expectedUsage;
    std::map<PrimitiveUsage, uint32_t> plannedUsage;
    for (uint32_t sceneNodeIndex = 0; sceneNodeIndex < scene.SceneNodes().size(); ++sceneNodeIndex)
    {
        const SceneNode& sceneNode = scene.SceneNodes()[sceneNodeIndex];
        const GltfAsset& asset = scene.Asset(sceneNode.assetId);
        for (const auto& node : asset.nodes)
        {
            if (!node->pMesh)
            {
                continue;
            }
            for (const auto& primitive : node->pMesh->primitives)
            {
                ++expectedUsage[{sceneNodeIndex, node->pMesh, primitive.get()}];
            }
        }
    }

    std::map<ChildKey, SceneNodeId> childrenByPlacement;
    for (uint32_t index = 0; index < scene.SceneNodes().size(); ++index)
    {
        const SceneNode& sceneNode = scene.SceneNodes()[index];
        childrenByPlacement.emplace(ChildKey{sceneNode.ParentId().value, sceneNode.ParentAnchor(),
                                             sceneNode.assetId.value, sceneNode.playbackGroup.value},
                                    SceneNodeId{index});
    }

    CoverageWalk walk{plan, scene, childrenByPlacement, plannedUsage};
    for (const AccelStructInstancePlan& instance : plan.instances)
    {
        CountInstancePath(walk, instance, SceneNodeId{UINT32_MAX}, SceneNodeId{UINT32_MAX});
    }
    if (expectedUsage != plannedUsage)
    {
        Fatal("Acceleration-structure plan does not represent every scene mesh primitive exactly once.");
    }
}

} // namespace

void ValidateAccelStructPlan(const AccelStructPlan& plan, const Scene& scene)
{
    if (plan.blases.empty() || plan.instances.empty())
    {
        Fatal("Acceleration-structure plan contains no BLAS or no TLAS instance.");
    }
    ValidateBlases(plan, scene);
    ValidateMlases(plan, scene);
    ValidatePlanTree(plan);
    ValidateInstances(plan, scene);
    ValidatePrimitiveCoverage(plan, scene);
}
