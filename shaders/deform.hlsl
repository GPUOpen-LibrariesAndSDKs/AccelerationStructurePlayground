#include "common.hlsli"

// Linear-blend skinning. Bind-pose positions and normals are deformed into world
// space and written to the buffers the BLAS is built from, so skinned geometry needs
// no instance transform in the TLAS.

struct DeformConstants
{
    AccessorInfo positions;
    AccessorInfo normals;
    AccessorInfo uvs;
    AccessorInfo joints;
    AccessorInfo weights;
    uint jointMatrixIndex;
    uint outPositionIndex;
    uint outAttributeIndex;
    uint vertexCount;
    uint jointOffset;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<DeformConstants> g_deform;
#else
ConstantBuffer<DeformConstants> g_deform : register(b0);
#endif

[numthreads(64, 1, 1)]
void DeformMain(uint3 threadId : SV_DispatchThreadID)
{
    const uint vertex = threadId.x;
    if (vertex >= g_deform.vertexCount)
        return;

    const StructuredBuffer<JointMatrix> jointMatrices = BINDLESS(g_jointMatrices, g_deform.jointMatrixIndex);
    RWStructuredBuffer<float3> outPositions = BINDLESS(g_deformedPositions, g_deform.outPositionIndex);
    RWStructuredBuffer<VertexAttributes> outAttributes = BINDLESS(g_deformedAttributes, g_deform.outAttributeIndex);

    const float3 bindPosition = LoadAccessorFloat3(g_deform.positions, vertex);
    VertexAttributes bindAttribute;
    bindAttribute.normal = LoadAccessorFloat3(g_deform.normals, vertex);
    bindAttribute.uv = LoadAccessorFloat2(g_deform.uvs, vertex);
    const SkinVertex skin = LoadSkinVertex(g_deform.joints, g_deform.weights, vertex);

    float3 position = 0.0;
    float3 normal = 0.0;
    float totalWeight = 0.0;

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        const float weight = skin.weights[i];
        if (weight <= 0.0)
            continue;

        const float3x4 joint = jointMatrices[g_deform.jointOffset + skin.joints[i]].transform;
        position += weight * mul(joint, float4(bindPosition, 1.0));
        normal += weight * mul((float3x3)joint, bindAttribute.normal);
        totalWeight += weight;
    }

    // Fall back to the bind pose for vertices with no joint influence.
    if (totalWeight <= 0.0)
    {
        position = bindPosition;
        normal = bindAttribute.normal;
    }
    else
    {
        position /= totalWeight;
    }

    VertexAttributes result;
    result.normal = normalize(normal);
    result.uv = bindAttribute.uv;

    outPositions[vertex] = position;
    outAttributes[vertex] = result;
}
