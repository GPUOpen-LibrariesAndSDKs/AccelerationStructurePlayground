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

#include "backends/vulkan/core/swapchain.h"

namespace vulkan
{
void SwapChain::Create(Gpu& gpu, HWND hwnd, uint32_t width, uint32_t height, bool vsync)
{
    m_gpu = &gpu;
    m_width = width;
    m_height = height;

    VkFormatProperties outputProperties{};
    vkGetPhysicalDeviceFormatProperties(gpu.PhysicalDevice(), VK_FORMAT_R8G8B8A8_UNORM, &outputProperties);
    if (!(outputProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
    {
        Fatal("The Vulkan device cannot blit from the RGBA8 ray tracing output.");
    }

    VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.hinstance = GetModuleHandleW(nullptr);
    surfaceInfo.hwnd = hwnd;
    VK_CHECK(vkCreateWin32SurfaceKHR(gpu.Instance(), &surfaceInfo, nullptr, &m_surface));

    VkBool32 supported = VK_FALSE;
    VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(gpu.PhysicalDevice(), gpu.QueueFamily(), m_surface, &supported));
    if (!supported)
    {
        Fatal("The Vulkan queue cannot present to the window surface.");
    }

    uint32_t formatCount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.PhysicalDevice(), m_surface, &formatCount, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.PhysicalDevice(), m_surface, &formatCount, formats.data()));
    for (VkFormat preferred : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM})
    {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(gpu.PhysicalDevice(), preferred, &properties);
        if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
        {
            continue;
        }
        for (const VkSurfaceFormatKHR& format : formats)
        {
            if (format.format == preferred && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                m_format = format.format;
                m_colorSpace = format.colorSpace;
                break;
            }
        }
        if (m_format != VK_FORMAT_UNDEFINED)
        {
            break;
        }
    }
    if (m_format == VK_FORMAT_UNDEFINED)
    {
        Fatal("The window surface supports neither RGBA8 nor BGRA8 UNORM presentation.");
    }

    uint32_t modeCount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(gpu.PhysicalDevice(), m_surface, &modeCount, nullptr));
    std::vector<VkPresentModeKHR> modes(modeCount);
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(gpu.PhysicalDevice(), m_surface, &modeCount, modes.data()));
    m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync)
    {
        for (VkPresentModeKHR preferred : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
        {
            if (std::find(modes.begin(), modes.end(), preferred) != modes.end())
            {
                m_presentMode = preferred;
                break;
            }
        }
    }

    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkSemaphore& semaphore : m_acquireSemaphores)
    {
        VK_CHECK(vkCreateSemaphore(gpu.Device(), &semaphoreInfo, nullptr, &semaphore));
    }
    CreateSwapchain();
}

void SwapChain::CreateSwapchain()
{
    const VkDevice device = m_gpu->Device();
    VkSurfaceCapabilitiesKHR capabilities{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_gpu->PhysicalDevice(), m_surface, &capabilities));
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
    {
        Fatal("The Vulkan window surface does not support transfer-destination swapchain images.");
    }
    VkExtent2D extent{m_width, m_height};
    if (capabilities.currentExtent.width != UINT32_MAX)
    {
        extent = capabilities.currentExtent;
    }
    extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    m_width = extent.width;
    m_height = extent.height;

    uint32_t imageCount = std::max(capabilities.minImageCount + 1, 2u);
    if (capabilities.maxImageCount > 0)
    {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    const VkSwapchainKHR oldSwapchain = m_swapchain;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = m_surface;
    info.minImageCount = imageCount;
    info.imageFormat = m_format;
    info.imageColorSpace = m_colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = m_presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;
    VK_CHECK(vkCreateSwapchainKHR(device, &info, nullptr, &m_swapchain));

    DestroyImageResources();
    if (oldSwapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
    }

    uint32_t count = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(device, m_swapchain, &count, nullptr));
    m_images.resize(count);
    VK_CHECK(vkGetSwapchainImagesKHR(device, m_swapchain, &count, m_images.data()));
    m_views.resize(count);
    m_renderFinishedSemaphores.resize(count);
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t index = 0; index < count; ++index)
    {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = m_images[index];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &m_views[index]));
        VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[index]));
    }
    m_imageIndex = 0;
}

void SwapChain::Resize(uint32_t width, uint32_t height)
{
    VK_CHECK(vkDeviceWaitIdle(m_gpu->Device()));
    m_width = width;
    m_height = height;
    CreateSwapchain();
}

void SwapChain::DestroyImageResources()
{
    const VkDevice device = m_gpu->Device();
    for (VkImageView view : m_views)
    {
        vkDestroyImageView(device, view, nullptr);
    }
    for (VkSemaphore semaphore : m_renderFinishedSemaphores)
    {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    m_views.clear();
    m_renderFinishedSemaphores.clear();
    m_images.clear();
}

void SwapChain::Destroy()
{
    if (!m_gpu)
    {
        return;
    }
    const VkDevice device = m_gpu->Device();
    vkDeviceWaitIdle(device);
    DestroyImageResources();
    if (m_swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    for (VkSemaphore& semaphore : m_acquireSemaphores)
    {
        vkDestroySemaphore(device, semaphore, nullptr);
        semaphore = VK_NULL_HANDLE;
    }
    if (m_surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(m_gpu->Instance(), m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    m_gpu = nullptr;
}

bool SwapChain::Acquire(uint32_t frameSlot)
{
    const VkResult result = vkAcquireNextImageKHR(m_gpu->Device(), m_swapchain, UINT64_MAX,
                                                  m_acquireSemaphores[frameSlot], VK_NULL_HANDLE, &m_imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        return false;
    }
    VK_CHECK(result);
    return true;
}

void SwapChain::Present()
{
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &m_renderFinishedSemaphores[m_imageIndex];
    info.swapchainCount = 1;
    info.pSwapchains = &m_swapchain;
    info.pImageIndices = &m_imageIndex;
    const VkResult result = vkQueuePresentKHR(m_gpu->Queue(), &info);
    if (result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_SUBOPTIMAL_KHR)
    {
        VK_CHECK(result);
    }
}
} // namespace vulkan
