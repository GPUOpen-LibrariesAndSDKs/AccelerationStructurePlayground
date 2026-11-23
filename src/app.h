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

#include "core/window.h"
#include "gui.h"
#include "options.h"
#include "render/backend.h"
#include "render/camera.h"
#include "assets/scene.h"

#include <memory>

class App
{
public:
    explicit App(Options options);
    ~App();

    int Run();
    const std::string& BenchmarkJson() const { return m_benchmarkJson; }

private:
    int RunHeadless();
    int RunBenchmark();
    int RunInteractive();

    std::optional<CompletedFrameTimings> RenderFrame(float time, ImDrawData* gui = nullptr);
    void UpdateCamera(float deltaTime);
    FrameInput BuildFrameInput() const;

    void SaveOutputPng(const std::wstring& path);
    void RebuildAccelStructs(const AccelStructHighLevelPlan& highLevelPlan);

    Options m_options;
    std::string m_benchmarkJson;
    std::unique_ptr<Scene> m_scene;
    Camera m_camera;
    float m_sceneRadius = 1.0f;
    float m_animationTime = 0.0f;
    bool m_animationPaused = false;
    DebugVisualization m_debug;

    std::unique_ptr<Window> m_window;
    Gui m_gui;
    std::unique_ptr<IRenderBackend> m_backend;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
};
