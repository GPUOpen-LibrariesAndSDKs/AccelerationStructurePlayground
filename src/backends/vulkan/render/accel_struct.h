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

#include "assets/gltf_state.h"
#include "backends/vulkan/core/gpu.h"
#include "render/accel_struct_plan.h"
#include "render/accel_struct_runtime.h"
#include "render/backend.h"
#include "backends/vulkan/render/asset_gpu.h"

namespace vulkan
{
class Deformer;

// Vulkan KHR acceleration structures with AMD multi-level acceleration structure support.
class AccelStructs
{
public:
    AccelStructs() = default;
    AccelStructs(const AccelStructs&) = delete;
    AccelStructs& operator=(const AccelStructs&) = delete;
    ~AccelStructs();
    void Reset();

    void Create(Gpu& gpu, VkCommandBuffer commandBuffer, const Scene& scene, const std::vector<AssetGpu>& assetGpus,
                const std::vector<Deformer>& deformers, const AccelStructPlan& plan);
    void Update(Gpu& gpu, VkCommandBuffer commandBuffer, const Scene& scene);
    void SetDebugVisualization(const DebugSelection& selection);
    // Debug aid: build each MLAS on its own with a barrier after it, instead of batching each level.
    void SetMlasFullBarriers(bool enable) { m_mlasFullBarriers = enable; }

    VkAccelerationStructureKHR Tlas() const { return m_tlas.handle; }
    // Geometry table for the frame slot recorded by the most recent Create/Update.
    const Buffer& GeometryTable() const { return m_geometryTableBuffer[m_uploadRing]; }

    uint32_t BlasCount() const { return static_cast<uint32_t>(m_blases.size()); }
    uint32_t MlasCount() const { return static_cast<uint32_t>(m_mlases.size()); }
    uint32_t AnimatedBlasCount() const;
    uint32_t GeometryCount() const { return static_cast<uint32_t>(m_geometryTable.size()); }
    const AccelStructPlan& Plan() const { return m_plan; }
    AccelStructSizes BlasSizes(uint32_t index) const;
    AccelStructSizes MlasSizes(uint32_t index) const;
    AccelStructSizes TlasSizes() const;

    static constexpr uint32_t kTimingHistorySize = 64;
    using Timing = RollingAverage<kTimingHistorySize>;
    float BlasBuildMilliseconds() const { return m_blasBuildMs; }
    float MlasTlasBuildMilliseconds() const { return m_tlasBuildMs; }
    const Timing& BlasBuildTiming() const { return m_blasBuildAverage; }
    const Timing& MlasTlasBuildTiming() const { return m_tlasBuildAverage; }
    const Timing& TotalBuildTiming() const { return m_totalBuildAverage; }

private:
    struct Blas : BlasNode
    {
        AccelerationStructure result;
        Buffer scratch;
        // One 3x4 matrix per geometry when baking; one buffer per in-flight frame.
        Buffer transforms[Gpu::kFrameCount];
    };

    using Instance = AccelStructInstance;

    struct Mlas : MlasNode
    {
        Buffer instanceBuffer[Gpu::kFrameCount];
        AccelerationStructure result;
        Buffer scratch;
    };

    void ApplyPlan(const AccelStructPlan& plan);
    void RefreshBakeTransforms(const Scene& scene, Blas& blas);
    void SeedUploadRingSlots();
    void BuildBlas(Gpu& gpu, VkCommandBuffer commandBuffer, Blas& blas, bool allowUpdate);
    // Fills the instance buffer and build description for one MLAS, creating its storage on first use. Records
    // nothing; `info.pGeometries` points at `geometry`, which must outlive the build command.
    void PrepareMlasBuild(Gpu& gpu, const Scene& scene, Mlas& mlas, VkAccelerationStructureGeometryKHR& geometry,
                          VkAccelerationStructureBuildGeometryInfoKHR& info,
                          VkAccelerationStructureBuildRangeInfoKHR& range);
    void BuildMlases(Gpu& gpu, VkCommandBuffer commandBuffer, const Scene& scene);
    void BuildTlas(Gpu& gpu, VkCommandBuffer commandBuffer, const Scene& scene);
    bool DebugIncludesInstance(const Instance& instance, int mlasIndex) const;
    void FillInstances(const std::vector<Instance>& source, const Scene& scene, bool applyPlacement, int mlasIndex,
                       VkAccelerationStructureInstanceKHR* instances) const;
    void RebuildDebugMembership();
    VkDeviceAddress AccelStructAddress(AccelStructRef accelStruct) const;
    uint32_t AccelStructInstanceId(AccelStructRef accelStruct) const;
    void UploadGeometryTable();
    const GpuPrimitive& Primitive(const BlasGeometry& geometry) const;
    const Deformer& GeometryDeformer(const BlasGeometry& geometry) const;

    static constexpr uint32_t kTimestampsPerFrame = 4; // blas begin/end, mlas/tlas begin/end
    void BeginTimers(Gpu& gpu, VkCommandBuffer commandBuffer);
    void WriteTimestamp(VkCommandBuffer commandBuffer, uint32_t index) const;

    const std::vector<AssetGpu>* m_assetGpus = nullptr;
    const std::vector<Deformer>* m_deformers = nullptr;
    AccelStructPlan m_plan;
    std::vector<Blas> m_blases;
    std::vector<Mlas> m_mlases;
    DebugMembership m_membership;
    std::vector<std::vector<uint32_t>> m_mlasBuildLevels; // see ComputeMlasBuildLevels
    bool m_mlasFullBarriers = false;
    std::vector<Instance> m_instances;
    std::vector<GeometryInfo> m_geometryTable;

    Buffer m_geometryTableBuffer[Gpu::kFrameCount];
    Buffer m_instanceBuffer[Gpu::kFrameCount];
    AccelerationStructure m_tlas;
    Buffer m_tlasScratch;

    DebugSelection m_debug;
    uint32_t m_frameCounter = 0;
    uint32_t m_uploadRing = 0;

    VkDevice m_device = VK_NULL_HANDLE;
    VkQueryPool m_timestampPool = VK_NULL_HANDLE;
    double m_timestampPeriodNs = 1.0;
    uint32_t m_timestampFrame = 0;
    float m_blasBuildMs = 0.0f;
    float m_tlasBuildMs = 0.0f;
    Timing m_blasBuildAverage;
    Timing m_tlasBuildAverage;
    Timing m_totalBuildAverage;
};
} // namespace vulkan
