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

#ifndef VK_AMDX_MULTI_LEVEL_ACCELERATION_STRUCTURE_H_
#define VK_AMDX_MULTI_LEVEL_ACCELERATION_STRUCTURE_H_

#define VK_AMDX_multi_level_acceleration_structure                  1
#define VK_AMDX_MULTI_LEVEL_ACCELERATION_STRUCTURE_SPEC_VERSION     1
#define VK_AMDX_MULTI_LEVEL_ACCELERATION_STRUCTURE_EXTENSION_NAME   "VK_AMDX_multi_level_acceleration_structure"

#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_LEVEL_ACCELERATION_STRUCTURE_FEATURES_AMDX ((VkStructureType)1000648000ull)

#define VK_PIPELINE_CREATE_2_RAY_TRACING_ALLOW_MULTI_LEVEL_ACCELERATION_STRUCTURES_BIT_AMDX ((VkPipelineCreateFlagBits2) (1ull << 47))

#define VK_ACCELERATION_STRUCTURE_TYPE_MID_LEVEL_AMDX ((VkAccelerationStructureTypeKHR)1000648000ull)

typedef struct VkPhysicalDeviceMultiLevelAccelerationStructureFeaturesAMDX
{
    VkStructureType    sType;
    void*              pNext;
    VkBool32           multiLevelAccelerationStructure;
} VkPhysicalDeviceMultiLevelAccelerationStructureFeaturesAMDX;

#endif /* VK_AMDX_MULTI_LEVEL_ACCELERATION_STRUCTURE_H_ */
