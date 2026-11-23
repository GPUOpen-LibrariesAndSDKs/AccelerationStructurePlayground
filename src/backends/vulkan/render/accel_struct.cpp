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

#include "backends/vulkan/render/accel_struct.h"

#include "backends/vulkan/render/deform.h"

#include <cstring>

namespace vulkan
{
namespace
{
constexpr uint32_t kFullRebuildInterval = 64; // periodic rebuild keeps refit quality from drifting
constexpr VkBufferUsageFlags kBuildInputUsage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
constexpr uint64_t kBuildInputAlignment = 16;

AccelerationStructure CreateAccelerationStructure(Gpu& gpu, VkAccelerationStructureTypeKHR type, uint64_t size,
                                                  const char* name)
{
    AccelerationStructure structure;
    structure.device = gpu.Device();
    structure.storage =
        gpu.CreateBuffer(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, MemoryKind::DeviceLocal, name);

    VkAccelerationStructureCreateInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    info.buffer = structure.storage.buffer;
    info.size = size;
    info.type = type;
    VK_CHECK(vkCreateAccelerationStructureKHR(gpu.Device(), &info, nullptr, &structure.handle));
    gpu.SetName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, reinterpret_cast<uint64_t>(structure.handle), name);

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    addressInfo.accelerationStructure = structure.handle;
    structure.address = vkGetAccelerationStructureDeviceAddressKHR(gpu.Device(), &addressInfo);
    return structure;
}
} // namespace

AccelStructs::~AccelStructs()
{
    Reset();
}

void AccelStructs::Reset()
{
    if (m_timestampPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(m_device, m_timestampPool, nullptr);
        m_timestampPool = VK_NULL_HANDLE;
    }
    m_blases.clear();
    m_mlases.clear();
    m_membership = {};
    m_mlasBuildLevels.clear();
    m_instances.clear();
    m_geometryTable.clear();
    for (Buffer& buffer : m_geometryTableBuffer)
    {
        buffer.Reset();
    }
    for (Buffer& buffer : m_instanceBuffer)
    {
        buffer.Reset();
    }
    m_tlas.Reset();
    m_tlasScratch.Reset();
    m_plan = {};
    m_frameCounter = 0;
    m_uploadRing = 0;
    m_timestampFrame = 0;
    m_blasBuildMs = 0.0f;
    m_tlasBuildMs = 0.0f;
    m_blasBuildAverage = {};
    m_tlasBuildAverage = {};
    m_totalBuildAverage = {};
}

uint32_t AccelStructs::AnimatedBlasCount() const
{
    return static_cast<uint32_t>(
        std::count_if(m_blases.begin(), m_blases.end(), [](const Blas& blas) { return blas.animated; }));
}

AccelStructSizes AccelStructs::BlasSizes(uint32_t index) const
{
    const Blas& blas = m_blases[index];
    return {blas.result.storage.size, blas.scratch.size};
}

AccelStructSizes AccelStructs::MlasSizes(uint32_t index) const
{
    const Mlas& mlas = m_mlases[index];
    return {mlas.result.storage.size, mlas.scratch.size};
}

AccelStructSizes AccelStructs::TlasSizes() const
{
    return {m_tlas.storage.size, m_tlasScratch.size};
}

// Gets the plan for building acceleration structures from plan and then creates the necessary Vulkan resources and
// builds the acceleration structures specified in the plan.
void AccelStructs::Create(Gpu& gpu, VkCommandBuffer commandBuffer, const Scene& scene,
                                    const std::vector<AssetGpu>& assetGpus, const std::vector<Deformer>& deformers,
                                    const AccelStructPlan& plan)
{
    if (!plan.mlases.empty() && !gpu.MlasEnabled())
    {
        Fatal("The selected Vulkan device does not support VK_AMDX_multi_level_acceleration_structure.");
    }
    if (assetGpus.size() != scene.Assets().size() || deformers.size() != scene.PlaybackGroups().size())
    {
        Fatal("Acceleration structures require one GPU asset per scene asset and one deformer per playback group.");
    }

    m_device = gpu.Device();
    m_timestampPeriodNs = gpu.TimestampPeriodNs();
    m_assetGpus = &assetGpus;
    m_deformers = &deformers;
    ApplyPlan(plan);
    m_uploadRing = gpu.FrameIndex();

    for (uint32_t i = 0; i < Gpu::kFrameCount; ++i)
    {
        m_geometryTableBuffer[i] = gpu.CreateUploadBuffer(nullptr, m_geometryTable.size() * sizeof(GeometryInfo),
                                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "GeometryTable");
    }

    BeginTimers(gpu, commandBuffer);
    WriteTimestamp(commandBuffer, 0);
    for (Blas& blas : m_blases)
    {
        if (blas.baked)
        {
            for (uint32_t i = 0; i < Gpu::kFrameCount; ++i)
            {
                blas.transforms[i] = gpu.CreateUploadBuffer(nullptr, blas.geometries.size() * sizeof(float) * 12,
                                                            kBuildInputUsage, "BlasTransforms", kBuildInputAlignment);
            }
            RefreshBakeTransforms(scene, blas);
        }
        BuildBlas(gpu, commandBuffer, blas, blas.animated);
    }
    Gpu::Barrier(commandBuffer, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                 VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                 VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                 VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    WriteTimestamp(commandBuffer, 1);

    for (Mlas& mlas : m_mlases)
    {
        for (uint32_t i = 0; i < Gpu::kFrameCount; ++i)
        {
            mlas.instanceBuffer[i] =
                gpu.CreateUploadBuffer(nullptr, mlas.instances.size() * sizeof(VkAccelerationStructureInstanceKHR),
                                       kBuildInputUsage, "MlasInstances", kBuildInputAlignment);
        }
    }

    UploadGeometryTable();
    SeedUploadRingSlots();

    for (uint32_t i = 0; i < Gpu::kFrameCount; ++i)
    {
        m_instanceBuffer[i] =
            gpu.CreateUploadBuffer(nullptr, m_instances.size() * sizeof(VkAccelerationStructureInstanceKHR),
                                   kBuildInputUsage, "TlasInstances", kBuildInputAlignment);
    }
    WriteTimestamp(commandBuffer, 2);
    BuildMlases(gpu, commandBuffer, scene);
    BuildTlas(gpu, commandBuffer, scene);
    WriteTimestamp(commandBuffer, 3);
    ++m_timestampFrame;
}

void AccelStructs::ApplyPlan(const AccelStructPlan& plan)
{
    m_plan = plan;
    MirrorPlan(plan, m_blases, m_mlases, m_instances);
    m_mlasBuildLevels = ComputeMlasBuildLevels(m_mlases);

    m_geometryTable = BuildGeometryTable(
        m_blases, [this](const BlasGeometry& geometry) -> const GpuPrimitive& { return Primitive(geometry); },
        [this](const BlasGeometry& geometry) {
            const Deformer& deformer = GeometryDeformer(geometry);
            return DeformedGeometrySrvs{deformer.PositionSrv(geometry.gltfNode, geometry.primitive),
                                        deformer.AttributeSrv(geometry.gltfNode, geometry.primitive)};
        },
        [this](AssetId asset) { return (*m_assetGpus)[asset.value].MaterialTableIndex(); });
    RebuildDebugMembership();
}

void AccelStructs::SetDebugVisualization(const DebugSelection& selection)
{
    if (m_debug.mode == selection.mode && m_debug.index == selection.index)
    {
        return;
    }
    m_debug.mode = selection.mode;
    m_debug.index = selection.index;
    RebuildDebugMembership();
}

void AccelStructs::RebuildDebugMembership()
{
    m_membership = ComputeDebugMembership(m_mlases, m_debug.mode, m_debug.index);
}

const GpuPrimitive& AccelStructs::Primitive(const BlasGeometry& geometry) const
{
    return (*m_assetGpus)[geometry.assetId.value].PrimitiveFor(geometry.primitive);
}

const Deformer& AccelStructs::GeometryDeformer(const BlasGeometry& geometry) const
{
    return (*m_deformers)[geometry.playbackGroup.value];
}

void AccelStructs::RefreshBakeTransforms(const Scene& scene, Blas& blas)
{
    ::RefreshBakeTransforms(scene, blas, static_cast<float (*)[3][4]>(blas.transforms[m_uploadRing].mapped),
                            m_geometryTable);
}

// Copies the slot filled during Create into the other ring slots, so BLASes that are baked but never
// animated stay valid whichever slot a later frame lands on.
void AccelStructs::SeedUploadRingSlots()
{
    for (uint32_t ring = 0; ring < Gpu::kFrameCount; ++ring)
    {
        if (ring == m_uploadRing)
        {
            continue;
        }
        const Buffer& table = m_geometryTableBuffer[m_uploadRing];
        memcpy(m_geometryTableBuffer[ring].mapped, table.mapped, static_cast<size_t>(table.size));
        for (Blas& blas : m_blases)
        {
            if (blas.baked)
            {
                const Buffer& transforms = blas.transforms[m_uploadRing];
                memcpy(blas.transforms[ring].mapped, transforms.mapped, static_cast<size_t>(transforms.size));
            }
        }
    }
}

void AccelStructs::BuildBlas(Gpu& gpu, VkCommandBuffer commandBuffer, Blas& blas, bool allowUpdate)
{
    const size_t count = blas.geometries.size();
    std::vector<VkAccelerationStructureGeometryKHR> geometries(count);
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(count);
    std::vector<uint32_t> primitiveCounts(count);
    for (size_t i = 0; i < count; ++i)
    {
        const BlasGeometry& geometry = blas.geometries[i];
        const GpuPrimitive& primitive = Primitive(geometry);

        VkAccelerationStructureGeometryKHR& desc = geometries[i];
        desc = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        desc.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        desc.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        VkAccelerationStructureGeometryTrianglesDataKHR& triangles = desc.geometry.triangles;
        triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        if (geometry.deformed)
        {
            triangles.vertexData.deviceAddress =
                GeometryDeformer(geometry).VertexBuffer(geometry.gltfNode, geometry.primitive).Address();
            triangles.vertexStride = sizeof(float3);
        }
        else
        {
            triangles.vertexData.deviceAddress = primitive.positions.Address();
            triangles.vertexStride = primitive.positions.byteStride;
        }
        triangles.maxVertex = primitive.vertexCount > 0 ? primitive.vertexCount - 1 : 0;
        triangles.indexType = primitive.indices.componentType == 5123 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress = primitive.indices.Address();
        if (blas.baked)
        {
            triangles.transformData.deviceAddress = blas.transforms[m_uploadRing].Address() + i * sizeof(float) * 12;
        }
        primitiveCounts[i] = primitive.indexCount / 3;
        ranges[i] = {primitiveCounts[i], 0, 0, 0};
    }

    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (allowUpdate)
    {
        info.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    }
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = static_cast<uint32_t>(count);
    info.pGeometries = geometries.data();

    if (!blas.result)
    {
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(gpu.Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
                                                primitiveCounts.data(), &sizes);
        blas.result = CreateAccelerationStructure(gpu, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                  sizes.accelerationStructureSize, "Blas");
        blas.scratch =
            gpu.CreateBuffer(std::max(sizes.buildScratchSize, sizes.updateScratchSize),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryKind::DeviceLocal, "BlasScratch",
                             gpu.AccelerationStructureProperties().minAccelerationStructureScratchOffsetAlignment);
    }

    const bool refit = blas.built && allowUpdate && (m_frameCounter % kFullRebuildInterval) != 0;
    info.mode =
        refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.srcAccelerationStructure = refit ? blas.result.handle : VK_NULL_HANDLE;
    info.dstAccelerationStructure = blas.result.handle;
    info.scratchData.deviceAddress = blas.scratch.Address();

    const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = ranges.data();
    vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &info, &rangePointer);
    blas.built = true;
}

VkDeviceAddress AccelStructs::AccelStructAddress(AccelStructRef accelStruct) const
{
    return accelStruct.kind == AccelStructRefKind::Blas ? m_blases[accelStruct.value].result.address
                                                        : m_mlases[accelStruct.value].result.address;
}

uint32_t AccelStructs::AccelStructInstanceId(AccelStructRef accelStruct) const
{
    return accelStruct.kind == AccelStructRefKind::Blas ? m_blases[accelStruct.value].geometryTableOffset : 0;
}

// Whether the debug visualization draws one instance. `mlasIndex` is the MLAS being filled, or
// -1 for the TLAS. Returns true whenever no node is isolated, so an ordinary frame is unaffected.
bool AccelStructs::DebugIncludesInstance(const Instance& instance, int mlasIndex) const
{
    if (m_debug.mode == AccelStructDebugMode::Full)
    {
        return true;
    }
    if (m_debug.mode == AccelStructDebugMode::SelectedBlas)
    {
        return instance.accelStruct.kind == AccelStructRefKind::Blas
                   ? instance.accelStruct.value == m_debug.index
                   : m_membership.mlasContainsSelectedBlas[instance.accelStruct.value];
    }
    // Everything beneath the selected MLAS is drawn whole; outside it, only children that still
    // lead to the selection survive.
    const bool insideSelectedSubtree =
        mlasIndex >= 0 && m_membership.mlasInSelectedMlasSubtree[static_cast<size_t>(mlasIndex)];
    return insideSelectedSubtree || (instance.accelStruct.kind == AccelStructRefKind::Mlas &&
                                     m_membership.mlasContainsSelectedMlas[instance.accelStruct.value]);
}

void AccelStructs::FillInstances(const std::vector<Instance>& source, const Scene& scene, bool applyPlacement,
                                           int mlasIndex, VkAccelerationStructureInstanceKHR* instances) const
{
    for (size_t i = 0; i < source.size(); ++i)
    {
        const Instance& instance = source[i];
        const bool included = DebugIncludesInstance(instance, mlasIndex);

        VkAccelerationStructureInstanceKHR desc{};
        StoreTransform3x4(desc.transform.matrix, ComposeInstanceTransform(scene, instance, applyPlacement));
        desc.instanceCustomIndex = AccelStructInstanceId(instance.accelStruct);
        // The extension tests the instance mask only on the final instance in a chain.
        desc.mask = instance.accelStruct.kind == AccelStructRefKind::Mlas || included ? 0xFF : 0;
        desc.instanceShaderBindingTableRecordOffset = 0;
        desc.flags = 0;
        desc.accelerationStructureReference = AccelStructAddress(instance.accelStruct);
        memcpy(&instances[i], &desc, sizeof(desc));
    }
}

void AccelStructs::PrepareMlasBuild(Gpu& gpu, const Scene& scene, Mlas& mlas,
                                    VkAccelerationStructureGeometryKHR& geometry,
                                    VkAccelerationStructureBuildGeometryInfoKHR& info,
                                    VkAccelerationStructureBuildRangeInfoKHR& range)
{
    const uint32_t index = static_cast<uint32_t>(&mlas - m_mlases.data());
    const Buffer& instanceBuffer = mlas.instanceBuffer[m_uploadRing];
    FillInstances(mlas.instances, scene, false, static_cast<int>(index),
                  static_cast<VkAccelerationStructureInstanceKHR*>(instanceBuffer.mapped));

    geometry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    geometry.geometry.instances.data.deviceAddress = instanceBuffer.Address();

    info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_MID_LEVEL_AMDX;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;

    const uint32_t instanceCount = static_cast<uint32_t>(mlas.instances.size());
    if (!mlas.result)
    {
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(gpu.Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
                                                &instanceCount, &sizes);
        mlas.result = CreateAccelerationStructure(gpu, VK_ACCELERATION_STRUCTURE_TYPE_MID_LEVEL_AMDX,
                                                  sizes.accelerationStructureSize, "Mlas");
        mlas.scratch = gpu.CreateBuffer(
            sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryKind::DeviceLocal, "MlasScratch",
            gpu.AccelerationStructureProperties().minAccelerationStructureScratchOffsetAlignment);
    }

    info.dstAccelerationStructure = mlas.result.handle;
    info.scratchData.deviceAddress = mlas.scratch.Address();
    range = {instanceCount, 0, 0, 0};
}

void AccelStructs::BuildMlases(Gpu& gpu, VkCommandBuffer commandBuffer, const Scene& scene)
{
    // MLASes in one level never reference each other and each owns its result and scratch, so a level is built in a
    // single command. The barrier after each level covers the next level's reads, and after the last level the TLAS
    // build's. With full barriers enabled every MLAS gets its own command and barrier instead.
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePointers;
    for (const std::vector<uint32_t>& level : m_mlasBuildLevels)
    {
        // Sized up front: each info points at its geometry, so these must not reallocate once filled.
        geometries.resize(level.size());
        infos.resize(level.size());
        ranges.resize(level.size());
        rangePointers.resize(level.size());
        for (size_t i = 0; i < level.size(); ++i)
        {
            PrepareMlasBuild(gpu, scene, m_mlases[level[i]], geometries[i], infos[i], ranges[i]);
            rangePointers[i] = &ranges[i];
        }

        const uint32_t batchSize = m_mlasFullBarriers ? 1 : static_cast<uint32_t>(level.size());
        for (uint32_t first = 0; first < level.size(); first += batchSize)
        {
            vkCmdBuildAccelerationStructuresKHR(commandBuffer, batchSize, &infos[first], &rangePointers[first]);
            Gpu::Barrier(commandBuffer, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                         VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        }
    }
}

void AccelStructs::BuildTlas(Gpu& gpu, VkCommandBuffer commandBuffer, const Scene& scene)
{
    const Buffer& instanceBuffer = m_instanceBuffer[m_uploadRing];
    FillInstances(m_instances, scene, true, -1,
                  static_cast<VkAccelerationStructureInstanceKHR*>(instanceBuffer.mapped));

    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    geometry.geometry.instances.data.deviceAddress = instanceBuffer.Address();

    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;

    const uint32_t instanceCount = static_cast<uint32_t>(m_instances.size());
    if (!m_tlas)
    {
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(gpu.Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
                                                &instanceCount, &sizes);
        m_tlas = CreateAccelerationStructure(gpu, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                                             sizes.accelerationStructureSize, "Tlas");
        m_tlasScratch = gpu.CreateBuffer(
            sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryKind::DeviceLocal, "TlasScratch",
            gpu.AccelerationStructureProperties().minAccelerationStructureScratchOffsetAlignment);
    }

    info.dstAccelerationStructure = m_tlas.handle;
    info.scratchData.deviceAddress = m_tlasScratch.Address();
    const VkAccelerationStructureBuildRangeInfoKHR range{instanceCount, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = &range;
    vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &info, &rangePointer);

    Gpu::Barrier(commandBuffer, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                 VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                 VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
}

void AccelStructs::UploadGeometryTable()
{
    memcpy(m_geometryTableBuffer[m_uploadRing].mapped, m_geometryTable.data(),
           m_geometryTable.size() * sizeof(GeometryInfo));
}

void AccelStructs::Update(Gpu& gpu, VkCommandBuffer commandBuffer, const Scene& scene)
{
    ++m_frameCounter;
    m_uploadRing = gpu.FrameIndex();
    BeginTimers(gpu, commandBuffer);

    // The previous frame's rays and builds may still be using the structures rebuilt below.
    Gpu::Barrier(
        commandBuffer,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);

    bool bakeTransformsChanged = false;
    WriteTimestamp(commandBuffer, 0);
    for (Blas& blas : m_blases)
    {
        if (!blas.animated)
        {
            continue;
        }
        if (blas.baked)
        {
            RefreshBakeTransforms(scene, blas);
            bakeTransformsChanged = true;
        }
        BuildBlas(gpu, commandBuffer, blas, true);
    }
    Gpu::Barrier(commandBuffer, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                 VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                 VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                 VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    WriteTimestamp(commandBuffer, 1);

    if (bakeTransformsChanged)
    {
        UploadGeometryTable();
    }
    WriteTimestamp(commandBuffer, 2);
    BuildMlases(gpu, commandBuffer, scene);
    BuildTlas(gpu, commandBuffer, scene);
    WriteTimestamp(commandBuffer, 3);
    ++m_timestampFrame;
}

void AccelStructs::BeginTimers(Gpu& gpu, VkCommandBuffer commandBuffer)
{
    if (m_timestampPool == VK_NULL_HANDLE)
    {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = Gpu::kFrameCount * kTimestampsPerFrame;
        VK_CHECK(vkCreateQueryPool(gpu.Device(), &info, nullptr, &m_timestampPool));
    }

    // The slot about to be reused was last written kFrameCount frames ago, which Gpu::Begin()
    // has already waited for.
    const uint32_t base = (m_timestampFrame % Gpu::kFrameCount) * kTimestampsPerFrame;
    if (m_timestampFrame >= Gpu::kFrameCount)
    {
        uint64_t timestamps[kTimestampsPerFrame]{};
        const VkResult result =
            vkGetQueryPoolResults(gpu.Device(), m_timestampPool, base, kTimestampsPerFrame, sizeof(timestamps),
                                  timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        // VK_NOT_READY means this slot's results have not landed yet, which is benign and just
        // costs one sample. Anything else is a real failure and should not be mistaken for it.
        if (result == VK_SUCCESS)
        {
            m_blasBuildMs = static_cast<float>((timestamps[1] - timestamps[0]) * m_timestampPeriodNs * 1e-6);
            m_tlasBuildMs = static_cast<float>((timestamps[3] - timestamps[2]) * m_timestampPeriodNs * 1e-6);
            m_blasBuildAverage.Add(m_blasBuildMs);
            m_tlasBuildAverage.Add(m_tlasBuildMs);
            m_totalBuildAverage.Add(m_blasBuildMs + m_tlasBuildMs);
        }
        else if (result != VK_NOT_READY)
        {
            VK_CHECK(result);
        }
    }
    vkCmdResetQueryPool(commandBuffer, m_timestampPool, base, kTimestampsPerFrame);
}

void AccelStructs::WriteTimestamp(VkCommandBuffer commandBuffer, uint32_t index) const
{
    const uint32_t base = (m_timestampFrame % Gpu::kFrameCount) * kTimestampsPerFrame;
    vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_timestampPool, base + index);
}
} // namespace vulkan
