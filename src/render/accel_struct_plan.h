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
#include "assets/scene.h"
#include "options.h"

#include <cstdint>
#include <optional>
#include <vector>

// These ID types exist to provide more type safety
struct GeometrySourceId
{
    uint32_t value = 0;
};

struct BlasPlanId
{
    uint32_t value = 0;
};

struct MlasPlanId
{
    uint32_t value = 0;
};

enum class AccelStructRefKind
{
    Blas,
    Mlas,
};

// Reference to an acceleration structure that can be instanced in another acceleration structure (BLAS or MLAS)
struct AccelStructRef
{
    AccelStructRefKind kind = AccelStructRefKind::Blas;
    uint32_t value = 0;

    static AccelStructRef OfBlas(BlasPlanId id) { return {AccelStructRefKind::Blas, id.value}; }
    static AccelStructRef OfMlas(MlasPlanId id) { return {AccelStructRefKind::Mlas, id.value}; }

    bool operator==(const AccelStructRef& other) const { return kind == other.kind && value == other.value; }
    bool operator<(const AccelStructRef& other) const
    {
        return kind != other.kind ? kind < other.kind : value < other.value;
    }
};

enum class GeometryTransformMode
{
    Identity,
    BakedFromNode,
};

enum class BlasUpdateMode
{
    Static,
    Refit,
};

struct AccelStructTransformStep
{
    PlaybackGroupId playbackGroup;
    const Gltf::Node* pGltfNode = nullptr;
};

// Loader pointers are non-owning; the source Scene must outlive the plan.
struct GeometrySource
{
    const Gltf::Node* pGltfNode = nullptr;
    const Gltf::Mesh* pMesh = nullptr;
    const Gltf::Primitive* pPrimitive = nullptr;
    GeometryTransformMode transformMode = GeometryTransformMode::Identity;
    bool deformed = false;
    AssetId assetId;
    PlaybackGroupId playbackGroup;
    // Set when this is owned by a specific scene node rather than shared.
    std::optional<SceneNodeId> sceneNode;
};

struct BlasPlan
{
    std::vector<GeometrySourceId> geometries;
    bool hasBakeTransforms = false;
    BlasUpdateMode updateMode = BlasUpdateMode::Static;
    PlaybackGroupId playbackGroup;
    // Set when this is owned by a specific scene node rather than shared.
    std::optional<SceneNodeId> sceneNode;
};

// One instance, whether it lives directly in the TLAS or inside an MLAS.
struct AccelStructInstancePlan
{
    AccelStructRef accelStruct;
    const Gltf::Node* pGltfNode = nullptr;
    SceneNodeId sceneNode;
    PlaybackGroupId playbackGroup;
    std::vector<AccelStructTransformStep> transformPath;
};

struct MlasPlan
{
    std::vector<AccelStructInstancePlan> instances;
    PlaybackGroupId playbackGroup;
    // Set when a specific scene node owns this MLAS; shared MLASes leave it unset.
    std::optional<SceneNodeId> ownerSceneNode;
};

struct AccelStructPlan
{
    std::vector<GeometrySource> geometrySources;
    std::vector<BlasPlan> blases;
    std::vector<MlasPlan> mlases;
    std::vector<AccelStructInstancePlan> instances;
};

AccelStructPlan BuildAccelStructPlan(const Scene& scene, DeformMode deformMode,
                                                         const AccelStructHighLevelPlan& highLevelPlan);
void ValidateAccelStructPlan(const AccelStructPlan& plan, const Scene& scene);

// The depth of each MLAS in the plan. MLASs that contain only BLASs have depth 1, MLASs that contain MLASs up to depth
// d have depth d+1.
std::vector<uint32_t> ComputeMlasDepths(const AccelStructPlan& plan);

// State used to track the progress of algorithms that traverse an acceleration structure hierarchy
enum class VisitState : uint8_t
{
    Unvisited,
    InProgress,
    Done,
};
