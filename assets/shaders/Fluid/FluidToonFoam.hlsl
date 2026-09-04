#include "../ShaderBindings.hlsli"

PRISM_VK_BINDING(0) cbuffer FluidRenderConstants : register(b0)
{
    float4x4 fluidView;
    float4x4 fluidProjection;
    float4x4 fluidInverseProjection;
    float4x4 fluidInverseView;
    float4 fluidResolutionInverseResolution;
    float4 fluidCameraPositionParticleRadius;
    float4 fluidFilterParameters;
    float4 fluidDensityParameters;
    float4 fluidAbsorptionScattering;
    float4 fluidWaterColorIor;
    float4 fluidOpticalParameters;
    float4 fluidFoamParameters;
    float4 fluidToonParameters;
    float4 fluidLightDirectionOutline;
    uint4 fluidCountsAndModes;
};

// ReconstructNormalFoamCS binds smooth depth/mask. RefineFoamCS reuses the
// same SRV slots for raw foam/mask. The surface mask encodes the optional
// normalized-density foam interval while remaining a regular validity mask.
PRISM_VK_BINDING(16)
Texture2D<float> fluidPrimaryTexture : register(t0);
PRISM_VK_BINDING(17)
Texture2D<float> fluidMaskTexture : register(t1);
PRISM_VK_BINDING(32)
RWTexture2D<float4> fluidNormalOutput : register(u0);
PRISM_VK_BINDING(33)
RWTexture2D<float> fluidFoamRawOutput : register(u1);
PRISM_VK_BINDING(34)
RWTexture2D<float> fluidFoamRefinedOutput : register(u2);

#define fluidResolution fluidResolutionInverseResolution.xy
#define fluidInverseResolution fluidResolutionInverseResolution.zw
#define foamNeighborhoodThreshold fluidFoamParameters.z
#define fluidFoamIntensity fluidOpticalParameters.w
#define fluidNormalSmoothingRadius ((int)fluidFilterParameters.w)

bool IsFluidPixel(int2 pixel)
{
    const int2 clampedPixel = clamp(
        pixel,
        int2(0, 0),
        int2(fluidResolution) - 1);
    return fluidMaskTexture.Load(
        int3(clampedPixel, 0)) > 0.5f;
}

bool IsSurfacePixel(int2 pixel)
{
    const int2 clampedPixel = clamp(
        pixel,
        int2(0, 0),
        int2(fluidResolution) - 1);
    const float depth = fluidPrimaryTexture.Load(
        int3(clampedPixel, 0));
    return depth > 1.0e-5f && isfinite(depth);
}

float LoadNeighborDepth(int2 pixel, float fallback)
{
    const int2 clampedPixel = clamp(
        pixel,
        int2(0, 0),
        int2(fluidResolution) - 1);
    return IsSurfacePixel(clampedPixel)
        ? fluidPrimaryTexture.Load(
            int3(clampedPixel, 0))
        : fallback;
}

bool IsFoamCandidate(int2 pixel)
{
    const int2 clampedPixel = clamp(
        pixel,
        int2(0, 0),
        int2(fluidResolution) - 1);
    const float encodedMask = fluidMaskTexture.Load(
        int3(clampedPixel, 0));
    return encodedMask > 0.5f && encodedMask < 0.875f;
}

float3 ReconstructViewPosition(
    int2 pixel,
    float linearDepth)
{
    const float2 uv =
        (float2(pixel) + 0.5f) * fluidInverseResolution;
    const float2 ndc = uv * float2(2.0f, -2.0f)
        + float2(-1.0f, 1.0f);
    const float4 farViewHomogeneous = mul(
        float4(ndc, 1.0f, 1.0f),
        fluidInverseProjection);
    const float3 farView = farViewHomogeneous.xyz
        / max(abs(farViewHomogeneous.w), 1.0e-6f);
    return farView
        * (linearDepth / max(farView.z, 1.0e-6f));
}

[numthreads(8, 8, 1)]
void ReconstructNormalFoamCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= uint2(fluidResolution)))
    {
        return;
    }
    if (!IsSurfacePixel(int2(pixel)))
    {
        fluidNormalOutput[pixel] =
            float4(0.0f, 0.0f, -1.0f, 0.0f);
        fluidFoamRawOutput[pixel] = 0.0f;
        return;
    }

    const int2 centerPixel = int2(pixel);
    const float centerDepth = fluidPrimaryTexture.Load(
        int3(pixel, 0));
    // Keep the normal footprint at least as wide as the authored baseline,
    // then grow it with the projected particle radius. This is what prevents a
    // close camera from resolving individual sphere caps again.
    const float projectedParticleRadiusPixels =
        abs(fluidProjection[1][1])
        * fluidCameraPositionParticleRadius.w
        * fluidResolution.y * 0.5f
        / max(centerDepth, 1.0e-4f);
    const int normalRadius = clamp(
        max(
            fluidNormalSmoothingRadius,
            (int)ceil(projectedParticleRadiusPixels * 0.5f)),
        1,
        15);
    const float leftDepth = LoadNeighborDepth(
        centerPixel + int2(-normalRadius, 0), centerDepth);
    const float rightDepth = LoadNeighborDepth(
        centerPixel + int2(normalRadius, 0), centerDepth);
    const float upDepth = LoadNeighborDepth(
        centerPixel + int2(0, -normalRadius), centerDepth);
    const float downDepth = LoadNeighborDepth(
        centerPixel + int2(0, normalRadius), centerDepth);

    const float3 centerPosition = ReconstructViewPosition(
        centerPixel, centerDepth);
    const float3 leftPosition = ReconstructViewPosition(
        centerPixel + int2(-normalRadius, 0), leftDepth);
    const float3 rightPosition = ReconstructViewPosition(
        centerPixel + int2(normalRadius, 0), rightDepth);
    const float3 upPosition = ReconstructViewPosition(
        centerPixel + int2(0, -normalRadius), upDepth);
    const float3 downPosition = ReconstructViewPosition(
        centerPixel + int2(0, normalRadius), downDepth);

    const float3 dx = abs(leftDepth - centerDepth)
            < abs(rightDepth - centerDepth)
        ? centerPosition - leftPosition
        : rightPosition - centerPosition;
    const float3 dy = abs(upDepth - centerDepth)
            < abs(downDepth - centerDepth)
        ? centerPosition - upPosition
        : downPosition - centerPosition;
    float3 normal = normalize(cross(dy, dx));
    if (!all(isfinite(normal)))
    {
        normal = float3(0.0f, 0.0f, -1.0f);
    }
    if (normal.z > 0.0f)
    {
        normal = -normal;
    }

    fluidNormalOutput[pixel] = float4(normal, 0.0f);
    fluidFoamRawOutput[pixel] =
        fluidFoamIntensity > 0.0f
            && IsFoamCandidate(centerPixel)
        ? 1.0f
        : 0.0f;
}

[numthreads(8, 8, 1)]
void RefineFoamCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= uint2(fluidResolution)))
    {
        return;
    }
    if (!IsFluidPixel(int2(pixel)))
    {
        fluidFoamRefinedOutput[pixel] = 0.0f;
        return;
    }

    // This pass is an erosion, not a dilation: never create foam where the
    // density-classified center pixel was not already a candidate. The
    // reference implementation uses a radius-four, 64/81 support test.
    const float centerFoam = fluidPrimaryTexture.Load(
        int3(pixel, 0));
    if (centerFoam <= 0.5f)
    {
        fluidFoamRefinedOutput[pixel] = 0.0f;
        return;
    }

    static const int FoamErosionRadius = 4;
    float activeSamples = 0.0f;
    [loop]
    for (int y = -FoamErosionRadius;
         y <= FoamErosionRadius;
         ++y)
    {
        [loop]
        for (int x = -FoamErosionRadius;
             x <= FoamErosionRadius;
             ++x)
        {
            const int2 samplePixel = clamp(
                int2(pixel) + int2(x, y),
                int2(0, 0),
                int2(fluidResolution) - 1);
            const float sampleFoam = fluidPrimaryTexture.Load(
                int3(samplePixel, 0));
            activeSamples += sampleFoam > 0.5f
                ? 1.0f
                : 0.0f;
        }
    }
    const float support = activeSamples / 81.0f;
    // Preserve the existing cleanup control while mapping it into the useful
    // radius-four erosion range. A preset value of 0.74 becomes 0.796, closely
    // matching the reference 64/81 criterion.
    const float requiredSupport = lerp(
        0.50f,
        0.90f,
        saturate(foamNeighborhoodThreshold));
    fluidFoamRefinedOutput[pixel] = centerFoam * smoothstep(
        requiredSupport,
        min(requiredSupport + 0.08f, 0.999f),
        support);
}
