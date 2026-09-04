#include "../ShaderBindings.hlsli"

PRISM_VK_BINDING(0) cbuffer FluidCausticsConstants : register(b0)
{
    float4x4 inverseProjection;
    float2 resolution;
    float2 inverseResolution;
    float3 causticsTint;
    float causticsIntensity;
    float3 lightDirectionView;
    float indexOfRefraction;
    float3 receiverUpDirectionView;
    float receiverPlanePadding;
    float refractionScalePixels;
    float depthAttenuation;
    float focusStrength;
    float focusPower;
    float depthBias;
    float blurSigma;
    uint blurRadius;
    uint causticsEnabled;
};

PRISM_VK_BINDING(16)
Texture2D<float> fluidDepthTexture : register(t0);
PRISM_VK_BINDING(17)
Texture2D<float4> fluidNormalTexture : register(t1);
PRISM_VK_BINDING(18)
Texture2D<float> sceneDepthTexture : register(t2);
PRISM_VK_BINDING(19)
Texture2D<float4> causticsInput : register(t3);
PRISM_VK_BINDING(32)
RWTexture2D<float4> causticsOutput : register(u0);

static const uint MaximumBlurRadius = 12u;
static const int GatherHalfWidth = 5;
static const float PhotonDensityNormalization = 2.65f;

int2 ClampPixel(const int2 pixel)
{
    return clamp(
        pixel,
        int2(0, 0),
        int2(resolution) - 1);
}

bool LoadFluidNormal(
    const int2 pixel,
    out float3 normal)
{
    const float3 loaded = fluidNormalTexture.Load(
        int3(ClampPixel(pixel), 0)).xyz;
    const float lengthSquared = dot(loaded, loaded);
    if (lengthSquared < 1.0e-5f)
    {
        normal = float3(0.0f, 0.0f, -1.0f);
        return false;
    }
    normal = loaded * rsqrt(lengthSquared);
    return true;
}

float3 ReconstructViewPositionFromDeviceDepth(
    const float2 uv,
    const float deviceDepth)
{
    const float2 ndc = uv * float2(2.0f, -2.0f)
        + float2(-1.0f, 1.0f);
    const float4 viewHomogeneous = mul(
        float4(ndc, deviceDepth, 1.0f),
        inverseProjection);
    return viewHomogeneous.xyz
        / max(abs(viewHomogeneous.w), 1.0e-6f);
}

float3 ReconstructViewPositionFromLinearDepth(
    const float2 uv,
    const float linearDepth)
{
    const float2 ndc = uv * float2(2.0f, -2.0f)
        + float2(-1.0f, 1.0f);
    const float4 farViewHomogeneous = mul(
        float4(ndc, 1.0f, 1.0f),
        inverseProjection);
    const float3 farView = farViewHomogeneous.xyz
        / max(abs(farViewHomogeneous.w), 1.0e-6f);
    return farView
        * (linearDepth / max(farView.z, 1.0e-6f));
}

float EvaluateRayCoverage(
    const float3 surfacePosition,
    const float3 receiverPosition,
    const float3 incidentDirection,
    const float3 surfaceNormal,
    const float eta,
    const float2 receiverPixelSize,
    const float footprintRadiusPixels)
{
    const float3 refractedDirection = refract(
        incidentDirection,
        surfaceNormal,
        eta);
    const float refractedLengthSquared = dot(
        refractedDirection,
        refractedDirection);
    if (refractedLengthSquared <= 1.0e-6f
        || refractedDirection.z <= 1.0e-4f)
    {
        return 0.0f;
    }

    const float3 rayDirection = refractedDirection
        * rsqrt(refractedLengthSquared);
    const float rayDistance =
        (receiverPosition.z - surfacePosition.z)
        / rayDirection.z;
    if (rayDistance <= depthBias)
    {
        return 0.0f;
    }

    const float3 projectedReceiver = surfacePosition
        + rayDirection * rayDistance;
    const float2 errorPixels =
        (projectedReceiver.xy - receiverPosition.xy)
        / receiverPixelSize;
    const float inverseFootprintSquared = 1.0f
        / max(
            footprintRadiusPixels * footprintRadiusPixels,
            1.0e-4f);
    return exp2(
        -0.72134752044f
        * dot(errorPixels, errorPixels)
        * inverseFootprintSquared);
}

[numthreads(8, 8, 1)]
void GenerateCausticsCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= (uint)resolution.x
        || pixel.y >= (uint)resolution.y)
    {
        return;
    }
    if (causticsEnabled == 0u)
    {
        causticsOutput[pixel] = 0.0f.xxxx;
        return;
    }

    const int2 destinationPixel = int2(pixel);
    const float destinationSceneDepth = sceneDepthTexture.Load(
        int3(destinationPixel, 0));
    if (destinationSceneDepth >= 0.999999f)
    {
        causticsOutput[pixel] = 0.0f.xxxx;
        return;
    }

    const float2 receiverUv =
        (float2(destinationPixel) + 0.5f)
        * inverseResolution;
    const float3 receiverPosition =
        ReconstructViewPositionFromDeviceDepth(
            receiverUv,
            destinationSceneDepth);
    const int2 rightPixel = ClampPixel(
        destinationPixel + int2(1, 0));
    const int2 downPixel = ClampPixel(
        destinationPixel + int2(0, 1));
    const float2 rightUv = min(
        receiverUv + float2(inverseResolution.x, 0.0f),
        1.0f.xx);
    const float2 downUv = min(
        receiverUv + float2(0.0f, inverseResolution.y),
        1.0f.xx);
    const float3 receiverRight =
        ReconstructViewPositionFromDeviceDepth(
            rightUv,
            destinationSceneDepth);
    const float3 receiverDown =
        ReconstructViewPositionFromDeviceDepth(
            downUv,
            destinationSceneDepth);
    const float rightSceneDepth = sceneDepthTexture.Load(
        int3(rightPixel, 0));
    const float downSceneDepth = sceneDepthTexture.Load(
        int3(downPixel, 0));
    const float3 receiverRightSurface =
        ReconstructViewPositionFromDeviceDepth(
            rightUv,
            rightSceneDepth);
    const float3 receiverDownSurface =
        ReconstructViewPositionFromDeviceDepth(
            downUv,
            downSceneDepth);
    const float3 receiverNormal = normalize(cross(
        receiverRightSurface - receiverPosition,
        receiverDownSurface - receiverPosition));
    const float receiverPlaneWeight = saturate(
        (abs(dot(
            receiverNormal,
            normalize(receiverUpDirectionView))) - 0.45f)
        / 0.45f);
    if (!all(isfinite(receiverNormal))
        || receiverPlaneWeight <= 1.0e-4f)
    {
        causticsOutput[pixel] = 0.0f.xxxx;
        return;
    }
    const float2 receiverPixelSize = max(
        float2(
            length(receiverRight.xy - receiverPosition.xy),
            length(receiverDown.xy - receiverPosition.xy)),
        1.0e-5f.xx);

    const float searchStepPixels = max(
        refractionScalePixels / (float)GatherHalfWidth,
        1.0f);
    const float footprintRadiusPixels = max(
        searchStepPixels * 0.45f,
        1.25f);
    const float attenuation = max(depthAttenuation, 0.0f);
    const float3 incidentDirection = normalize(
        lightDirectionView);
    const float centerIor = max(indexOfRefraction, 1.0001f);
    const float3 eta = 1.0f / max(
        float3(
            centerIor - 0.018f,
            centerIor,
            centerIor + 0.018f),
        1.0001f.xxx);
    float3 gatheredPhotons = 0.0f.xxx;

    // Each opaque receiver pixel gathers a bounded set of possible fluid
    // sources. This writes beyond the visible fluid silhouette without UAV
    // atomics, while refracted-ray convergence still raises local energy.
    [unroll]
    for (int gatherY = -GatherHalfWidth;
         gatherY <= GatherHalfWidth;
         ++gatherY)
    {
        [unroll]
        for (int gatherX = -GatherHalfWidth;
             gatherX <= GatherHalfWidth;
             ++gatherX)
        {
            const float2 sourcePixelFloat =
                float2(destinationPixel)
                + float2(gatherX, gatherY)
                    * searchStepPixels;
            const int2 sourcePixel = ClampPixel(
                int2(round(sourcePixelFloat)));
            const float sourceFluidDepth =
                fluidDepthTexture.Load(
                    int3(sourcePixel, 0));
            float3 sourceNormal;
            if (sourceFluidDepth <= depthBias
                || !LoadFluidNormal(
                    sourcePixel,
                    sourceNormal))
            {
                continue;
            }

            const float2 sourceUv =
                (float2(sourcePixel) + 0.5f)
                * inverseResolution;
            const float3 surfacePosition =
                ReconstructViewPositionFromLinearDepth(
                    sourceUv,
                    sourceFluidDepth);
            const float3 surfaceToReceiver =
                receiverPosition - surfacePosition;
            if (surfaceToReceiver.z <= depthBias)
            {
                continue;
            }

            if (dot(incidentDirection, sourceNormal) > 0.0f)
            {
                sourceNormal = -sourceNormal;
            }
            const float transmission = saturate(
                -dot(incidentDirection, sourceNormal));
            if (transmission <= 1.0e-4f)
            {
                continue;
            }

            const float receiverAttenuation = exp2(
                -length(surfaceToReceiver)
                    * attenuation);
            const float sourceEnergy = receiverAttenuation
                * lerp(0.35f, 1.0f, transmission);
            gatheredPhotons.r += sourceEnergy
                * EvaluateRayCoverage(
                    surfacePosition,
                    receiverPosition,
                    incidentDirection,
                    sourceNormal,
                    eta.r,
                    receiverPixelSize,
                    footprintRadiusPixels);
            gatheredPhotons.g += sourceEnergy
                * EvaluateRayCoverage(
                    surfacePosition,
                    receiverPosition,
                    incidentDirection,
                    sourceNormal,
                    eta.g,
                    receiverPixelSize,
                    footprintRadiusPixels);
            gatheredPhotons.b += sourceEnergy
                * EvaluateRayCoverage(
                    surfacePosition,
                    receiverPosition,
                    incidentDirection,
                    sourceNormal,
                    eta.b,
                    receiverPixelSize,
                    footprintRadiusPixels);
        }
    }

    const float3 photonDensity = gatheredPhotons
        / PhotonDensityNormalization;
    const float3 densityExcess = max(
        photonDensity - 0.08f.xxx,
        0.0f.xxx);
    const float3 focusedEnergy = pow(
        min(
            densityExcess * max(focusStrength, 0.0f),
            16.0f.xxx),
        max(focusPower, 0.1f).xxx);
    const float3 energy = min(
        max(causticsIntensity, 0.0f)
            * focusedEnergy * 0.14f
            * receiverPlaneWeight,
        32.0f.xxx);
    causticsOutput[pixel] = float4(
        max(causticsTint, 0.0f.xxx) * energy,
        max(energy.r, max(energy.g, energy.b)));
}

void BlurCaustics(
    const uint2 pixel,
    const int2 direction)
{
    if (pixel.x >= (uint)resolution.x
        || pixel.y >= (uint)resolution.y)
    {
        return;
    }
    if (causticsEnabled == 0u)
    {
        causticsOutput[pixel] = 0.0f.xxxx;
        return;
    }

    const int radius = (int)min(
        blurRadius,
        MaximumBlurRadius);
    if (radius == 0)
    {
        causticsOutput[pixel] = causticsInput.Load(
            int3(pixel, 0));
        return;
    }

    const float sigma = max(blurSigma, 0.25f);
    const float inverseTwoSigmaSquared =
        0.5f / (sigma * sigma);
    float4 accumulated = 0.0f.xxxx;
    float totalWeight = 0.0f;
    [unroll]
    for (int offset = -(int)MaximumBlurRadius;
         offset <= (int)MaximumBlurRadius;
         ++offset)
    {
        if (abs(offset) > radius)
        {
            continue;
        }
        const float weight = exp2(
            -(float)(offset * offset)
                * inverseTwoSigmaSquared
                * 1.44269504089f);
        const int2 samplePixel = ClampPixel(
            int2(pixel) + direction * offset);
        accumulated += causticsInput.Load(
            int3(samplePixel, 0)) * weight;
        totalWeight += weight;
    }
    causticsOutput[pixel] = accumulated
        / max(totalWeight, 1.0e-5f);
}

[numthreads(8, 8, 1)]
void BlurCausticsHorizontalCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    BlurCaustics(dispatchThreadId.xy, int2(1, 0));
}

[numthreads(8, 8, 1)]
void BlurCausticsVerticalCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    BlurCaustics(dispatchThreadId.xy, int2(0, 1));
}
