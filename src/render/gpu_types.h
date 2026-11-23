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

#include "core/math.h"

// C++ mirrors of the structures in shaders/common.hlsli. Keep both in sync.

struct FrameConstants
{
    float3 cameraPosition;
    float tanHalfFovY = 0.0f;
    float3 cameraForward;
    float aspectRatio = 1.0f;
    float3 cameraRight;
    float rayEpsilon = 1e-3f;
    float3 cameraUp;
    uint32_t maxBounces = 1;
    float3 sunDirection;
    uint32_t outputIndex = 0;
    float3 sunColor;
    uint32_t geometryTableIndex = 0;
    float3 skyTopColor;
    float ambientIntensity = 0.05f;
    float3 skyBottomColor;
    uint32_t shadeByBlas = 0;
};

struct AccessorInfo
{
    uint32_t bufferIndex = 0xFFFFFFFFu;
    uint32_t byteOffset = 0;
    uint32_t byteStride = 0;
    uint32_t format = 0;
};

struct GeometryInfo
{
    float bakeTransform[3][4];
    float bakeNormalTransform[3][4];
    AccessorInfo positions;
    AccessorInfo normals;
    AccessorInfo uvs;
    AccessorInfo indices;
    AccessorInfo joints;
    AccessorInfo weights;
    uint32_t materialIndex = 0;
    uint32_t materialTableIndex = 0;
    uint32_t blasIndex = 0;
};

struct MaterialInfo
{
    float4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float3 emissive{0.0f, 0.0f, 0.0f};
    float metallic = 0.0f;
    float roughness = 1.0f;
    uint32_t baseColorTexture = 0xFFFFFFFFu;
    float2 padding{0.0f, 0.0f};
};

struct VertexAttributes
{
    float3 normal;
    float2 uv;
};

struct SkinVertex
{
    uint4 joints;
    float4 weights;
};

struct JointMatrix
{
    float transform[3][4];
};

static_assert(sizeof(FrameConstants) == 128, "FrameConstants must match the HLSL layout.");
static_assert(sizeof(AccessorInfo) == 16, "AccessorInfo must match the HLSL layout.");
static_assert(sizeof(GeometryInfo) == 204, "GeometryInfo must match the HLSL layout.");
static_assert(sizeof(MaterialInfo) == 48, "MaterialInfo must match the HLSL layout.");
static_assert(sizeof(VertexAttributes) == 20, "VertexAttributes must match the HLSL layout.");
static_assert(sizeof(SkinVertex) == 32, "SkinVertex must match the HLSL layout.");
static_assert(sizeof(JointMatrix) == 48, "JointMatrix must match the HLSL layout.");
