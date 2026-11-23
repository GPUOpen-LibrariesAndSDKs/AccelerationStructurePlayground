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

#include "render/asset_layout.h"

#include "core/error.h"

#include <limits>

uint32_t ComponentSize(Gltf::AccessorComponentType componentType)
{
    switch (componentType)
    {
    case Gltf::AccessorComponentType::Byte:
    case Gltf::AccessorComponentType::UnsignedByte:
        return 1;
    case Gltf::AccessorComponentType::Short:
    case Gltf::AccessorComponentType::UnsignedShort:
        return 2;
    case Gltf::AccessorComponentType::UnsignedInt:
    case Gltf::AccessorComponentType::Float:
        return 4;
    default:
        Fatal("Unsupported glTF accessor component type.");
    }
}

std::vector<AccessorLayout> BuildAccessorLayouts(const GltfAsset& asset, size_t bufferCount)
{
    std::vector<AccessorLayout> layouts(asset.accessors.size());
    for (size_t i = 0; i < asset.accessors.size(); ++i)
    {
        const Gltf::Accessor& source = *asset.accessors[i];
        if (!source.pBufferView || !source.pBufferView->pBuffer)
        {
            Fatal("Accessor %zu has no bufferView.", i);
        }
        const Gltf::BufferView& view = *source.pBufferView;
        const size_t bufferIndex = Gltf::PointerIndex(asset.buffers, view.pBuffer, "buffer");
        if (bufferIndex >= bufferCount)
        {
            Fatal("Accessor %zu references an invalid buffer.", i);
        }

        const uint64_t offset = view.byteOffset + source.byteOffset;
        // A zero byteStride means tightly packed, which glTF leaves implicit.
        const uint64_t stride = view.byteStride != 0 ? view.byteStride
                                                     : static_cast<uint64_t>(ComponentSize(source.componentType)) *
                                                           Gltf::GetAccessorTypeSize(source.type);
        if (offset > std::numeric_limits<uint32_t>::max() || stride > std::numeric_limits<uint32_t>::max() ||
            source.count > std::numeric_limits<uint32_t>::max())
        {
            Fatal("Accessor %zu exceeds the supported 32-bit GPU address range.", i);
        }

        AccessorLayout& layout = layouts[i];
        layout.bufferIndex = static_cast<uint32_t>(bufferIndex);
        layout.byteOffset = static_cast<uint32_t>(offset);
        layout.byteStride = static_cast<uint32_t>(stride);
        layout.count = static_cast<uint32_t>(source.count);
        layout.componentType = static_cast<uint32_t>(source.componentType);
        layout.components = static_cast<uint32_t>(Gltf::GetAccessorTypeSize(source.type));
        layout.normalized = source.normalized;
    }
    return layouts;
}
