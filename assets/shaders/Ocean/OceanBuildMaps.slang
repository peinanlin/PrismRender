#include "../ShaderBindings.hlsli"

PRISM_VK_BINDING(0) cbuffer OceanMapConstants : register(b0)
{
    uint resolution;
    uint mapMipLevel;
    uint destinationResolution;
    uint mapPadding;
    float4 cascadePatchLengths;
};

PRISM_VK_BINDING(16) Texture2DArray<float4> spatialSpectrumA
    : register(t0);
PRISM_VK_BINDING(17) Texture2DArray<float4> spatialSpectrumB
    : register(t1);
PRISM_VK_BINDING(32) RWTexture2DArray<float4> outputDisplacement
    : register(u0);
PRISM_VK_BINDING(33) RWTexture2DArray<float4> outputGradient
    : register(u1);
PRISM_VK_BINDING(34) RWTexture2DArray<float4> outputMoments
    : register(u2);
PRISM_VK_BINDING(35) RWTexture2DArray<float4> sourceDisplacement
    : register(u3);
PRISM_VK_BINDING(36) RWTexture2DArray<float4> sourceGradient
    : register(u4);
PRISM_VK_BINDING(37) RWTexture2DArray<float4> sourceMoments
    : register(u5);
PRISM_VK_BINDING(38) RWTexture2DArray<float4> sourceFoam
    : register(u6);
PRISM_VK_BINDING(39) RWTexture2DArray<float4> outputFoam
    : register(u7);

// Mirrors OceanMapBuilder::FoldingResponseScale.  The raw Jacobian remains in
// displacement.w; this calibration only maps its deficit to the reference
// whitecap threshold range exposed by the Lab.
static const float FoldingResponseScale = 2.25;

float CheckerboardSign(uint2 coordinate)
{
    return ((coordinate.x + coordinate.y) & 1u) != 0u ? -1.0 : 1.0;
}

float3 SpatialDisplacement(uint2 coordinate, uint cascade)
{
    const float normalization = 1.0 / float(resolution * resolution);
    const float sign = CheckerboardSign(coordinate) * normalization;
    const float4 a = spatialSpectrumA.Load(int4(coordinate, cascade, 0));
    const float4 b = spatialSpectrumB.Load(int4(coordinate, cascade, 0));
    return float3(a.z, a.x, b.x) * sign;
}

[numthreads(8, 8, 1)]
void BuildOceanMapsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= resolution || dispatchThreadId.y >= resolution
        || dispatchThreadId.z >= 4u)
    {
        return;
    }
    const uint2 coordinate = dispatchThreadId.xy;
    const uint cascade = dispatchThreadId.z;
    const uint2 left = uint2((coordinate.x + resolution - 1u) % resolution,
        coordinate.y);
    const uint2 right = uint2((coordinate.x + 1u) % resolution,
        coordinate.y);
    const uint2 down = uint2(coordinate.x,
        (coordinate.y + resolution - 1u) % resolution);
    const uint2 up = uint2(coordinate.x,
        (coordinate.y + 1u) % resolution);
    const float3 center = SpatialDisplacement(coordinate, cascade);
    const float3 deltaX = (SpatialDisplacement(right, cascade)
        - SpatialDisplacement(left, cascade))
        * (0.5 * float(resolution) / cascadePatchLengths[cascade]);
    const float3 deltaZ = (SpatialDisplacement(up, cascade)
        - SpatialDisplacement(down, cascade))
        * (0.5 * float(resolution) / cascadePatchLengths[cascade]);

    const float3 tangentX = float3(1.0 + deltaX.x,
        deltaX.y, deltaX.z);
    const float3 tangentZ = float3(deltaZ.x,
        deltaZ.y, 1.0 + deltaZ.z);
    float3 normal = cross(tangentZ, tangentX);
    const float normalLengthSquared = dot(normal, normal);
    normal = normalLengthSquared > 0.00000001
        ? normal * rsqrt(normalLengthSquared) : float3(0.0, 1.0, 0.0);
    const float deformationX = 1.0 + deltaX.x;
    const float deformationZ = 1.0 + deltaZ.z;
    const float jacobian = deformationX * deformationZ
        - deltaZ.x * deltaX.z;
    // The determinant measures total area compression, so two mild
    // compressions can look like one broad breaking region. Tessendorf's
    // folding matrix identifies onset along its minimum principal direction.
    // The FFT displacement is potential-derived and should be symmetric; use
    // the averaged off-diagonal to remain stable under finite differences.
    const float symmetricOffDiagonal = 0.5 * (deltaZ.x + deltaX.z);
    const float eigenDiscriminant = sqrt(max(
        (deformationX - deformationZ)
            * (deformationX - deformationZ)
            + 4.0 * symmetricOffDiagonal * symmetricOffDiagonal,
        0.0));
    const float minimumPrincipalStretch = 0.5
        * (deformationX + deformationZ - eigenDiscriminant);
    const float folding = max(0.0, 1.0 - minimumPrincipalStretch)
        * FoldingResponseScale;
    const float2 slopes = float2(deltaX.y, deltaZ.y);

    outputDisplacement[dispatchThreadId] = float4(center, jacobian);
    outputGradient[dispatchThreadId] = float4(normal, folding);
    outputMoments[dispatchThreadId] = float4(
        slopes, slopes.x * slopes.x, slopes.y * slopes.y);
}

[numthreads(8, 8, 1)]
void DownsampleOceanMapsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= destinationResolution
        || dispatchThreadId.y >= destinationResolution
        || dispatchThreadId.z >= 4u)
    {
        return;
    }
    const uint3 sourceBase = uint3(dispatchThreadId.xy * 2u,
        dispatchThreadId.z);
    float4 displacement = 0.0.xxxx;
    float4 gradient = 0.0.xxxx;
    float4 moments = 0.0.xxxx;
    float foldingAverage = 0.0;
    float foldingMaximum = 0.0;
    float foamAverage = 0.0;
    float foamMaximum = 0.0;
    [unroll]
    for (uint y = 0u; y < 2u; ++y)
    {
        [unroll]
        for (uint x = 0u; x < 2u; ++x)
        {
            const int3 location = int3(sourceBase.xy + uint2(x, y),
                sourceBase.z);
            displacement += sourceDisplacement.Load(location);
            const float4 sourceNormal = sourceGradient.Load(location);
            gradient.xyz += sourceNormal.xyz;
            const float foldingSample = max(sourceNormal.w, 0.0);
            foldingAverage += foldingSample;
            foldingMaximum = max(foldingMaximum, foldingSample);
            moments += sourceMoments.Load(location);
            const float foam = saturate(sourceFoam.Load(location).x);
            foamAverage += foam;
            foamMaximum = max(foamMaximum, foam);
        }
    }
    displacement *= 0.25;
    moments *= 0.25;
    foldingAverage *= 0.25;
    foamAverage *= 0.25;
    const float normalLengthSquared = dot(gradient.xyz, gradient.xyz);
    gradient.xyz = normalLengthSquared > 0.00000001
        ? gradient.xyz * rsqrt(normalLengthSquared)
        : float3(0.0, 1.0, 0.0);
    gradient.w = max(foldingAverage, foldingMaximum * 0.35);
    outputDisplacement[dispatchThreadId] = displacement;
    outputGradient[dispatchThreadId] = gradient;
    outputMoments[dispatchThreadId] = moments;
    // Preserve aggregate coverage at distance without recursively spreading a
    // single hot texel across the horizon. The old 0.75 maximum term survived
    // every mip level and turned sparse crests into a distant white sheet.
    const float filteredFoam = saturate(max(
        foamAverage, foamMaximum * 0.35));
    outputFoam[dispatchThreadId] = float4(
        filteredFoam, 0.0, 0.0, 0.0);
}
