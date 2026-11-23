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

#include "core/platform_win32.h"

#include <functional>

// Per-frame input snapshot consumed by the camera controller.
struct InputState
{
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    float wheelDelta = 0.0f;
    bool leftDown = false;
    bool rightDown = false;
    bool middleDown = false;
    bool moveForward = false;
    bool moveBack = false;
    bool moveLeft = false;
    bool moveRight = false;
    // One-shot event: true for exactly one frame after the spacebar is pressed.
    bool togglePausePressed = false;

    void BeginFrame()
    {
        mouseDeltaX = 0.0f;
        mouseDeltaY = 0.0f;
        wheelDelta = 0.0f;
        togglePausePressed = false;
    }
};

class Window
{
public:
    Window(const wchar_t* title, uint32_t width, uint32_t height);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Drains the message queue; returns false once the window has been closed.
    bool PumpMessages();

    HWND Handle() const { return m_hwnd; }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    InputState& Input() { return m_input; }

    std::function<void(uint32_t, uint32_t)> onResize;
    // Installed by the ImGui layer so it can consume input before the camera does.
    std::function<bool(HWND, UINT, WPARAM, LPARAM)> wndProcHook;

private:
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_closed = false;
    POINT m_lastMouse{};
    InputState m_input;
};
