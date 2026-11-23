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
#include "render/accel_struct_plan.h"
#include "render/timing.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ImDrawData;
class Window;

struct BackendOptions
{
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t maxBounces = 1;
    bool debugLayer = false;
    bool vsync = true;
    bool mlasFullBarriers = false;
    bool measureFrameTime = false;
};

// What the backend can actually do on the current device, so that callers test a capability
// rather than assuming it.
struct BackendCapabilities
{
    std::string adapter;
    uint32_t maxMlasLevels = 0;
};

struct AccelStructSizes
{
    uint64_t accelStructBytes = 0;
    uint64_t scratchBytes = 0;
};

struct RenderDiagnostics
{
    AccelStructPlan plan;
    std::vector<AccelStructSizes> blases;
    std::vector<AccelStructSizes> mlases;
    AccelStructSizes tlas;
    uint32_t animatedBlasCount = 0;
    uint32_t geometryCount = 0;
    uint32_t deformedPrimitiveCount = 0;
    uint32_t deformedVertexCount = 0;
    TimingSnapshot blasBuild;
    TimingSnapshot mlasTlasBuild;
    TimingSnapshot totalBuild;
    TimingSnapshot traceRays;
};

// Which acceleration-structure node the debug visualization isolates. This is all the
// AccelStructs classes need, so it is what they are handed.
struct DebugSelection
{
    AccelStructDebugMode mode = AccelStructDebugMode::Full;
    uint32_t index = 0;
};

// Everything the debug visualization controls: which node is isolated, and how hits are shaded.
// Carried whole by the app, the GUI and a frame's input so the settings cannot drift apart.
struct DebugVisualization
{
    DebugSelection selection;
    bool shadeByBlas = false;
};

struct FrameInput
{
    float3 cameraPosition{};
    float3 cameraForward{};
    float3 cameraRight{};
    float3 cameraUp{};
    float tanHalfFovY = 0.0f;
    float rayEpsilon = 1e-3f;
    uint32_t maxBounces = 1;
    float3 sunDirection{};
    float3 sunColor{1.0f, 0.97f, 0.92f};
    float ambientIntensity = 0.15f;
    float3 skyTopColor{0.30f, 0.50f, 0.90f};
    float3 skyBottomColor{0.75f, 0.80f, 0.85f};
    DebugVisualization debug;
};

struct CompletedFrameTimings
{
    uint64_t frameId = 0;
    double frameMs = 0.0;
    double blasMs = 0.0;
    double mlasTlasMs = 0.0;
    double traceMs = 0.0;
};

struct OutputImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowPitch = 0;
    std::vector<uint8_t> rgba;
};

class IRenderBackend
{
public:
    virtual ~IRenderBackend() = default;
    virtual const BackendCapabilities& Capabilities() const = 0;
    virtual const RenderDiagnostics& Diagnostics() const = 0;
    // The scene and its loader-owned objects must outlive the backend.
    virtual void SetScene(const Scene& scene, DeformMode deformMode, const AccelStructPlan& plan) = 0;
    virtual void RebuildAccelStructs(const AccelStructPlan& plan) = 0;
    virtual void Resize(uint32_t width, uint32_t height) = 0;
    virtual void BeginGuiFrame() = 0;
    // Consumes evaluated scene state and frame input; GPU execution remains asynchronous.
    virtual std::optional<CompletedFrameTimings> RenderFrame(const FrameInput& frame, ImDrawData* gui = nullptr) = 0;
    virtual OutputImage ReadOutput() = 0;
    virtual void WaitIdle() = 0;
};

std::unique_ptr<IRenderBackend> CreateRenderBackend(const BackendOptions& options, Window* window);
// Call ValidateAccelStructPlan first; this checks device capabilities, not graph integrity.
void ValidateBackendPlan(const BackendCapabilities& capabilities, const AccelStructPlan& plan);
