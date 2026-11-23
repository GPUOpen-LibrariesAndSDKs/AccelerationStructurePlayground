#ifndef ASP_COMMON_HLSLI
#define ASP_COMMON_HLSLI

// These structures must stay in sync with src/render/gpu_types.h.

struct FrameConstants
{
    float3 cameraPosition;
    float tanHalfFovY;
    float3 cameraForward;
    float aspectRatio;
    float3 cameraRight;
    float rayEpsilon;
    float3 cameraUp;
    uint maxBounces;
    float3 sunDirection; // direction the light travels
    uint outputIndex;
    float3 sunColor;
    uint geometryTableIndex;
    float3 skyTopColor;
    float ambientIntensity;
    float3 skyBottomColor;
    uint shadeByBlas;
};

struct AccessorInfo
{
    uint bufferIndex;
    uint byteOffset;
    uint byteStride;
    uint format;
};

struct GeometryInfo
{
    // Transform baked into the BLAS at build time; identity unless geometry from
    // several nodes was merged into one bottom-level structure.
    row_major float3x4 bakeTransform;
    // Inverse transpose of bakeTransform to transform normals from object to BLAS space.
    row_major float3x4 bakeNormalTransform;
    AccessorInfo positions;
    AccessorInfo normals;
    AccessorInfo uvs;
    AccessorInfo indices;
    AccessorInfo joints;
    AccessorInfo weights;
    uint materialIndex;
    uint materialTableIndex;
    uint blasIndex;
};

struct MaterialInfo
{
    float4 baseColorFactor;
    float3 emissive;
    float metallic;
    float roughness;
    uint baseColorTexture;
    float2 padding;
};

struct VertexAttributes
{
    float3 normal;
    float2 uv;
};

struct SkinVertex
{
    uint4 joints;
    float4 weights;
};

// Wrapper so the matrix storage order is explicit inside a StructuredBuffer.
struct JointMatrix
{
    row_major float3x4 transform;
};

static const uint kInvalidIndex = 0xFFFFFFFF;

#ifdef __spirv__
// Vulkan has no ResourceDescriptorHeap: each bindless resource type is its own array in set 0,
// matching vulkan::Bindless in src/backends/vulkan/core/gpu.h.
[[vk::binding(0, 0)]] ByteAddressBuffer g_byteBuffers[];
[[vk::binding(1, 0)]] StructuredBuffer<MaterialInfo> g_materialTables[];
[[vk::binding(2, 0)]] Texture2D<float4> g_textures[];
[[vk::binding(3, 0)]] StructuredBuffer<JointMatrix> g_jointMatrices[];
[[vk::binding(4, 0)]] RWStructuredBuffer<float3> g_deformedPositions[];
[[vk::binding(5, 0)]] RWStructuredBuffer<VertexAttributes> g_deformedAttributes[];
[[vk::binding(6, 0)]] SamplerState g_sampler;
#define BINDLESS(vulkanArray, index) vulkanArray[index]
#else
#define BINDLESS(vulkanArray, index) ResourceDescriptorHeap[index]
#endif

uint AccessorComponentSize(AccessorInfo accessor)
{
    const uint componentType = accessor.format & 0xFFFF;
    return componentType == 5120 || componentType == 5121 ? 1
         : componentType == 5122 || componentType == 5123 ? 2
         : 4;
}

uint LoadAccessorBits(AccessorInfo accessor, uint element, uint component)
{
    if (accessor.bufferIndex == kInvalidIndex)
        return 0;
    const uint size = AccessorComponentSize(accessor);
    const uint address = accessor.byteOffset + element * accessor.byteStride + component * size;
    const ByteAddressBuffer buffer = BINDLESS(g_byteBuffers, NonUniformResourceIndex(accessor.bufferIndex));
    const uint word = buffer.Load(address & ~3u);
    if (size == 1)
        return (word >> ((address & 3u) * 8u)) & 0xFFu;
    if (size == 2)
        return (word >> ((address & 2u) * 8u)) & 0xFFFFu;
    return word;
}

uint LoadAccessorUint(AccessorInfo accessor, uint element, uint component = 0)
{
    return LoadAccessorBits(accessor, element, component);
}

float LoadAccessorFloat(AccessorInfo accessor, uint element, uint component = 0)
{
    const uint componentType = accessor.format & 0xFFFF;
    const bool normalized = (accessor.format & 0x10000) != 0;
    const uint bits = LoadAccessorBits(accessor, element, component);
    if (componentType == 5126)
        return asfloat(bits);
    if (componentType == 5120)
    {
        const int value = bits >= 128 ? int(bits) - 256 : int(bits);
        return normalized ? max(float(value) / 127.0, -1.0) : float(value);
    }
    if (componentType == 5121)
        return normalized ? float(bits) / 255.0 : float(bits);
    if (componentType == 5122)
    {
        const int value = bits >= 32768 ? int(bits) - 65536 : int(bits);
        return normalized ? max(float(value) / 32767.0, -1.0) : float(value);
    }
    if (componentType == 5123)
        return normalized ? float(bits) / 65535.0 : float(bits);
    return normalized ? float(bits) / 4294967295.0 : float(bits);
}

float2 LoadAccessorFloat2(AccessorInfo accessor, uint element)
{
    return float2(LoadAccessorFloat(accessor, element, 0), LoadAccessorFloat(accessor, element, 1));
}

float3 LoadAccessorFloat3(AccessorInfo accessor, uint element)
{
    return float3(LoadAccessorFloat(accessor, element, 0), LoadAccessorFloat(accessor, element, 1),
                  LoadAccessorFloat(accessor, element, 2));
}

float4 LoadAccessorFloat4(AccessorInfo accessor, uint element)
{
    return float4(LoadAccessorFloat(accessor, element, 0), LoadAccessorFloat(accessor, element, 1),
                  LoadAccessorFloat(accessor, element, 2), LoadAccessorFloat(accessor, element, 3));
}

uint4 LoadAccessorUint4(AccessorInfo accessor, uint element)
{
    return uint4(LoadAccessorUint(accessor, element, 0), LoadAccessorUint(accessor, element, 1),
                 LoadAccessorUint(accessor, element, 2), LoadAccessorUint(accessor, element, 3));
}

SkinVertex LoadSkinVertex(AccessorInfo joints, AccessorInfo weights, uint element)
{
    SkinVertex result;
    result.joints = LoadAccessorUint4(joints, element);
    result.weights = LoadAccessorFloat4(weights, element);
    return result;
}

float3 LinearToSrgb(float3 color)
{
    color = saturate(color);
    return select(color <= 0.0031308, color * 12.92, 1.055 * pow(color, 1.0 / 2.4) - 0.055);
}

#endif // ASP_COMMON_HLSLI
