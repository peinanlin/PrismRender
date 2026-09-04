#include "ShaderBindings.hlsli"

PRISM_VK_BINDING(32)
RWTexture2D<float4> sourceDisplacement : register(u0);
PRISM_VK_BINDING(33)
RWTexture2D<float4> sourceNormalFoam : register(u1);
PRISM_VK_BINDING(34)
RWTexture2D<float4> destinationDisplacement : register(u2);
PRISM_VK_BINDING(35)
RWTexture2D<float4> destinationNormalFoam : register(u3);

[numthreads(8, 8, 1)]
void DownsampleOceanMapsCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint destinationWidth = 0u;
    uint destinationHeight = 0u;
    destinationDisplacement.GetDimensions(
        destinationWidth,
        destinationHeight);
    const uint2 destinationCoordinate = dispatchThreadId.xy;
    if (destinationCoordinate.x >= destinationWidth
        || destinationCoordinate.y >= destinationHeight)
    {
        return;
    }

    const uint2 sourceCoordinate = destinationCoordinate * 2u;
    const uint2 sourceCoordinates[4] = {
        sourceCoordinate,
        sourceCoordinate + uint2(1u, 0u),
        sourceCoordinate + uint2(0u, 1u),
        sourceCoordinate + uint2(1u, 1u)};
    float4 displacementSum = 0.0.xxxx;
    float3 normalSum = 0.0.xxx;
    float foamSum = 0.0;
    [unroll]
    for (uint sampleIndex = 0u;
         sampleIndex < 4u;
         ++sampleIndex)
    {
        displacementSum += sourceDisplacement[
            sourceCoordinates[sampleIndex]];
        const float4 normalFoam = sourceNormalFoam[
            sourceCoordinates[sampleIndex]];
        normalSum += normalFoam.xyz * 2.0 - 1.0;
        foamSum += normalFoam.a;
    }

    const float4 filteredDisplacement = displacementSum * 0.25;
    const float3 filteredNormal = normalize(normalSum);
    const float filteredFoam = foamSum * 0.25;
    destinationDisplacement[destinationCoordinate] =
        filteredDisplacement;
    destinationNormalFoam[destinationCoordinate] = float4(
        filteredNormal * 0.5 + 0.5,
        filteredFoam);
}
