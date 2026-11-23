#ifndef ASP_RAYTRACING_COMMON_HLSLI
#define ASP_RAYTRACING_COMMON_HLSLI

#include "common.hlsli"

// Ray tracing helpers included by shaders/raytracing_vulkan.hlsl. Expects the including file to
// have already declared g_frame, g_scene and g_sampler, which carry their Vulkan bindings.

float3 SampleSky(float3 direction)
{
    return lerp(g_frame.skyBottomColor, g_frame.skyTopColor, saturate(direction.y * 0.5 + 0.5));
}

float3 DebugBlasColor(uint blasIndex)
{
    const float hue = frac(float(blasIndex) * 0.61803398875);
    const float hue6 = hue * 6.0;
    const float x = 1.0 - abs(fmod(hue6, 2.0) - 1.0);
    if (hue6 < 1.0) return float3(1.0, x, 0.0);
    if (hue6 < 2.0) return float3(x, 1.0, 0.0);
    if (hue6 < 3.0) return float3(0.0, 1.0, x);
    if (hue6 < 4.0) return float3(0.0, x, 1.0);
    if (hue6 < 5.0) return float3(x, 0.0, 1.0);
    return float3(1.0, 0.0, x);
}

// Returns 0 when the sun is occluded. Opaque geometry only, so a single Proceed()
// is enough to resolve the query.
float TraceShadowRay(float3 position, float3 toLight)
{
    RayDesc ray;
    ray.Origin = position;
    ray.Direction = toLight;
    ray.TMin = 0.0;
    ray.TMax = 1e30;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
             RAY_FLAG_FORCE_OPAQUE>
        query;
    query.TraceRayInline(g_scene, 0, 0xFF, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

float3 dbgColorNormals(const float3 normal)
{
    return normal * 0.5 + 0.5;
}

float3 dbgNumJoints(const uint3 indices, const GeometryInfo geometry)
{
    const SkinVertex sv0 = LoadSkinVertex(geometry.joints, geometry.weights, indices.x);
    const SkinVertex sv1 = LoadSkinVertex(geometry.joints, geometry.weights, indices.y);
    const SkinVertex sv2 = LoadSkinVertex(geometry.joints, geometry.weights, indices.z);

    uint64_t seenJoints = 0;
    for (uint i = 0; i < 4; ++i)
    {
        if (sv0.weights[i] > 0.0) seenJoints |= 1ull << sv0.joints[i];
        if (sv1.weights[i] > 0.0) seenJoints |= 1ull << sv1.joints[i];
        if (sv2.weights[i] > 0.0) seenJoints |= 1ull << sv2.joints[i];
    }

    // count the number of bits set in seenJoints
    uint jointCount = 0;
    for (uint64_t mask = seenJoints; mask != 0; mask >>= 1)
        jointCount += uint(mask & 1ull);

    if (jointCount == 0)
        return float3(1.0, 0.0, 1.0);
    else if (jointCount == 1)
        return float3(0.0, 1.0, 0.0);
    else if (jointCount == 2)
        return float3(0.25, 0.75, 0.0);
    else if (jointCount == 3)
        return float3(0.5, 0.5, 0.0);
    else if (jointCount == 4)
        return float3(0.75, 0.25, 0.0);
    else
        return float3(1.0, 0.0, 0.0);
}

float3 dbgJointsInfluence(const uint3 indices, const uint3 targetJoints, const GeometryInfo geometry)
{
    const SkinVertex sv0 = LoadSkinVertex(geometry.joints, geometry.weights, indices.x);
    const SkinVertex sv1 = LoadSkinVertex(geometry.joints, geometry.weights, indices.y);
    const SkinVertex sv2 = LoadSkinVertex(geometry.joints, geometry.weights, indices.z);

    float3 influence = float3(0.0, 0.0, 0.0);

    for (uint i = 0; i < 4; ++i)
    {
        if (sv0.joints[i] == targetJoints.x) influence.x = max(influence.x, sv0.weights[i]);
        if (sv0.joints[i] == targetJoints.y) influence.y = max(influence.y, sv0.weights[i]);
        if (sv0.joints[i] == targetJoints.z) influence.z = max(influence.z, sv0.weights[i]);

        if (sv1.joints[i] == targetJoints.x) influence.x = max(influence.x, sv1.weights[i]);
        if (sv1.joints[i] == targetJoints.y) influence.y = max(influence.y, sv1.weights[i]);
        if (sv1.joints[i] == targetJoints.z) influence.z = max(influence.z, sv1.weights[i]);

        if (sv2.joints[i] == targetJoints.x) influence.x = max(influence.x, sv2.weights[i]);
        if (sv2.joints[i] == targetJoints.y) influence.y = max(influence.y, sv2.weights[i]);
        if (sv2.joints[i] == targetJoints.z) influence.z = max(influence.z, sv2.weights[i]);
    }

    return influence;
}

// Treats each joint's per-vertex weight as a scalar field linear over the triangle and
// returns the Frobenius norm of the Jacobian of the full (dense) per-joint influence vector.
float dbgJointInfluenceGradient(const uint3 indices, const float3 weights, const GeometryInfo geometry)
{
    const float3 p0 = LoadAccessorFloat3(geometry.positions, indices.x);
    const float3 p1 = LoadAccessorFloat3(geometry.positions, indices.y);
    const float3 p2 = LoadAccessorFloat3(geometry.positions, indices.z);

    const SkinVertex sv0 = LoadSkinVertex(geometry.joints, geometry.weights, indices.x);
    const SkinVertex sv1 = LoadSkinVertex(geometry.joints, geometry.weights, indices.y);
    const SkinVertex sv2 = LoadSkinVertex(geometry.joints, geometry.weights, indices.z);

    // Per-vertex scalar field f is linear in the triangle plane, so grad = a*e1 + b*e2
    // solves dot(grad, e1) = f1 - f0 and dot(grad, e2) = f2 - f0.
    const float3 e1 = p1 - p0;
    const float3 e2 = p2 - p0;
    const float e1e1 = dot(e1, e1);
    const float e2e2 = dot(e2, e2);
    const float e1e2 = dot(e1, e2);
    const float det = e1e1 * e2e2 - e1e2 * e1e2;

    float sumSquaredGrad = 0.0;
    if (det > 1e-12)
    {
        for (uint joint = 0; joint < 100; joint++)
        {
            float f0 = 0.0, f1 = 0.0, f2 = 0.0;
            for (uint i = 0; i < 4; ++i)
            {
                if (sv0.joints[i] == joint) f0 += sv0.weights[i];
                if (sv1.joints[i] == joint) f1 += sv1.weights[i];
                if (sv2.joints[i] == joint) f2 += sv2.weights[i];
            }

            const float d1 = f1 - f0;
            const float d2 = f2 - f0;
            const float a = (d1 * e2e2 - d2 * e1e2) / det;
            const float b = (d2 * e1e1 - d1 * e1e2) / det;
            const float3 grad = a * e1 + b * e2;

            sumSquaredGrad += dot(grad, grad);
        }
    }

    return sqrt(sumSquaredGrad);
}

struct HitShading
{
    float3 color;
    float3 albedo;
    float3 reflectionOrigin;
    float3 reflectionDirection;
    float reflectivity;
};

// Direct lighting (sun + ambient + emissive) at a triangle hit, plus the reflectivity and the
// ray to continue with when the surface is reflective. Callers are responsible for tracing that
// ray on only when reflectivity is above threshold, and for combining its result back in with
// lerp(result.color, nextColor * result.albedo, result.reflectivity) - how that next trace
// happens (recursion vs. an iterative loop) is left to the caller.
HitShading ComputeHitShading(GeometryInfo geometry, BuiltInTriangleIntersectionAttributes attributes)
{
    HitShading result;
    result.albedo = 0.0;
    result.reflectionOrigin = 0.0;
    result.reflectionDirection = 0.0;
    result.reflectivity = 0.0;

    if (g_frame.shadeByBlas != 0)
    {
        result.color = DebugBlasColor(geometry.blasIndex);
        return result;
    }

    // Rays in a wave can hit different geometries, so every descriptor index derived
    // from the hit must be marked non-uniform or the whole wave reads one lane's resource.
    const uint base = PrimitiveIndex() * 3;
    const uint3 corner = uint3(LoadAccessorUint(geometry.indices, base),
                               LoadAccessorUint(geometry.indices, base + 1),
                               LoadAccessorUint(geometry.indices, base + 2));
    const float3 weights = float3(1.0 - attributes.barycentrics.x - attributes.barycentrics.y,
                                  attributes.barycentrics.x, attributes.barycentrics.y);

    VertexAttributes v0, v1, v2;
    v0.normal = LoadAccessorFloat3(geometry.normals, corner.x);
    v0.uv = LoadAccessorFloat2(geometry.uvs, corner.x);
    v1.normal = LoadAccessorFloat3(geometry.normals, corner.y);
    v1.uv = LoadAccessorFloat2(geometry.uvs, corner.y);
    v2.normal = LoadAccessorFloat3(geometry.normals, corner.z);
    v2.uv = LoadAccessorFloat2(geometry.uvs, corner.z);

    const float2 uv = v0.uv * weights.x + v1.uv * weights.y + v2.uv * weights.z;
    float3 normal;
    if (geometry.normals.bufferIndex == kInvalidIndex)
    {
        const float3 p0 = LoadAccessorFloat3(geometry.positions, corner.x);
        const float3 p1 = LoadAccessorFloat3(geometry.positions, corner.y);
        const float3 p2 = LoadAccessorFloat3(geometry.positions, corner.z);
        normal = normalize(cross(p1 - p0, p2 - p0));
    }
    else
    {
        normal = v0.normal * weights.x + v1.normal * weights.y + v2.normal * weights.z;
    }

    // Transform the normal from object space to world space using the baked normal transform.
    normal = mul((float3x3)geometry.bakeNormalTransform, normal);
    // Note that WorldToObject4x3() is the inverse transpose of ObjectToWorld4x3()
    float3 worldNormal = normalize(mul((float3x3)WorldToObject4x3(), normal));
    if (dot(worldNormal, WorldRayDirection()) > 0.0)
    {
        worldNormal = -worldNormal;
    }

    const StructuredBuffer<MaterialInfo> materialTable =
        BINDLESS(g_materialTables, NonUniformResourceIndex(geometry.materialTableIndex));
    const MaterialInfo material = materialTable[geometry.materialIndex];

    float3 albedo = material.baseColorFactor.rgb;
    if (material.baseColorTexture != kInvalidIndex)
    {
        const Texture2D<float4> baseColor =
            BINDLESS(g_textures, NonUniformResourceIndex(material.baseColorTexture));
        albedo *= baseColor.SampleLevel(g_sampler, uv, 0).rgb;
    }

    const float3 worldPosition = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    const float3 shadowOrigin = worldPosition + worldNormal * g_frame.rayEpsilon;
    const float3 toLight = -g_frame.sunDirection;

    const float nDotL = saturate(dot(worldNormal, toLight));
    const float shadow = nDotL > 0.0 ? TraceShadowRay(shadowOrigin, toLight) : 0.0;

    result.color = albedo * (g_frame.sunColor * nDotL * shadow + g_frame.ambientIntensity) + material.emissive;
    result.albedo = albedo;
    // Smooth metals reflect; everything else keeps a weak Fresnel-free specular term.
    result.reflectivity = lerp(0.02, 1.0, material.metallic) * (1.0 - material.roughness);
    result.reflectionOrigin = shadowOrigin;
    result.reflectionDirection = reflect(WorldRayDirection(), worldNormal);

    return result;
}

#endif // ASP_RAYTRACING_COMMON_HLSLI
