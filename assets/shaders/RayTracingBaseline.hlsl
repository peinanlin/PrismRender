#include "ShaderBindings.hlsli"

struct RayPayload
{
    float3 color;
};

PRISM_VK_BINDING(16)
RaytracingAccelerationStructure SceneAccelerationStructure
    : register(t0);
PRISM_VK_BINDING(32)
RWTexture2D<float4> RayTracingOutput : register(u0);

[shader("raygeneration")]
void RayGenerationMain()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    const float2 uv =
        (float2(pixel) + 0.5f) / float2(dimensions);

    RayDesc ray;
    ray.Origin = float3(0.0f, 0.0f, -2.0f);
    ray.Direction = normalize(
        float3(uv * 2.0f - 1.0f, 1.5f));
    ray.TMin = 0.001f;
    ray.TMax = 10000.0f;

    RayPayload payload;
    payload.color = float3(0.0f, 0.0f, 0.0f);
    TraceRay(
        SceneAccelerationStructure,
        RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
        0xff,
        0,
        1,
        0,
        ray,
        payload);
    RayTracingOutput[pixel] =
        float4(payload.color, 1.0f);
}

[shader("miss")]
void MissMain(inout RayPayload payload)
{
    payload.color = float3(0.08f, 0.16f, 0.3f);
}

[shader("closesthit")]
void ClosestHitMain(
    inout RayPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    payload.color = float3(
        attributes.barycentrics,
        1.0f - attributes.barycentrics.x
            - attributes.barycentrics.y);
}
