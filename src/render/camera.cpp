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

#include "render/camera.h"

#include <cmath>

Camera Camera::FrameBounds(const Bounds& bounds)
{
    Camera camera;
    if (!bounds.Valid())
    {
        return camera;
    }

    camera.target = bounds.Center();

    // Pull back far enough for the bounding sphere to fit the vertical field of view,
    // with a small margin so nothing touches the edge of the frame.
    const float distance = 1.15f * bounds.Radius() / std::sin(0.5f * camera.fovY);
    const float3 direction = Normalize(float3(0.6f, 0.35f, 1.0f));
    camera.position = camera.target + direction * distance;
    return camera;
}

void Camera::Basis(float3& forward, float3& right, float3& up) const
{
    forward = Normalize(target - position);
    float3 worldUp(0.0f, 1.0f, 0.0f);
    // Fall back to a different up vector when looking straight up or down.
    if (std::abs(glm::dot(forward, worldUp)) > 0.999f)
    {
        worldUp = float3(0.0f, 0.0f, 1.0f);
    }

    right = Normalize(glm::cross(worldUp, forward));
    up = glm::cross(forward, right);
}

void Camera::Move(const float3& localDelta)
{
    float3 forward, right, up;
    Basis(forward, right, up);

    const float3 offset = right * localDelta.x + up * localDelta.y + forward * localDelta.z;

    position += offset;
    target += offset;
}

void Camera::Rotate(float yawDelta, float pitchDelta)
{
    float3 forward, right, up;
    Basis(forward, right, up);

    const float3 worldUp(0.0f, 1.0f, 0.0f);
    const float distance = glm::length(target - position);

    float3 forwardVector =
        Normalize(float3(glm::rotate(float4x4(1.0f), yawDelta, worldUp) * float4(forward, 0.0f)));

    const float3 rightVector = Normalize(glm::cross(worldUp, forwardVector));
    const float3 pitchedForward =
        Normalize(float3(glm::rotate(float4x4(1.0f), pitchDelta, rightVector) * float4(forwardVector, 0.0f)));

    // Reject the pitch step once it would flip the view past straight up/down.
    if (std::abs(glm::dot(pitchedForward, worldUp)) < 0.999f)
    {
        forwardVector = pitchedForward;
    }

    target = position + forwardVector * distance;
}
