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

#include "assets/scene.h"
#include "core/math.h"
#include "render/accel_struct_plan.h"
#include "render/gpu_types.h"
#include "render/asset_layout.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

// The acceleration-structure runtime that does not touch a graphics API: mirroring the plan into
// per-node state, ordering MLAS builds, and laying out the geometry table. Backends derive their
// own node types from the ones here and add GPU resources.

// One geometry inside a BLAS, traced back to the glTF primitive it came from.
struct BlasGeometry
{
    const Gltf::Node* gltfNode = nullptr;
    const Gltf::Mesh* mesh = nullptr;
    const Gltf::Primitive* primitive = nullptr;
    bool baked = false;
    bool deformed = false;
    AssetId assetId;
    PlaybackGroupId playbackGroup;
    std::optional<SceneNodeId> sceneNode;
};

// The API-neutral half of a BLAS.
struct BlasNode
{
    std::vector<BlasGeometry> geometries;
    uint32_t geometryTableOffset = 0;

    bool baked = false;    // geometry carries a non-identity bake transform
    bool animated = false; // a contributing node is animated, so the BLAS is refit
    bool built = false;
    PlaybackGroupId playbackGroup;
    std::optional<SceneNodeId> sceneNode;
};

// One instance, whether it sits directly in the TLAS or inside an MLAS.
struct AccelStructInstance
{
    AccelStructRef accelStruct;
    const Gltf::Node* gltfNode = nullptr; // nullptr means identity
    SceneNodeId sceneNode;
    PlaybackGroupId playbackGroup;
    std::vector<AccelStructTransformStep> transformPath;
};

// The API-neutral half of an MLAS.
struct MlasNode
{
    std::vector<AccelStructInstance> instances;
    PlaybackGroupId playbackGroup;
};

// Mirrors a plan into the backend's node types, which must derive from BlasNode and MlasNode.
// The containers are assumed empty, matching the rebuild contract.
template<typename Blas, typename Mlas>
void MirrorPlan(const AccelStructPlan& plan, std::vector<Blas>& blases, std::vector<Mlas>& mlases,
                std::vector<AccelStructInstance>& instances)
{
    blases.reserve(plan.blases.size());
    for (const BlasPlan& source : plan.blases)
    {
        Blas blas;
        blas.baked = source.hasBakeTransforms;
        blas.animated = source.updateMode == BlasUpdateMode::Refit;
        blas.playbackGroup = source.playbackGroup;
        blas.sceneNode = source.sceneNode;
        for (GeometrySourceId geometryId : source.geometries)
        {
            const GeometrySource& geometry = plan.geometrySources[geometryId.value];
            blas.geometries.push_back({geometry.pGltfNode, geometry.pMesh, geometry.pPrimitive,
                                       geometry.transformMode == GeometryTransformMode::BakedFromNode,
                                       geometry.deformed, geometry.assetId, geometry.playbackGroup,
                                       geometry.sceneNode});
        }
        blases.push_back(std::move(blas));
    }

    mlases.reserve(plan.mlases.size());
    for (const MlasPlan& source : plan.mlases)
    {
        Mlas mlas;
        mlas.playbackGroup = source.playbackGroup;
        mlas.instances.reserve(source.instances.size());
        for (const AccelStructInstancePlan& instance : source.instances)
        {
            mlas.instances.push_back({instance.accelStruct, instance.pGltfNode, instance.sceneNode,
                                      instance.playbackGroup, instance.transformPath});
        }
        mlases.push_back(std::move(mlas));
    }

    instances.reserve(plan.instances.size());
    for (const AccelStructInstancePlan& instance : plan.instances)
    {
        instances.push_back({instance.accelStruct, instance.pGltfNode, instance.sceneNode, instance.playbackGroup,
                             instance.transformPath});
    }
}

// An MLAS must be built after every MLAS it instances, so the order is a post-order walk of the
// plan tree, coloured with VisitState. The plan is validated acyclic before it reaches us.
namespace detail
{
template<typename Mlas>
void OrderMlas(const std::vector<Mlas>& mlases, uint32_t index, std::vector<VisitState>& state,
               std::vector<uint32_t>& order)
{
    if (state[index] != VisitState::Unvisited)
    {
        return;
    }
    state[index] = VisitState::InProgress;
    for (const AccelStructInstance& instance : mlases[index].instances)
    {
        if (instance.accelStruct.kind == AccelStructRefKind::Mlas)
        {
            OrderMlas(mlases, instance.accelStruct.value, state, order);
        }
    }
    state[index] = VisitState::Done;
    order.push_back(index);
}
} // namespace detail

template<typename Mlas>
std::vector<uint32_t> ComputeMlasBuildOrder(const std::vector<Mlas>& mlases)
{
    std::vector<uint32_t> order;
    order.reserve(mlases.size());
    std::vector<VisitState> state(mlases.size(), VisitState::Unvisited);
    for (uint32_t index = 0; index < static_cast<uint32_t>(mlases.size()); ++index)
    {
        detail::OrderMlas(mlases, index, state, order);
    }
    return order;
}

// Groups MLASes into build levels: levels[0] holds the MLASes that reference only BLASes, and levels[d] those whose
// deepest MLAS child sits in levels[d - 1]. No MLAS references another in its own level, so a level can be built in
// one batch with a single barrier before the next level.
template<typename Mlas>
std::vector<std::vector<uint32_t>> ComputeMlasBuildLevels(const std::vector<Mlas>& mlases)
{
    // Children come before parents in the build order, so each child's level is known by the time it is needed.
    std::vector<uint32_t> levelOf(mlases.size(), 0);
    std::vector<std::vector<uint32_t>> levels;
    for (uint32_t index : ComputeMlasBuildOrder(mlases))
    {
        uint32_t level = 0;
        for (const AccelStructInstance& instance : mlases[index].instances)
        {
            if (instance.accelStruct.kind == AccelStructRefKind::Mlas)
            {
                level = std::max(level, levelOf[instance.accelStruct.value] + 1);
            }
        }
        levelOf[index] = level;
        if (level >= levels.size())
        {
            levels.resize(level + 1);
        }
        levels[level].push_back(index);
    }
    return levels;
}

// Composes an instance's world transform: its own node transform, then every step of its
// transform path, then optionally the scene placement of the instance that owns it.
float4x4 ComposeInstanceTransform(const Scene& scene, const AccelStructInstance& instance, bool applyPlacement);

// Recomputes the bake transform of every geometry in a BLAS, writing it both to the GPU-visible
// `transforms` array and into the matching geometry table entries. `transforms` must have room
// for blas.geometries.size() matrices; the caller owns how it is mapped.
void RefreshBakeTransforms(const Scene& scene, const BlasNode& blas, float (*transforms)[3][4],
                           std::vector<GeometryInfo>& geometryTable);

// Which MLASes contain the debug-selected BLAS or MLAS. The debug visualization uses this to
// decide whether an instance is drawn when a single node has been isolated.
struct DebugMembership
{
    std::vector<bool> mlasContainsSelectedBlas;
    std::vector<bool> mlasContainsSelectedMlas;
    // Only populated for SelectedMlas: the selected MLAS and everything beneath it.
    std::vector<bool> mlasInSelectedMlasSubtree;
};

namespace detail
{
// Does the subtree under `mlasIndex` reach the selected BLAS? Records the answer for every MLAS
// it passes through, which is what the caller actually wants.
template<typename Mlas>
bool MarkContainsSelectedBlas(const std::vector<Mlas>& mlases, uint32_t mlasIndex, uint32_t selectedBlas,
                              std::vector<bool>& contains)
{
    bool found = false;
    for (const AccelStructInstance& instance : mlases[mlasIndex].instances)
    {
        found = found || (instance.accelStruct.kind == AccelStructRefKind::Blas
                              ? instance.accelStruct.value == selectedBlas
                              : MarkContainsSelectedBlas(mlases, instance.accelStruct.value, selectedBlas, contains));
    }
    contains[mlasIndex] = found;
    return found;
}

// The same question for a selected MLAS, where an MLAS also "contains" itself.
template<typename Mlas>
bool MarkContainsSelectedMlas(const std::vector<Mlas>& mlases, uint32_t mlasIndex, uint32_t selectedMlas,
                              std::vector<bool>& contains)
{
    bool found = mlasIndex == selectedMlas;
    for (const AccelStructInstance& instance : mlases[mlasIndex].instances)
    {
        if (instance.accelStruct.kind == AccelStructRefKind::Mlas)
        {
            found = found || MarkContainsSelectedMlas(mlases, instance.accelStruct.value, selectedMlas, contains);
        }
    }
    contains[mlasIndex] = found;
    return found;
}

// Flags the selected MLAS and every MLAS beneath it.
template<typename Mlas>
void MarkSelectedMlasSubtree(const std::vector<Mlas>& mlases, uint32_t mlasIndex, std::vector<bool>& inSubtree)
{
    if (inSubtree[mlasIndex])
    {
        return;
    }
    inSubtree[mlasIndex] = true;
    for (const AccelStructInstance& instance : mlases[mlasIndex].instances)
    {
        if (instance.accelStruct.kind == AccelStructRefKind::Mlas)
        {
            MarkSelectedMlasSubtree(mlases, instance.accelStruct.value, inSubtree);
        }
    }
}
} // namespace detail

// Hierarchies are at most kMaxMlasLevels deep, so these walks stay shallow.
template<typename Mlas>
DebugMembership ComputeDebugMembership(const std::vector<Mlas>& mlases, AccelStructDebugMode mode,
                                       uint32_t selectedIndex)
{
    DebugMembership membership;
    membership.mlasContainsSelectedBlas.assign(mlases.size(), false);
    membership.mlasContainsSelectedMlas.assign(mlases.size(), false);
    membership.mlasInSelectedMlasSubtree.assign(mlases.size(), false);

    for (uint32_t index = 0; index < static_cast<uint32_t>(mlases.size()); ++index)
    {
        detail::MarkContainsSelectedBlas(mlases, index, selectedIndex, membership.mlasContainsSelectedBlas);
        detail::MarkContainsSelectedMlas(mlases, index, selectedIndex, membership.mlasContainsSelectedMlas);
    }

    if (mode == AccelStructDebugMode::SelectedMlas && selectedIndex < mlases.size())
    {
        detail::MarkSelectedMlasSubtree(mlases, selectedIndex, membership.mlasInSelectedMlasSubtree);
    }
    return membership;
}

// Bindless indices for a geometry whose vertices are produced by the deform pass.
struct DeformedGeometrySrvs
{
    uint32_t positionSrv = kInvalidBindlessIndex;
    uint32_t attributeSrv = kInvalidBindlessIndex;
};

// Lays out the flat geometry table and records each BLAS's offset into it. Entries are grouped
// per BLAS so a hit shader can address them with InstanceID() + GeometryIndex().
//
// The three callbacks are the only backend-dependent parts: resolving a geometry to its uploaded
// primitive, to its deform-pass outputs, and an asset to its material table index.
template<typename Blas, typename PrimitiveFn, typename DeformedFn, typename MaterialTableFn>
std::vector<GeometryInfo> BuildGeometryTable(std::vector<Blas>& blases, PrimitiveFn primitiveFor,
                                             DeformedFn deformedFor, MaterialTableFn materialTableFor)
{
    std::vector<GeometryInfo> table;
    for (uint32_t blasIndex = 0; blasIndex < static_cast<uint32_t>(blases.size()); ++blasIndex)
    {
        Blas& blas = blases[blasIndex];
        blas.geometryTableOffset = static_cast<uint32_t>(table.size());
        for (const BlasGeometry& geometry : blas.geometries)
        {
            const auto& primitive = primitiveFor(geometry);

            GeometryInfo info{};
            StoreTransform3x4(info.bakeTransform, float4x4(1.0f));
            StoreNormalTransform3x4(info.bakeNormalTransform, float4x4(1.0f));
            info.positions = ToAccessorInfo(primitive.positions);
            info.normals = ToAccessorInfo(primitive.normals);
            info.uvs = ToAccessorInfo(primitive.uvs);
            if (geometry.deformed)
            {
                // Deformed vertices are written as tightly packed float3 positions and
                // VertexAttributes records, so they are described directly rather than through the
                // source accessor layout. UVs ride along inside the attribute record.
                const DeformedGeometrySrvs deformed = deformedFor(geometry);
                const uint32_t floatType = static_cast<uint32_t>(Gltf::AccessorComponentType::Float);
                info.positions = {deformed.positionSrv, 0, sizeof(float3), floatType};
                info.normals = {deformed.attributeSrv, 0, sizeof(VertexAttributes), floatType};
                info.uvs = {deformed.attributeSrv, sizeof(float3), sizeof(VertexAttributes), floatType};
            }
            info.indices = ToAccessorInfo(primitive.indices);
            info.joints = ToAccessorInfo(primitive.joints);
            info.weights = ToAccessorInfo(primitive.weights);
            info.materialIndex = primitive.materialIndex;
            info.materialTableIndex = materialTableFor(geometry.assetId);
            info.blasIndex = blasIndex;
            table.push_back(info);
        }
    }
    return table;
}
