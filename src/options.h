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

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class AccelStructGroupingTarget
{
    Meshes,
    Scenes,
    Hybrid,
};

// Where a SceneLevel's distance is measured from, in the hierarchy of scene instances formed by
// external scene references.
enum class SceneLevelOrigin
{
    FromLeaves, // height: edges down to the instance's deepest descendant
    FromRoot,   // root depth: edges up to the top-level scene, which has root depth 0
};

// Selects the scene instances a scene grouping wraps: those at exactly `distance` from `origin`.
struct SceneLevel
{
    SceneLevelOrigin origin = SceneLevelOrigin::FromLeaves;
    uint32_t distance = 0;

    bool operator==(const SceneLevel& other) const = default;
};

struct AccelStructGrouping
{
    AccelStructGroupingTarget target = AccelStructGroupingTarget::Meshes;
    // Only meaningful for the Scenes target.
    SceneLevel sceneLevel;
};

// How many MLAS grouping levels a plan may request. This is a limitation of the MLAS extension.
constexpr uint32_t kMaxMlasLevels = 2;

// Plan for how to build acceleration structures for the scene
struct AccelStructHighLevelPlan
{
    AccelStructGrouping blasLevel;
    // Each MLAS level groups by scene; the value selects the scene instances that level wraps.
    std::vector<SceneLevel> mlasLevels;
};

enum class AccelStructDebugMode : uint32_t
{
    Full,
    SelectedBlas,
    SelectedMlas,
};

// How animated vertex data reaches the acceleration structures.
enum class DeformMode
{
    Auto,      // skinned meshes use the compute path, rigid nodes use transforms
    Compute,   // force the compute deformation path
    Transform, // ignore skinning and animate with node transforms only
};

struct Options
{
    std::string scenePath;
    std::wstring outputPath; // when set, render headless and exit
    uint32_t width = 1280;
    uint32_t height = 720;
    float time = 0.0f;
    uint32_t frames = 1;
    uint32_t benchmarkFrames = 0;
    uint32_t warmupFrames = 2;

    AccelStructHighLevelPlan accelStructPlan;
    DeformMode deformMode = DeformMode::Auto;

    uint32_t maxBounces = 3;
    float sunDirection[3] = {-0.45f, -0.8f, -0.4f};
    std::optional<std::array<float, 6>> camera; // eyeX,eyeY,eyeZ, targetX,targetY,targetZ

    // Set when --help or -h was given: usage has been printed and there is nothing to run.
    bool helpRequested = false;

    bool debugLayer = false;
    bool vsync = true;
    bool mlasFullBarriers = false;

    // Throws std::runtime_error with a usage message on invalid input.
    static Options Parse(int argc, char** argv);
    static std::string Usage();
};
