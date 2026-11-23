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

#include "render/accel_struct_runtime.h"

#include "assets/gltf_state.h"

float4x4 ComposeInstanceTransform(const Scene& scene, const AccelStructInstance& instance, bool applyPlacement)
{
    const GltfState& state = scene.Group(instance.playbackGroup).State();
    float4x4 local = instance.gltfNode ? state.Node(instance.gltfNode).world : float4x4(1.0f);

    // The path walks from the instance up to whatever node owns it, so that an instance nested
    // inside an MLAS is positioned relative to that MLAS rather than to the world.
    for (const AccelStructTransformStep& step : instance.transformPath)
    {
        const GltfState& pathState = scene.Group(step.playbackGroup).State();
        const float4x4 pathTransform = step.pGltfNode ? pathState.Node(step.pGltfNode).world : float4x4(1.0f);
        local = pathTransform * local;
    }

    if (!applyPlacement)
    {
        return local;
    }
    const SceneNode& placement = scene.SceneNodes()[instance.sceneNode.value];
    return placement.placement * local;
}

void RefreshBakeTransforms(const Scene& scene, const BlasNode& blas, float (*transforms)[3][4],
                           std::vector<GeometryInfo>& geometryTable)
{
    // Geometry is baked into the BLAS relative to the scene node that owns it, so the owner's
    // placement is divided back out.
    float4x4 rootInverse(1.0f);
    if (blas.sceneNode.has_value())
    {
        rootInverse = glm::inverse(scene.SceneNodes()[blas.sceneNode->value].placement);
    }

    for (size_t i = 0; i < blas.geometries.size(); ++i)
    {
        const BlasGeometry& geometry = blas.geometries[i];
        // Deformed geometry is already in the space the deform pass wrote it in.
        float4x4 geometryWorld = geometry.deformed
                                     ? float4x4(1.0f)
                                     : scene.Group(geometry.playbackGroup).State().Node(geometry.gltfNode).world;
        if (geometry.sceneNode.has_value())
        {
            geometryWorld = scene.SceneNodes()[geometry.sceneNode->value].placement * geometryWorld;
        }
        const float4x4 bake = geometry.baked ? rootInverse * geometryWorld : float4x4(1.0f);

        StoreTransform3x4(transforms[i], bake);
        GeometryInfo& info = geometryTable[blas.geometryTableOffset + i];
        StoreTransform3x4(info.bakeTransform, bake);
        StoreNormalTransform3x4(info.bakeNormalTransform, bake);
    }
}
