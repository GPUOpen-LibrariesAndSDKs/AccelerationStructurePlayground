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

#include "render/backend.h"

#include "backends/vulkan/core/gpu.h"
#include "backends/vulkan/core/swapchain.h"
#include "core/window.h"
#include "backends/vulkan/render/accel_struct.h"
#include "backends/vulkan/render/deform.h"
#include "backends/vulkan/render/raytracer.h"
#include "backends/vulkan/render/asset_gpu.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cstring>

namespace vulkan
{
namespace
{
class VulkanBackend final : public IRenderBackend
{
public:
    VulkanBackend(const BackendOptions& options, Window* window);
    ~VulkanBackend() override;

    const BackendCapabilities& Capabilities() const override { return m_capabilities; }
    const RenderDiagnostics& Diagnostics() const override { return m_diagnostics; }
    void SetScene(const Scene& scene, DeformMode deformMode, const AccelStructPlan& plan) override;
    void RebuildAccelStructs(const AccelStructPlan& plan) override;
    void Resize(uint32_t width, uint32_t height) override;
    void BeginGuiFrame() override;
    std::optional<CompletedFrameTimings> RenderFrame(const FrameInput& frame, ImDrawData* gui) override;
    OutputImage ReadOutput() override;
    void WaitIdle() override { m_gpu.WaitForGpu(); }

private:
    void CreateOutput(uint32_t width, uint32_t height);
    void CaptureStructureDiagnostics();
    void CaptureFrameDiagnostics();
    void CreateImGuiRenderer();
    void DestroyImGuiRenderer();
    void Present(VkCommandBuffer commandBuffer, ImDrawData* gui);

    BackendOptions m_options;
    BackendCapabilities m_capabilities;
    RenderDiagnostics m_diagnostics;
    const Scene* m_scene = nullptr;
    Gpu m_gpu;
    std::vector<AssetGpu> m_assetGpus;
    std::vector<Deformer> m_deformers;
    AccelStructs m_accelStruct;
    RayTracer m_rayTracer;
    SwapChain m_swapChain;
    Image m_output;
    bool m_present = false;
    bool m_guiCreated = false;
    bool m_imguiRenderer = false;
    uint64_t m_frameNumber = 0;
    VkQueryPool m_frameQueries = VK_NULL_HANDLE;
};

VulkanBackend::VulkanBackend(const BackendOptions& options, Window* window) : m_options(options)
{
    m_gpu.Create(options.debugLayer, window != nullptr);
    m_capabilities.adapter = m_gpu.AdapterName();
    m_capabilities.maxMlasLevels = m_gpu.MlasEnabled() ? kMaxMlasLevels : 0;

    m_rayTracer.Create(m_gpu);
    CreateOutput(options.width, options.height);
    if (options.measureFrameTime)
    {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = Gpu::kFrameCount * 2;
        VK_CHECK(vkCreateQueryPool(m_gpu.Device(), &info, nullptr, &m_frameQueries));
    }
    if (window)
    {
        m_swapChain.Create(m_gpu, window->Handle(), options.width, options.height, options.vsync);
        m_present = true;
        CreateImGuiRenderer();
    }
}

VulkanBackend::~VulkanBackend()
{
    m_gpu.WaitForGpu();
    DestroyImGuiRenderer();
    if (m_frameQueries != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(m_gpu.Device(), m_frameQueries, nullptr);
    }
}

void VulkanBackend::CreateOutput(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        Fatal("Render output dimensions must be nonzero.");
    }
    m_gpu.WaitForGpu();
    m_options.width = width;
    m_options.height = height;
    m_output = m_gpu.CreateImage2D(VK_FORMAT_R8G8B8A8_UNORM, width, height,
                                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "RayTracingOutput");
    VkCommandBuffer commandBuffer = m_gpu.Begin();
    Gpu::ImageBarrier(commandBuffer, m_output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    m_gpu.EndAndSubmit();
}

void VulkanBackend::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        Fatal("Render output dimensions must be nonzero.");
    }
    m_gpu.WaitForGpu();
    if (m_present)
    {
        m_swapChain.Resize(width, height);
        width = m_swapChain.Width();
        height = m_swapChain.Height();
    }
    CreateOutput(width, height);
}

void VulkanBackend::CreateImGuiRenderer()
{
    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount = 1;
    const VkFormat format = m_swapChain.Format();
    renderingInfo.pColorAttachmentFormats = &format;

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = m_gpu.Instance();
    info.PhysicalDevice = m_gpu.PhysicalDevice();
    info.Device = m_gpu.Device();
    info.QueueFamily = m_gpu.QueueFamily();
    info.Queue = m_gpu.Queue();
    info.DescriptorPoolSize = 128;
    info.MinImageCount = std::min(2u, m_swapChain.ImageCount());
    info.ImageCount = m_swapChain.ImageCount();
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo;
    if (!ImGui_ImplVulkan_Init(&info))
    {
        Fatal("Could not initialize the Vulkan ImGui renderer.");
    }
    m_imguiRenderer = true;
}

void VulkanBackend::DestroyImGuiRenderer()
{
    if (m_imguiRenderer)
    {
        ImGui_ImplVulkan_Shutdown();
        m_imguiRenderer = false;
    }
}

void VulkanBackend::SetScene(const Scene& scene, DeformMode deformMode, const AccelStructPlan& plan)
{
    if (m_scene)
    {
        Fatal("A rendering backend can only realize one scene. Use RebuildAccelStructs to change its plan.");
    }
    ValidateAccelStructPlan(plan, scene);
    ValidateBackendPlan(m_capabilities, plan);
    m_scene = &scene;

    VkCommandBuffer commandBuffer = m_gpu.Begin();
    m_assetGpus.resize(scene.Assets().size());
    for (size_t index = 0; index < scene.Assets().size(); ++index)
    {
        m_assetGpus[index].Create(m_gpu, *scene.Assets()[index], deformMode != DeformMode::Transform);
    }
    m_deformers.resize(scene.PlaybackGroups().size());
    for (size_t index = 0; index < scene.PlaybackGroups().size(); ++index)
    {
        const auto& group = scene.PlaybackGroups()[index];
        const auto& asset = scene.Asset(group.assetId);
        m_deformers[index].Create(m_gpu, asset, m_assetGpus[group.assetId.value]);
        m_deformers[index].Update(m_gpu, commandBuffer, asset, group.State());
    }
    m_accelStruct.SetMlasFullBarriers(m_options.mlasFullBarriers);
    m_accelStruct.Create(m_gpu, commandBuffer, scene, m_assetGpus, m_deformers, plan);
    m_gpu.EndAndSubmit();
    m_gpu.WaitForGpu();
    CaptureStructureDiagnostics();
}

void VulkanBackend::RebuildAccelStructs(const AccelStructPlan& plan)
{
    if (!m_scene)
    {
        Fatal("SetScene must be called before rebuilding acceleration structures.");
    }
    ValidateAccelStructPlan(plan, *m_scene);
    ValidateBackendPlan(m_capabilities, plan);
    m_gpu.WaitForGpu();
    m_accelStruct.Reset();
    VkCommandBuffer commandBuffer = m_gpu.Begin();
    m_accelStruct.SetMlasFullBarriers(m_options.mlasFullBarriers);
    m_accelStruct.Create(m_gpu, commandBuffer, *m_scene, m_assetGpus, m_deformers, plan);
    m_gpu.EndAndSubmit();
    m_gpu.WaitForGpu();
    CaptureStructureDiagnostics();
}

void VulkanBackend::CaptureStructureDiagnostics()
{
    m_diagnostics.plan = m_accelStruct.Plan();
    m_diagnostics.blases.resize(m_accelStruct.BlasCount());
    for (uint32_t index = 0; index < m_accelStruct.BlasCount(); ++index)
    {
        m_diagnostics.blases[index] = m_accelStruct.BlasSizes(index);
    }
    m_diagnostics.mlases.resize(m_accelStruct.MlasCount());
    for (uint32_t index = 0; index < m_accelStruct.MlasCount(); ++index)
    {
        m_diagnostics.mlases[index] = m_accelStruct.MlasSizes(index);
    }
    m_diagnostics.tlas = m_accelStruct.TlasSizes();
    m_diagnostics.animatedBlasCount = m_accelStruct.AnimatedBlasCount();
    m_diagnostics.geometryCount = m_accelStruct.GeometryCount();
    m_diagnostics.deformedPrimitiveCount = 0;
    m_diagnostics.deformedVertexCount = 0;
    for (const Deformer& deformer : m_deformers)
    {
        m_diagnostics.deformedPrimitiveCount += deformer.PrimitiveCount();
        m_diagnostics.deformedVertexCount += deformer.VertexCount();
    }
    CaptureFrameDiagnostics();
}

void VulkanBackend::CaptureFrameDiagnostics()
{
    m_diagnostics.blasBuild = MakeTimingSnapshot(m_accelStruct.BlasBuildTiming());
    m_diagnostics.mlasTlasBuild = MakeTimingSnapshot(m_accelStruct.MlasTlasBuildTiming());
    m_diagnostics.totalBuild = MakeTimingSnapshot(m_accelStruct.TotalBuildTiming());
    m_diagnostics.traceRays = MakeTimingSnapshot(m_rayTracer.TraceRaysTiming());
}

void VulkanBackend::BeginGuiFrame()
{
    if (!m_imguiRenderer)
    {
        Fatal("GUI rendering requires an interactive backend.");
    }
    ImGui_ImplVulkan_NewFrame();
}

std::optional<CompletedFrameTimings> VulkanBackend::RenderFrame(const FrameInput& frame, ImDrawData* gui)
{
    if (!m_scene)
    {
        Fatal("SetScene must be called before rendering.");
    }
    if (gui && !m_imguiRenderer)
    {
        Fatal("ImGui draw data requires an interactive backend.");
    }

    VkCommandBuffer commandBuffer = m_gpu.Begin();
    std::optional<CompletedFrameTimings> completed;
    const uint32_t ring = m_gpu.FrameIndex();
    if (m_present && !m_swapChain.Acquire(ring))
    {
        m_gpu.Cancel();
        Resize(m_options.width, m_options.height);
        return std::nullopt;
    }
    const uint32_t queryBase = ring * 2;
    if (m_frameQueries != VK_NULL_HANDLE)
    {
        if (m_frameNumber >= Gpu::kFrameCount)
        {
            uint64_t timestamps[2]{};
            if (vkGetQueryPoolResults(m_gpu.Device(), m_frameQueries, queryBase, 2, sizeof(timestamps), timestamps,
                                      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                completed = CompletedFrameTimings{};
                completed->frameId = m_frameNumber - Gpu::kFrameCount;
                completed->frameMs = (timestamps[1] - timestamps[0]) * m_gpu.TimestampPeriodNs() * 1e-6;
            }
        }
        vkCmdResetQueryPool(commandBuffer, m_frameQueries, queryBase, 2);
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_frameQueries, queryBase);
    }

    m_accelStruct.SetDebugVisualization(frame.debug.selection);
    for (size_t index = 0; index < m_scene->PlaybackGroups().size(); ++index)
    {
        const auto& group = m_scene->PlaybackGroups()[index];
        m_deformers[index].Update(m_gpu, commandBuffer, m_scene->Asset(group.assetId), group.State());
    }
    m_accelStruct.Update(m_gpu, commandBuffer, *m_scene);

    FrameConstants constants{};
    constants.cameraPosition = frame.cameraPosition;
    constants.cameraForward = frame.cameraForward;
    constants.cameraRight = frame.cameraRight;
    constants.cameraUp = frame.cameraUp;
    constants.tanHalfFovY = frame.tanHalfFovY;
    constants.aspectRatio = static_cast<float>(m_options.width) / m_options.height;
    constants.rayEpsilon = frame.rayEpsilon;
    constants.maxBounces = frame.maxBounces;
    constants.sunDirection = frame.sunDirection;
    constants.sunColor = frame.sunColor;
    constants.ambientIntensity = frame.ambientIntensity;
    constants.skyTopColor = frame.skyTopColor;
    constants.skyBottomColor = frame.skyBottomColor;
    constants.shadeByBlas = frame.debug.shadeByBlas ? 1u : 0u;

    Gpu::ImageBarrier(commandBuffer, m_output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    m_rayTracer.Render(m_gpu, commandBuffer, constants, m_accelStruct.Tlas(), m_accelStruct.GeometryTable(), m_output.view,
                       m_options.width, m_options.height);
    CaptureFrameDiagnostics();
    if (completed)
    {
        completed->blasMs = m_diagnostics.blasBuild.latest;
        completed->mlasTlasMs = m_diagnostics.mlasTlasBuild.latest;
        completed->traceMs = m_diagnostics.traceRays.latest;
    }

    if (m_present)
    {
        Present(commandBuffer, gui);
    }
    else
    {
        Gpu::ImageBarrier(commandBuffer, m_output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    }

    if (m_frameQueries != VK_NULL_HANDLE)
    {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_frameQueries, queryBase + 1);
    }
    if (m_present)
    {
        m_gpu.EndAndSubmit(m_swapChain.AcquireSemaphore(ring), VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           m_swapChain.RenderFinishedSemaphore());
        m_swapChain.Present();
    }
    else
    {
        m_gpu.EndAndSubmit();
    }
    ++m_frameNumber;
    return completed;
}

void VulkanBackend::Present(VkCommandBuffer commandBuffer, ImDrawData* gui)
{
    const VkImage backBuffer = m_swapChain.Image();
    Gpu::ImageBarrier(commandBuffer, m_output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    Gpu::ImageBarrier(commandBuffer, backBuffer, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_BLIT_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(m_options.width), static_cast<int32_t>(m_options.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(m_swapChain.Width()), static_cast<int32_t>(m_swapChain.Height()), 1};
    vkCmdBlitImage(commandBuffer, m_output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, backBuffer,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

    Gpu::ImageBarrier(commandBuffer, m_output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    Gpu::ImageBarrier(commandBuffer, backBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    if (gui)
    {
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = m_swapChain.ImageView();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {m_swapChain.Width(), m_swapChain.Height()}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(commandBuffer, &rendering);
        ImGui_ImplVulkan_RenderDrawData(gui, commandBuffer);
        vkCmdEndRendering(commandBuffer);
    }

    Gpu::ImageBarrier(commandBuffer, backBuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
}

OutputImage VulkanBackend::ReadOutput()
{
    m_gpu.WaitForGpu();
    const uint64_t size = static_cast<uint64_t>(m_options.width) * m_options.height * 4;
    Buffer readback = m_gpu.CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryKind::Readback, "PngReadback");
    VkCommandBuffer commandBuffer = m_gpu.Begin();
    Gpu::ImageBarrier(commandBuffer, m_output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {m_options.width, m_options.height, 1};
    vkCmdCopyImageToBuffer(commandBuffer, m_output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
                           &copy);
    Gpu::ImageBarrier(commandBuffer, m_output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    m_gpu.EndAndSubmit();
    m_gpu.WaitForGpu();

    OutputImage image;
    image.width = m_options.width;
    image.height = m_options.height;
    image.rowPitch = image.width * 4;
    image.rgba.resize(static_cast<size_t>(size));
    memcpy(image.rgba.data(), readback.mapped, image.rgba.size());
    return image;
}
} // namespace
} // namespace vulkan

std::unique_ptr<IRenderBackend> CreateVulkanBackend(const BackendOptions& options, Window* window)
{
    return std::make_unique<vulkan::VulkanBackend>(options, window);
}
