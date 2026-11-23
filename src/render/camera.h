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

// Pinhole camera. The default constructor frames a scene's bounds from a fixed
// three-quarter view so headless renders are deterministic.
struct Camera
{
    float3 position{0.0f, 0.0f, 5.0f};
    float3 target{0.0f, 0.0f, 0.0f};
    float fovY = glm::quarter_pi<float>();

    static Camera FrameBounds(const Bounds& bounds);

    // Orthonormal basis with forward pointing at the target.
    void Basis(float3& forward, float3& right, float3& up) const;

    // Translates position and target together along the camera's local right/up/forward axes.
    void Move(const float3& localDelta);
    // Turns the view direction in place (radians); yaw is around world up, pitch around the camera's right axis.
    void Rotate(float yawDelta, float pitchDelta);
};
