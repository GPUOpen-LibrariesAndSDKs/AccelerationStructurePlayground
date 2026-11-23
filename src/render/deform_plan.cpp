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

#include "render/deform_plan.h"

#include "core/math.h"

JointMatrixLayout BuildJointMatrixLayout(const GltfAsset& asset)
{
    JointMatrixLayout layout;
    for (const auto& skin : asset.skins)
    {
        layout.skinOffsets.emplace(skin.get(), layout.totalMatrices);
        layout.totalMatrices += static_cast<uint32_t>(skin->joints.size());
    }
    return layout;
}

void WriteJointMatrices(const GltfAsset& asset, const GltfState& state, const JointMatrixLayout& layout,
                        JointMatrix* destination)
{
    std::vector<float4x4> skinMatrices;
    for (const auto& skin : asset.skins)
    {
        state.ComputeJointMatrices(skin.get(), skinMatrices);
        const uint32_t offset = layout.OffsetOf(skin.get());
        for (size_t j = 0; j < skinMatrices.size(); ++j)
        {
            StoreTransform3x4(destination[offset + j].transform, skinMatrices[j]);
        }
    }
}
