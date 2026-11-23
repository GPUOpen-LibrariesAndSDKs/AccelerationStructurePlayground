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

#include "app.h"

#include "assets/gltf_state.h"
#include "core/error.h"
#include "core/image_write.h"
#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

App::App(Options options) : m_options(std::move(options)), m_width(m_options.width), m_height(m_options.height)
{
    if (m_options.scenePath.empty())
    {
        Fatal("No scene file given.\n\n%s", Options::Usage().c_str());
    }
    m_scene = std::make_unique<Scene>(m_options.scenePath);
    m_scene->SetAnimationTime(m_options.time);
    const Bounds bounds = m_scene->WorldBounds();
    m_sceneRadius = bounds.Radius();
    m_camera = Camera::FrameBounds(bounds);
    if (m_options.camera)
    {
        const auto& values = *m_options.camera;
        m_camera.position = {values[0], values[1], values[2]};
        m_camera.target = {values[3], values[4], values[5]};
    }

    size_t triangleCount = 0;
    size_t primitiveCount = 0;
    size_t nodeCount = 0;
    size_t meshCount = 0;
    size_t materialCount = 0;
    size_t textureCount = 0;
    size_t skinCount = 0;
    size_t animationCount = 0;
    for (const GltfAsset* asset : m_scene->Assets())
    {
        nodeCount += asset->nodes.size();
        meshCount += asset->meshes.size();
        materialCount += asset->materials.size();
        textureCount += asset->textures.size();
        skinCount += asset->skins.size();
        animationCount += asset->animations.size();
        for (const auto& mesh : asset->meshes)
        {
            for (const auto& primitive : mesh->primitives)
            {
                const size_t indexCount =
                    primitive->pIndices ? primitive->pIndices->count : primitive->pPosition->count;
                triangleCount += indexCount / 3;
                ++primitiveCount;
            }
        }
    }
    printf("Scene: %s\n", m_options.scenePath.c_str());
    printf("  assets %zu, playback groups %zu, scene nodes %zu\n", m_scene->Assets().size(),
           m_scene->PlaybackGroups().size(), m_scene->SceneNodes().size());
    printf("  nodes %zu, meshes %zu, primitives %zu, triangles %zu\n", nodeCount, meshCount, primitiveCount,
           triangleCount);
    printf("  materials %zu, textures %zu, skins %zu, animations %zu\n", materialCount, textureCount, skinCount,
           animationCount);
    printf("  bounds [%.3f %.3f %.3f] .. [%.3f %.3f %.3f], radius %.3f\n", bounds.min.x, bounds.min.y, bounds.min.z,
           bounds.max.x, bounds.max.y, bounds.max.z, m_sceneRadius);
    printf("  camera eye [%.3f %.3f %.3f] target [%.3f %.3f %.3f]\n", m_camera.position.x, m_camera.position.y,
           m_camera.position.z, m_camera.target.x, m_camera.target.y, m_camera.target.z);

    if (m_options.outputPath.empty() && m_options.benchmarkFrames == 0)
    {
        m_window = std::make_unique<Window>(L"Acceleration Structure Playground", m_width, m_height);
        m_gui.Create(*m_window);
    }
    BackendOptions backendOptions;
    backendOptions.width = m_width;
    backendOptions.height = m_height;
    backendOptions.maxBounces = m_options.maxBounces;
    backendOptions.debugLayer = m_options.debugLayer;
    backendOptions.vsync = m_options.vsync;
    backendOptions.mlasFullBarriers = m_options.mlasFullBarriers;
    backendOptions.measureFrameTime = m_options.benchmarkFrames != 0;
    m_backend = CreateRenderBackend(backendOptions, m_window.get());
    printf("Adapter: %s\n", m_backend->Capabilities().adapter.c_str());
    if (m_window)
    {
        m_window->onResize = [this](uint32_t width, uint32_t height) {
            m_backend->Resize(width, height);
            m_width = width;
            m_height = height;
        };
    }

    const auto plan =
        BuildAccelStructPlan(*m_scene, m_options.deformMode, m_options.accelStructPlan);
    m_backend->SetScene(*m_scene, m_options.deformMode, plan);
    const auto& diagnostics = m_backend->Diagnostics();
    printf("Acceleration structures: %zu BLAS (%u animated), %zu TLAS instances, %u geometries\n",
           diagnostics.blases.size(), diagnostics.animatedBlasCount, plan.instances.size(), diagnostics.geometryCount);
    if (diagnostics.deformedPrimitiveCount > 0)
    {
        printf("Compute skinning: %u playback-group primitives, %u vertices\n", diagnostics.deformedPrimitiveCount,
               diagnostics.deformedVertexCount);
    }
}

App::~App() = default;

FrameInput App::BuildFrameInput() const
{
    FrameInput frame;
    m_camera.Basis(frame.cameraForward, frame.cameraRight, frame.cameraUp);
    frame.cameraPosition = m_camera.position;
    frame.tanHalfFovY = std::tan(0.5f * m_camera.fovY);
    frame.rayEpsilon = 1e-3f; // std::max(1e-4f, m_sceneRadius * 1e-3f);
    frame.maxBounces = m_options.maxBounces;
    frame.sunDirection =
        Normalize(float3(m_options.sunDirection[0], m_options.sunDirection[1], m_options.sunDirection[2]));
    frame.debug = m_debug;
    return frame;
}

void App::UpdateCamera(float deltaTime)
{
    InputState& input = m_window->Input();
    if (input.leftDown && !m_gui.WantCaptureMouse())
    {
        constexpr float kLookSpeed = 0.0035f;
        m_camera.Rotate(input.mouseDeltaX * kLookSpeed, input.mouseDeltaY * kLookSpeed);
    }
    if (m_gui.WantCaptureKeyboard())
    {
        return;
    }
    float3 localDelta{0.0f, 0.0f, 0.0f};
    if (input.moveForward) localDelta.z += 1.0f;
    if (input.moveBack) localDelta.z -= 1.0f;
    if (input.moveRight) localDelta.x += 1.0f;
    if (input.moveLeft) localDelta.x -= 1.0f;
    if (localDelta.x != 0.0f || localDelta.z != 0.0f)
    {
        const float speed = std::max(1.0f, m_sceneRadius) * 0.5f;
        m_camera.Move(Normalize(localDelta) * (speed * deltaTime));
    }
}

std::optional<CompletedFrameTimings> App::RenderFrame(float time, ImDrawData* gui)
{
    m_scene->SetAnimationTime(time);
    return m_backend->RenderFrame(BuildFrameInput(), gui);
}

int App::Run()
{
    if (m_options.benchmarkFrames != 0)
    {
        return RunBenchmark();
    }
    return m_options.outputPath.empty() ? RunInteractive() : RunHeadless();
}

int App::RunBenchmark()
{
    std::vector<double> frameMs;
    std::vector<double> blasMs;
    std::vector<double> mlasTlasMs;
    std::vector<double> traceMs;
    while (frameMs.size() < m_options.benchmarkFrames)
    {
        const auto completed = RenderFrame(m_options.time);
        if (completed && completed->frameId >= m_options.warmupFrames)
        {
            frameMs.push_back(completed->frameMs);
            blasMs.push_back(completed->blasMs);
            mlasTlasMs.push_back(completed->mlasTlasMs);
            traceMs.push_back(completed->traceMs);
        }
    }
    m_backend->WaitIdle();
    const auto timingStats = [](const std::vector<double>& samples) {
        const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
        std::vector<double> sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        const size_t middle = sorted.size() / 2;
        const double median = sorted.size() % 2 == 0 ? (sorted[middle - 1] + sorted[middle]) / 2.0 : sorted[middle];
        return nlohmann::json{{"mean", mean}, {"median", median}, {"all", samples}};
    };
    const auto& diagnostics = m_backend->Diagnostics();
    const auto tlas = diagnostics.tlas;
    AccelStructSizes mlases;
    AccelStructSizes blases;
    for (const auto& sizes : diagnostics.mlases)
    {
        mlases.accelStructBytes += sizes.accelStructBytes;
        mlases.scratchBytes += sizes.scratchBytes;
    }
    for (const auto& sizes : diagnostics.blases)
    {
        blases.accelStructBytes += sizes.accelStructBytes;
        blases.scratchBytes += sizes.scratchBytes;
    }
    const AccelStructSizes total{tlas.accelStructBytes + mlases.accelStructBytes +
                                               blases.accelStructBytes,
                                           tlas.scratchBytes + mlases.scratchBytes + blases.scratchBytes};
    const auto memory = [](const AccelStructSizes& sizes) {
        return nlohmann::json{{"result", sizes.accelStructBytes}, {"scratch", sizes.scratchBytes}};
    };
    nlohmann::json accelStructMemory{{"tlas", memory(tlas)},
                                     {"mlas", memory(mlases)},
                                     {"blas", memory(blases)},
                                     {"total", memory(total)}};
    nlohmann::json gpuTimings{{"frame", timingStats(frameMs)},
                              {"blas_build_refit", timingStats(blasMs)},
                              {"mlas_tlas_build", timingStats(mlasTlasMs)},
                              {"ray_tracing", timingStats(traceMs)}};
    m_benchmarkJson = nlohmann::json{{"adapter", m_backend->Capabilities().adapter},
                                     {"benchmark_frames", m_options.benchmarkFrames},
                                     {"warmup_frames", m_options.warmupFrames},
                                     {"width", m_width},
                                     {"height", m_height},
                                     {"gpu_ms", std::move(gpuTimings)},
                                     {"acceleration_structure_memory_bytes", std::move(accelStructMemory)}}
                          .dump();
    return 0;
}

int App::RunHeadless()
{
    for (uint32_t frame = 0; frame < m_options.frames; ++frame)
    {
        RenderFrame(m_options.time);
    }
    SaveOutputPng(m_options.outputPath);
    printf("Wrote %s (%ux%u)\n", Narrow(m_options.outputPath).c_str(), m_width, m_height);
    return 0;
}

int App::RunInteractive()
{
    auto previous = std::chrono::steady_clock::now();
    auto previousFrameEnd = previous;
    while (m_window->PumpMessages())
    {
        m_backend->BeginGuiFrame();
        m_gui.BeginFrame();
        const auto request = m_gui.Draw(*m_scene, m_backend->Diagnostics(), m_options.accelStructPlan,
                                        m_backend->Capabilities());
        m_debug = request.debug;
        if (request.rebuild)
        {
            RebuildAccelStructs(request.highLevelPlan);
        }
        const auto now = std::chrono::steady_clock::now();
        const float deltaTime = std::chrono::duration<float>(now - previous).count();
        previous = now;
        if (m_window->Input().togglePausePressed && !m_gui.WantCaptureKeyboard())
        {
            m_animationPaused = !m_animationPaused;
        }
        if (!m_animationPaused)
        {
            m_animationTime += deltaTime;
        }
        UpdateCamera(deltaTime);
        RenderFrame(m_animationTime, m_gui.EndFrame());
        const auto frameEnd = std::chrono::steady_clock::now();
        m_gui.RecordFrameTime(std::chrono::duration<float, std::milli>(frameEnd - previousFrameEnd).count());
        previousFrameEnd = frameEnd;
    }
    m_backend->WaitIdle();
    return 0;
}

void App::RebuildAccelStructs(const AccelStructHighLevelPlan& highLevelPlan)
{
    const auto plan = BuildAccelStructPlan(*m_scene, m_options.deformMode, highLevelPlan);
    m_backend->RebuildAccelStructs(plan);
    m_options.accelStructPlan = highLevelPlan;
    const auto& diagnostics = m_backend->Diagnostics();
    printf("Acceleration structures rebuilt (high-level plan): %zu BLAS (%u animated), %zu TLAS instances, %u "
           "geometries\n",
           diagnostics.blases.size(), diagnostics.animatedBlasCount, plan.instances.size(), diagnostics.geometryCount);
}

void App::SaveOutputPng(const std::wstring& path)
{
    const auto image = m_backend->ReadOutput();
    SavePng(path, image.rgba.data(), image.width, image.height, image.rowPitch);
}
