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

#include "core/window.h"
#include "core/error.h"

#include <windowsx.h>

namespace
{
constexpr const wchar_t* kClassName = L"AccelerationStructurePlaygroundWindow";
}

Window::Window(const wchar_t* title, uint32_t width, uint32_t height) : m_width(width), m_height(height)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &Window::WndProcThunk;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);

    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(0, kClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, this);
    if (!m_hwnd)
    {
        Fatal("CreateWindowEx failed (0x%08lX).", GetLastError());
    }

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
}

Window::~Window()
{
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
    }
    UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
}

bool Window::PumpMessages()
{
    m_input.BeginFrame();

    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return !m_closed;
}

LRESULT CALLBACK Window::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!window)
    {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return window->HandleMessage(hwnd, message, wParam, lParam);
}

LRESULT Window::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (wndProcHook && wndProcHook(hwnd, message, wParam, lParam))
    {
        return 1;
    }

    switch (message)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            m_width = LOWORD(lParam);
            m_height = HIWORD(lParam);
            if (onResize && m_width > 0 && m_height > 0) onResize(m_width, m_height);
        }
        return 0;

    case WM_MOUSEMOVE: {
        const POINT current{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        m_input.mouseDeltaX += static_cast<float>(current.x - m_lastMouse.x);
        m_input.mouseDeltaY += static_cast<float>(current.y - m_lastMouse.y);
        m_lastMouse = current;
        return 0;
    }

    case WM_MOUSEWHEEL:
        m_input.wheelDelta += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        m_input.leftDown = (wParam & MK_LBUTTON) != 0;
        m_input.rightDown = (wParam & MK_RBUTTON) != 0;
        m_input.middleDown = (wParam & MK_MBUTTON) != 0;
        SetCapture(m_input.leftDown || m_input.rightDown || m_input.middleDown ? hwnd : nullptr);
        if (!m_input.leftDown && !m_input.rightDown && !m_input.middleDown)
        {
            ReleaseCapture();
        }
        return 0;

    case WM_KEYDOWN:
    case WM_KEYUP: {
        const bool down = message == WM_KEYDOWN;
        switch (wParam)
        {
        case VK_ESCAPE:
            if (down) m_closed = true;
            break;
        case 'W':
            m_input.moveForward = down;
            break;
        case 'S':
            m_input.moveBack = down;
            break;
        case 'A':
            m_input.moveLeft = down;
            break;
        case 'D':
            m_input.moveRight = down;
            break;
        case VK_SPACE:
            // Bit 30 of lParam is set for auto-repeat key-down messages; ignore those.
            if (down && (lParam & (1 << 30)) == 0) m_input.togglePausePressed = true;
            break;
        default:
            break;
        }
        return 0;
    }

    case WM_KILLFOCUS:
        // Release held keys/buttons so the camera doesn't keep moving after alt-tab.
        m_input = InputState{};
        return 0;

    case WM_CLOSE:
    case WM_DESTROY:
        m_closed = true;
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
