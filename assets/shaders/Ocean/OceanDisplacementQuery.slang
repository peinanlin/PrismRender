#include "../ShaderBindings.hlsli"

// Query input/output records use world XZ positions and PrismRender's
// XZ-horizontal/Y-up convention. The pass is intentionally one thread per
// request so a caller can submit a bounded batch without a queue-wide wait.
PRISM_VK_BINDING(0) cbuffer OceanQueryConstants : register(b0)
{
    uint queryCount;
    uint resolution;
    uint localEnabled;
    uint padding;
    float simulationTimeSeconds;
    float domainSizeMeters;
    float2 domainCenter;
    float4 cascadePatchLengths;
    float4 uvWarpParameters;
};

PRISM_VK_BINDING(16) StructuredBuffer<float2> queryPoints
    : register(t0);
PRISM_VK_BINDING(17) Texture2DArray<float4> spectralDisplacement
    : register(t1);
PRISM_VK_BINDING(18) Texture2D<float4> localDisplacement
    : register(t2);
PRISM_VK_BINDING(32) RWStructuredBuffer<float4> queryResults
    : register(u0);

float3 SampleCascade(float2 worldPosition, uint cascade)
{
    const float patchLength = max(cascadePatchLengths[cascade], 0.001);
    float2 uv = worldPosition / patchLength;
    uv += uvWarpParameters.x * float2(cos(uv.y * uvWarpParameters.y),
        sin(uv.x * uvWarpParameters.y));
    const float2 texel = frac(uv) * float(resolution) - 0.5f;
    const int2 base = int2(floor(texel));
    const float2 blend = frac(texel);
    const int size = int(resolution);
    const int2 a = (base + size) % size;
    const int2 b = (base + 1 + size) % size;
    return lerp(lerp(spectralDisplacement.Load(int4(a, cascade, 0)).xyz,
            spectralDisplacement.Load(int4(b.x, a.y, cascade, 0)).xyz, blend.x),
        lerp(spectralDisplacement.Load(int4(a.x, b.y, cascade, 0)).xyz,
            spectralDisplacement.Load(int4(b, cascade, 0)).xyz, blend.x), blend.y);
}

float3 SampleDisplacement(float2 worldPosition)
{
    float3 displacement = 0.0.xxx;
    [unroll]
    for (uint cascade = 0u; cascade < 4u; ++cascade)
    {
        displacement += SampleCascade(worldPosition, cascade);
    }

    if (localEnabled != 0u)
    {
        const float2 localUv = (worldPosition - domainCenter)
            / max(domainSizeMeters, 0.001) + 0.5;
        if (all(localUv >= 0.0.xx) && all(localUv <= 1.0.xx))
        {
            uint width, height;
            localDisplacement.GetDimensions(width, height);
            const int2 size = int2(width, height);
            const float2 texel = localUv * float2(size) - 0.5f;
            const int2 base = int2(floor(texel));
            const int2 a = clamp(base, 0, size - 1);
            const int2 b = clamp(base + 1, 0, size - 1);
            const float2 blend = frac(texel);
            const float2 edge = min(localUv, 1.0f - localUv);
            const float weight = smoothstep(0.0f, 0.12f, min(edge.x, edge.y));
            displacement += weight * lerp(lerp(
                localDisplacement.Load(int3(a, 0)).xyz,
                localDisplacement.Load(int3(b.x, a.y, 0)).xyz, blend.x), lerp(
                localDisplacement.Load(int3(a.x, b.y, 0)).xyz,
                localDisplacement.Load(int3(b, 0)).xyz, blend.x), blend.y);
        }
    }
    return displacement;
}

[numthreads(64, 1, 1)]
void SampleOceanDisplacementCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint queryIndex = dispatchThreadId.x;
    if (queryIndex >= queryCount)
        return;
    const float2 worldPosition = queryPoints[queryIndex];
    // The query is Eulerian: invert choppy horizontal displacement to find
    // the undisplaced surface location directly below the camera.
    float2 undisplaced = worldPosition;
    [unroll]
    for (uint iteration = 0u; iteration < 3u; ++iteration)
        undisplaced = worldPosition - SampleDisplacement(undisplaced).xz;
    const float3 displacement = SampleDisplacement(undisplaced);
    queryResults[queryIndex] = float4(
        all(isfinite(displacement)) ? displacement : 0.0f.xxx, simulationTimeSeconds);
}
