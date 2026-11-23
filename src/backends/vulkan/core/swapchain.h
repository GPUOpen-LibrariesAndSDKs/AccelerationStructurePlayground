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

#include "backends/vulkan/core/gpu.h"

#include <vector>

namespace vulkan
{
// FIFO (vsync) or immediate/mailbox swap chain. The ray tracing output is blitted into the
// acquired image, then ImGui renders on top with dynamic rendering.
class SwapChain
{
public:
    ~SwapChain() { Destroy(); }

    void Create(Gpu& gpu, HWND hwnd, uint32_t width, uint32_t height, bool vsync);
    void Resize(uint32_t width, uint32_t height);
    void Destroy();

    // Returns false when the swap chain is out of date and this frame cannot be presented.
    bool Acquire(uint32_t frameSlot);
    void Present();

    VkImage Image() const { return m_images[m_imageIndex]; }
    VkImageView ImageView() const { return m_views[m_imageIndex]; }
    VkSemaphore AcquireSemaphore(uint32_t frameSlot) const { return m_acquireSemaphores[frameSlot]; }
    VkSemaphore RenderFinishedSemaphore() const { return m_renderFinishedSemaphores[m_imageIndex]; }
    VkFormat Format() const { return m_format; }
    uint32_t ImageCount() const { return static_cast<uint32_t>(m_images.size()); }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

private:
    void CreateSwapchain();
    void DestroyImageResources();

    Gpu* m_gpu = nullptr;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR m_colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_views;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    VkSemaphore m_acquireSemaphores[Gpu::kFrameCount]{};
    uint32_t m_imageIndex = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};
} // namespace vulkan
