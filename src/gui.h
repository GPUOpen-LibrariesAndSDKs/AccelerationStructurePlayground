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

#include "options.h"
#include "render/backend.h"

#include <vector>

class Window;
class Scene;
struct ImDrawData;

// Summary of user input from calling Gui::Draw
struct AccelStructPlanRequest
{
    bool rebuild = false;
    AccelStructHighLevelPlan highLevelPlan;
    DebugVisualization debug;
};

class Gui
{
public:
    ~Gui();
    void Create(Window& window);
    void Destroy();
    void BeginFrame();
    void RecordFrameTime(float milliseconds) { m_frameTiming.AddSample(milliseconds); }
    AccelStructPlanRequest Draw(const Scene& scene, const RenderDiagnostics& diagnostics,
                                const AccelStructHighLevelPlan& highLevelPlan,
                                const BackendCapabilities& capabilities);
    ImDrawData* EndFrame();

    // True while ImGui wants exclusive use of the mouse/keyboard
    bool WantCaptureMouse() const;
    bool WantCaptureKeyboard() const;

private:
    // Context needed for drawing the different panels in the GUI
    struct PanelContext
    {
        const Scene& scene;
        const RenderDiagnostics& accelStruct;
        const BackendCapabilities& capabilities;
        AccelStructPlanRequest& request;
    };

    void DrawPerformancePanel(const PanelContext& context);
    void DrawMemoryPanel(const PanelContext& context);
    void DrawPlanPanel(const PanelContext& context);
    void DrawDebugPanel(const PanelContext& context);
    void DrawObjectsPanel(const PanelContext& context);
    void DrawTlasTreePanel(const PanelContext& context);

    TimingSnapshot m_frameTiming;
    Window* m_window = nullptr;
    bool m_created = false;
    bool m_planInitialized = false;
    AccelStructHighLevelPlan m_pendingHighLevelPlan;
    int m_pendingMlasLevelCount = 0;
    DebugVisualization m_debug;
};