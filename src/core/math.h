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

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <cstring>
#include <limits>

using float2 = glm::vec2;
using float3 = glm::vec3;
using float4 = glm::vec4;
using float4x4 = glm::mat4;

constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

// Deliberately not glm::uvec4: this only ever describes a GPU-visible record, and the member
// initializers below are what keep SkinVertex zero-filled.
struct uint4
{
    uint32_t x = 0, y = 0, z = 0, w = 0;
};

// Normalization by a single division, preferred over glm::normalize throughout. glm::normalize
// evaluates `v * inversesqrt(dot(v, v))`, which rounds twice; dividing once is the more accurate
// of the two. The difference is one ULP, but it reaches the camera basis and therefore every
// primary ray, so it is visible as moved pixels in the golden images.
template<typename T>
inline T Normalize(const T& v)
{
    return v / glm::length(v);
}

// Column-vector convention throughout: world = parentWorld * local, and points are transformed as
// `M * p`. This is what glTF specifies for node hierarchies and skinning, and what the shaders
// already do with the `row_major float3x4` matrices in common.hlsli.
inline float4x4 ComposeTrs(const float3& translation, const float4& rotation, const float3& scale)
{
    // glTF stores quaternions xyzw; glm::quat's scalar-first constructor takes w first.
    const glm::quat orientation(rotation.w, rotation.x, rotation.y, rotation.z);
    return glm::translate(float4x4(1.0f), translation) * glm::mat4_cast(orientation) *
           glm::scale(float4x4(1.0f), scale);
}

// Writes the 3x4 row-major matrix that Vulkan acceleration structure instances expect.
// glm::mat4 is column-major, so transposing first puts the matrix's top three rows in the leading
// twelve floats.
inline void StoreTransform3x4(float destination[3][4], const float4x4& matrix)
{
    const float4x4 transposed = glm::transpose(matrix);
    memcpy(destination, &transposed, sizeof(float) * 12);
}

// Companion normal matrix for StoreTransform3x4. Normals need (M^-1)^T, and the shader reads this
// slot as a `row_major float3x4` - reading column-major storage row-major already transposes it, so
// the plain inverse is stored and no explicit transpose is applied.
inline void StoreNormalTransform3x4(float destination[3][4], const float4x4& matrix)
{
    const float4x4 inverse = glm::inverse(matrix);
    memcpy(destination, &inverse, sizeof(float) * 12);
}

struct Bounds
{
    float3 min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    float3 max{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
               std::numeric_limits<float>::lowest()};

    bool Valid() const { return min.x <= max.x; }

    void Add(const float3& point)
    {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    float3 Center() const { return 0.5f * (min + max); }

    float Radius() const
    {
        if (!Valid())
        {
            return 1.0f;
        }
        return 0.5f * glm::length(max - min);
    }
};
