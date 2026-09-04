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

PRISM_VK_BINDING(16)
Texture2D<float> sourceDepthTexture : register(t0);
PRISM_VK_BINDING(32)
RWTexture2D<float> filteredDepthOutput : register(u0);

#define fluidResolution fluidResolutionInverseResolution.xy
#define fluidInverseResolution fluidResolutionInverseResolution.zw
#define bilateralRadius ((int)fluidFilterParameters.x)
#define bilateralSpatialSigma fluidFilterParameters.y
#define bilateralDepthSigma fluidFilterParameters.z
#define fluidParticleRadius fluidCameraPositionParticleRadius.w

static const int MaximumFilterRadius = 15;
static const int MaximumGapRadius = 7;
static const float MinimumSurfaceDepth = 1.0e-5f;

float LoadSurfaceDepth(int2 pixel)
{
    const int2 clampedPixel = clamp(
        pixel,
        int2(0, 0),
        int2(fluidResolution) - 1);
    return sourceDepthTexture.Load(int3(clampedPixel, 0));
}

bool IsSurfaceDepth(float depth)
{
    return depth > MinimumSurfaceDepth && isfinite(depth);
}

// Close only a bounded gap with valid depth on both sides. Unlike dilation,
// this fills holes and bead-shaped notches without growing the outer fluid
// silhouette into the background.
bool ReconstructGapDepth(
    int2 pixel,
    int2 direction,
    out float reconstructedDepth)
{
    float negativeDepth = 0.0f;
    float positiveDepth = 0.0f;
    int negativeDistance = 0;
    int positiveDistance = 0;
    const float immediateNegative = LoadSurfaceDepth(
        pixel - direction);
    const float immediatePositive = LoadSurfaceDepth(
        pixel + direction);
    if (IsSurfaceDepth(immediateNegative))
    {
        negativeDepth = immediateNegative;
        negativeDistance = 1;
    }
    if (IsSurfaceDepth(immediatePositive))
    {
        positiveDepth = immediatePositive;
        positiveDistance = 1;
    }
    // Most threads are far from the silhouette. Two taps reject them before
    // the wider bounded-gap search, keeping the closing operation inexpensive.
    if (negativeDistance == 0 && positiveDistance == 0)
    {
        reconstructedDepth = 0.0f;
        return false;
    }
    [unroll]
    for (int distance = 2;
         distance <= MaximumGapRadius;
         ++distance)
    {
        if (distance > bilateralRadius)
        {
            break;
        }
        if (negativeDistance == 0)
        {
            const float candidate = LoadSurfaceDepth(
                pixel - direction * distance);
            if (IsSurfaceDepth(candidate))
            {
                negativeDepth = candidate;
                negativeDistance = distance;
            }
        }
        if (positiveDistance == 0)
        {
            const float candidate = LoadSurfaceDepth(
                pixel + direction * distance);
            if (IsSurfaceDepth(candidate))
            {
                positiveDepth = candidate;
                positiveDistance = distance;
            }
        }
    }
    if (negativeDistance == 0 || positiveDistance == 0)
    {
        reconstructedDepth = 0.0f;
        return false;
    }

    // Do not bridge two unrelated depth layers that happen to overlap in
    // screen space. The threshold scales with both the user range sigma and
    // the rendered particle diameter.
    const float bridgeThreshold = max(
        bilateralDepthSigma * 0.75f,
        fluidParticleRadius * 4.0f);
    if (abs(positiveDepth - negativeDepth) > bridgeThreshold)
    {
        reconstructedDepth = 0.0f;
        return false;
    }
    const float interpolation =
        (float)negativeDistance
        / (float)(negativeDistance + positiveDistance);
    reconstructedDepth = lerp(
        negativeDepth,
        positiveDepth,
        interpolation);
    return true;
}

float FilterDepth(uint2 pixel, int2 direction)
{
    const int2 centerPixel = int2(pixel);
    float centerDepth = LoadSurfaceDepth(centerPixel);
    if (!IsSurfaceDepth(centerDepth)
        && !ReconstructGapDepth(
            centerPixel,
            direction,
            centerDepth))
    {
        return 0.0f;
    }

    const int2 resolution = int2(fluidResolution);
    // Each pass samples every intervening pixel. The previous adaptive stride
    // skipped samples while still weighting them as adjacent taps, producing
    // periodic rings when particles occupied many pixels near the camera.
    const float effectiveSpatialSigma = max(
        bilateralSpatialSigma,
        1.0f);
    const float inverseSpatialVariance = 0.5f
        / max(
            effectiveSpatialSigma * effectiveSpatialSigma,
            1.0e-6f);
    const float inverseDepthVariance = 0.5f
        / max(
            bilateralDepthSigma * bilateralDepthSigma,
            1.0e-8f);
    float weightedDepth = 0.0f;
    float totalWeight = 0.0f;
    [loop]
    for (int offset = -MaximumFilterRadius;
         offset <= MaximumFilterRadius;
         ++offset)
    {
        if (abs(offset) > bilateralRadius)
        {
            continue;
        }
        const int2 samplePixel = clamp(
            int2(pixel) + direction * offset,
            int2(0, 0),
            resolution - 1);
        const float sampleDepth = sourceDepthTexture.Load(
            int3(samplePixel, 0));
        if (!IsSurfaceDepth(sampleDepth))
        {
            continue;
        }
        const float depthDifference =
            sampleDepth - centerDepth;
        const float weight = exp(
            -(float)(offset * offset) * inverseSpatialVariance
            - depthDifference * depthDifference
                * inverseDepthVariance);
        weightedDepth += sampleDepth * weight;
        totalWeight += weight;
    }
    return totalWeight > 1.0e-6f
        ? weightedDepth / totalWeight
        : centerDepth;
}

[numthreads(8, 8, 1)]
void BilateralHorizontalCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= uint2(fluidResolution)))
    {
        return;
    }
    filteredDepthOutput[pixel] = FilterDepth(
        pixel, int2(1, 0));
}

[numthreads(8, 8, 1)]
void BilateralVerticalCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= uint2(fluidResolution)))
    {
        return;
    }
    filteredDepthOutput[pixel] = FilterDepth(
        pixel, int2(0, 1));
}
