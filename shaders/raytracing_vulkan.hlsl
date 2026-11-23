#include "common.hlsli"

[[vk::binding(0, 1)]] ConstantBuffer<FrameConstants> g_frame;
[[vk::binding(1, 1)]] RaytracingAccelerationStructure g_scene;
[[vk::binding(2, 1)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> g_output;
[[vk::binding(3, 1)]] StructuredBuffer<GeometryInfo> g_geometryTable;

#include "raytracing_common.hlsli"

// AMD's Vulkan ray tracing driver only guarantees maxRayRecursionDepth == 1, so ClosestHit
// cannot recurse through TraceRay to chain reflection bounces. Instead RayGeneration traces one
// ray per bounce in a loop, and ClosestHit/Miss just report their result back to it instead of
// tracing the next ray themselves.
struct RayPayload
{
    float3 color;
    float3 albedo;
    float3 reflectionOrigin;
    float3 reflectionDirection;
    float reflectivity;
    uint hit;
};

// Bounds the bounce-chain arrays below, and so is the effective ceiling on --max-bounces.
static const uint kMaxBounceChain = 31;

[shader("raygeneration")]
void RayGeneration()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const float2 uv = (float2(pixel) + 0.5) / float2(DispatchRaysDimensions().xy);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    RayDesc ray;
    ray.Origin = g_frame.cameraPosition;
    ray.Direction = normalize(g_frame.cameraForward +
                              g_frame.cameraRight * (ndc.x * g_frame.aspectRatio * g_frame.tanHalfFovY) +
                              g_frame.cameraUp * (ndc.y * g_frame.tanHalfFovY));
    ray.TMin = 0.0;
    ray.TMax = 1e30;

    float3 chainColor[kMaxBounceChain];
    float3 chainAlbedo[kMaxBounceChain];
    float chainReflectivity[kMaxBounceChain];

    const uint maxBounces = min(g_frame.maxBounces, kMaxBounceChain - 1);
    uint chainLength = 0;
    for (uint depth = 0; depth <= maxBounces; ++depth)
    {
        RayPayload payload;
        TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

        chainColor[chainLength] = payload.color;
        chainAlbedo[chainLength] = payload.albedo;
        chainReflectivity[chainLength] = payload.reflectivity;
        ++chainLength;

        if (payload.hit == 0 || payload.reflectivity <= 0.01 || depth == maxBounces)
        {
            break;
        }

        ray.Origin = payload.reflectionOrigin;
        ray.Direction = payload.reflectionDirection;
    }

    // Fold the chain back-to-front: the recursive lerp(color, bounce.color * albedo, reflectivity)
    // unrolled into a loop.
    float3 color = chainColor[chainLength - 1];
    for (uint i = chainLength - 1; i > 0; --i)
    {
        const uint level = i - 1;
        color = lerp(chainColor[level], color * chainAlbedo[level], chainReflectivity[level]);
    }

    g_output[pixel] = float4(LinearToSrgb(color), 1.0);
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload.color = SampleSky(WorldRayDirection());
    payload.hit = 0;
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    const GeometryInfo geometry = g_geometryTable[InstanceID() + GeometryIndex()];
    const HitShading shading = ComputeHitShading(geometry, attributes);

    payload.color = shading.color;
    payload.albedo = shading.albedo;
    payload.reflectionOrigin = shading.reflectionOrigin;
    payload.reflectionDirection = shading.reflectionDirection;
    payload.reflectivity = shading.reflectivity;
    payload.hit = 1;
}
