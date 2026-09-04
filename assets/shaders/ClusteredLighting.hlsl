#include "ShaderBindings.hlsli"

static const uint MaxLightsPerCluster = 64;

struct ClusterPointLight
{
    float3 position;
    float range;
    float3 color;
    float intensity;
};

PRISM_VK_BINDING(0) cbuffer ClusterConstants : register(b0)
{
    float4x4 worldToView;
    float4 viewportProjection;
    float4 depthParameters;
    uint tileCountX;
    uint tileCountY;
    uint depthSliceCount;
    uint lightCount;
};

PRISM_VK_BINDING(16)
StructuredBuffer<ClusterPointLight>
    pointLights : register(t0);
PRISM_VK_BINDING(32)
RWStructuredBuffer<uint>
    clusterLightCounts : register(u0);
PRISM_VK_BINDING(33)
RWStructuredBuffer<uint>
    clusterLightIndices : register(u1);

bool OverlapsTile(
    float3 viewPosition,
    float radius,
    uint tileX,
    uint tileY)
{
    if (viewPosition.z + radius <= 0.0f)
    {
        return false;
    }
    const float safeDepth =
        max(viewPosition.z, 0.001f);
    const float2 centerNdc =
        float2(
            viewPosition.x
                * viewportProjection.z,
            -viewPosition.y
                * viewportProjection.w)
        / safeDepth;
    const float2 radiusNdc =
        float2(
            radius
                * viewportProjection.z,
            radius
                * viewportProjection.w)
        / safeDepth;
    const float2 tileMinUv =
        float2(
            (float)tileX
                / (float)tileCountX,
            (float)tileY
                / (float)tileCountY);
    const float2 tileMaxUv =
        float2(
            (float)(tileX + 1u)
                / (float)tileCountX,
            (float)(tileY + 1u)
                / (float)tileCountY);
    const float2 lightMinUv =
        centerNdc * 0.5f + 0.5f
        - radiusNdc * 0.5f;
    const float2 lightMaxUv =
        centerNdc * 0.5f + 0.5f
        + radiusNdc * 0.5f;
    return all(lightMaxUv >= tileMinUv)
        && all(lightMinUv <= tileMaxUv);
}

[numthreads(64, 1, 1)]
void BuildLightClustersCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint clusterIndex =
        dispatchThreadId.x;
    const uint clusterCount =
        tileCountX
        * tileCountY
        * depthSliceCount;
    if (clusterIndex >= clusterCount)
    {
        return;
    }

    const uint clustersPerSlice =
        tileCountX * tileCountY;
    const uint sliceIndex =
        clusterIndex / clustersPerSlice;
    const uint tileIndex =
        clusterIndex
        - sliceIndex * clustersPerSlice;
    const uint tileY =
        tileIndex / tileCountX;
    const uint tileX =
        tileIndex - tileY * tileCountX;
    const float nearPlane =
        depthParameters.x;
    const float farPlane =
        depthParameters.y;
    const float sliceNear =
        nearPlane
        * pow(
            farPlane / nearPlane,
            (float)sliceIndex
                / (float)depthSliceCount);
    const float sliceFar =
        nearPlane
        * pow(
            farPlane / nearPlane,
            (float)(sliceIndex + 1u)
                / (float)depthSliceCount);

    uint outputCount = 0;
    [loop]
    for (uint lightIndex = 0;
         lightIndex < lightCount;
         ++lightIndex)
    {
        const ClusterPointLight light =
            pointLights[lightIndex];
        const float3 viewPosition =
            mul(
                float4(light.position, 1.0f),
                worldToView).xyz;
        const bool overlapsDepth =
            viewPosition.z + light.range
                    >= sliceNear
            && viewPosition.z - light.range
                    <= sliceFar;
        if (overlapsDepth
            && OverlapsTile(
                viewPosition,
                light.range,
                tileX,
                tileY)
            && outputCount
                < MaxLightsPerCluster)
        {
            clusterLightIndices[
                clusterIndex
                    * MaxLightsPerCluster
                + outputCount] =
                lightIndex;
            ++outputCount;
        }
    }
    clusterLightCounts[clusterIndex] =
        outputCount;
}
