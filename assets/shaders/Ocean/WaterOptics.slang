#include "../ShaderBindings.hlsli"

static const uint WaterRefractionValid = 0u;
static const uint WaterRefractionOffScreen = 1u;
static const uint WaterRefractionForeground = 2u;
static const uint WaterRefractionZeroThickness = 3u;
static const uint WaterRefractionNonFinite = 4u;
static const uint WaterRefractionNoIntersection = 5u;

struct WaterFramePointLight
{
    float3 position;
    float range;
    float3 color;
    float intensity;
};

struct WaterFrameDirectionalLight
{
    float3 direction;
    float intensity;
    float3 color;
    float padding;
};

// This prefix and trailing ocean metadata mirror SharedFrameConstants. The
// dedicated water composite uses lighting data directly and does not include
// Mesh.hlsl or the ordinary opaque BRDF/material bindings.
PRISM_VK_BINDING(0) cbuffer WaterFrameConstants : register(b0)
{
    float3 cameraPosition;
    float pointLightCount;
    float3 directionalLightDirection;
    float directionalLightIntensity;
    float3 directionalLightColor;
    float ambientIntensity;
    WaterFramePointLight pointLights[4];
    float4x4 lightViewProjections[3];
    float4 cascadeSplits;
    float3 skyZenithColor;
    float pbrEnabled;
    float3 skyHorizonColor;
    float iblEnabled;
    float3 groundColor;
    float iblIntensity;
    float iblDiffuseStrength;
    float iblSpecularStrength;
    float iblReflectionBlend;
    float iblHorizonSharpness;
    float iblSplitSumEnabled;
    float shadowBias;
    float shadowsEnabled;
    float gtaoEnabled;
    float directLightingEnabled;
    float pointLightsEnabled;
    float normalMappingEnabled;
    float occlusionEnabled;
    float emissiveEnabled;
    float alphaMaskEnabled;
    float clusteredLightingEnabled;
    float shadowFilterMode;
    float3 cameraForward;
    float cascadeBlendFraction;
    float3 oceanDeepWaterColor;
    float physicalAtmosphereEnabled;
    float3 oceanScatteringColor;
    float atmosphereBrightness;
    float3 oceanFoamColor;
    float oceanScatteringStrength;
    WaterFrameDirectionalLight auxiliaryDirectionalLights[2];
    float4 oceanCascadePatchLengths;
    float4 oceanCascadeLowerWavelengths;
    float4 oceanCascadeUpperWavelengths;
    float4 oceanCascadeUvScales;
    float4 oceanCascadeUvOffsetX;
    float4 oceanCascadeUvOffsetY;
    float4 oceanCascadeFadeStarts;
    float4 oceanCascadeFadeEnds;
    float4 oceanCascadeLayerOrder;
};

PRISM_VK_BINDING(5) cbuffer WaterOpticsConstants : register(b5)
{
    float4 waterAbsorptionRoughness;
    float4 waterScatteringIor;
    float4 waterPhaseThinBacklit;
    float4 waterDistanceRanges;
    float4 waterRefractionParameters0;
    float4 waterRefractionParameters1;
    float4 waterCausticsParameters;
    float4 waterVolumetricParameters0;
    float4 waterVolumetricParameters1;
    uint4 waterModes0;
    uint4 waterModes1;
    uint4 waterModes2;
    float4x4 waterInverseViewProjection;
};

#include "../ShadowFiltering.hlsli"

struct ApproximateRefractionResult
{
    float2 sampleUv;
    float thickness;
    uint fallback;
};

bool WaterFinite2(float2 value)
{
    return all(isfinite(value));
}

float2 WaterSafeOriginalUv(float2 uv)
{
    return WaterFinite2(uv) ? saturate(uv) : 0.5f.xx;
}

struct WaterOpticalResponse
{
    float3 transmittance;
    float3 singleScattering;
    float3 thinLayer;
    float3 backlit;
    float fresnel;
    float phase;
};

struct WaterFoamResponse
{
    float effectiveFoam;
    float roughness;
    float refractionWeight;
    float diffuseWeight;
};

float WaterFiniteOr(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

WaterOpticalResponse EvaluateWaterOpticalResponse(
    float3 absorption,
    float3 scattering,
    float indexOfRefraction,
    float phaseG,
    float thinLayerStrength,
    float backlitStrength,
    float viewCosine,
    float lightCosine,
    float thickness)
{
    WaterOpticalResponse result;
    absorption = clamp(float3(
        WaterFiniteOr(absorption.x, 0.0f),
        WaterFiniteOr(absorption.y, 0.0f),
        WaterFiniteOr(absorption.z, 0.0f)), 0.0f.xxx, 100.0f.xxx);
    scattering = clamp(float3(
        WaterFiniteOr(scattering.x, 0.0f),
        WaterFiniteOr(scattering.y, 0.0f),
        WaterFiniteOr(scattering.z, 0.0f)), 0.0f.xxx, 100.0f.xxx);
    const float safeIor = clamp(WaterFiniteOr(
        indexOfRefraction, 1.333f), 1.0001f, 3.0f);
    const float safeG = clamp(WaterFiniteOr(phaseG, 0.0f), -0.95f, 0.95f);
    const float safeViewCosine = saturate(WaterFiniteOr(viewCosine, 1.0f));
    const float safeLightCosine = clamp(WaterFiniteOr(
        lightCosine, 0.0f), -1.0f, 1.0f);
    const float safeThickness = clamp(WaterFiniteOr(
        thickness, 0.0f), 0.0f, 10000.0f);
    const float safeThinStrength = clamp(WaterFiniteOr(
        thinLayerStrength, 0.0f), 0.0f, 8.0f);
    const float safeBacklitStrength = clamp(WaterFiniteOr(
        backlitStrength, 0.0f), 0.0f, 8.0f);
    const float ratio = (safeIor - 1.0f) / (safeIor + 1.0f);
    const float f0 = ratio * ratio;
    result.fresnel = saturate(f0 + (1.0f - f0)
        * pow(1.0f - safeViewCosine, 5.0f));
    const float phaseDenominator = max(1.0f + safeG * safeG
        - 2.0f * safeG * safeLightCosine, 1.0e-4f);
    result.phase = clamp((1.0f - safeG * safeG)
        / (4.0f * 3.14159265358979323846f
            * pow(phaseDenominator, 1.5f)), 0.0f, 16.0f);
    const float3 extinction = absorption + scattering;
    result.transmittance = exp(-min(
        extinction * safeThickness, 80.0f.xxx));
    const float3 albedo = float3(
        extinction.x > 1.0e-6f ? scattering.x / extinction.x : 0.0f,
        extinction.y > 1.0e-6f ? scattering.y / extinction.y : 0.0f,
        extinction.z > 1.0e-6f ? scattering.z / extinction.z : 0.0f);
    result.singleScattering = clamp((1.0f.xxx - result.transmittance)
        * albedo * result.phase, 0.0f.xxx, 16.0f.xxx);
    result.thinLayer = clamp(result.transmittance
        * (1.0f - result.fresnel) * (1.0f - safeViewCosine)
        * safeThinStrength, 0.0f.xxx, 8.0f.xxx);
    result.backlit = clamp((1.0f.xxx - result.transmittance)
        * albedo * max(-safeLightCosine, 0.0f) * safeBacklitStrength,
        0.0f.xxx, 8.0f.xxx);
    return result;
}

float WaterFoamWorldDetail(float3 worldPosition)
{
    // Stable project-owned detail. Both frequencies are tied to world space,
    // so camera movement cannot make the aerated pattern swim.
    const float2 p = worldPosition.xz;
    const float low = sin(dot(p, float2(0.173f, 0.127f)) + 1.31f);
    const float high = sin(dot(p, float2(-0.619f, 0.733f)) - 0.47f);
    return saturate(0.5f + 0.25f * low + 0.25f * high);
}

WaterFoamResponse EvaluateWaterFoamResponse(
    float simulatedFoam,
    float worldDetail,
    float baseRoughness)
{
    WaterFoamResponse result;
    const float safeFoam = saturate(WaterFiniteOr(simulatedFoam, 0.0f));
    const float safeDetail = saturate(WaterFiniteOr(worldDetail, 0.5f));
    const float safeRoughness = clamp(
        WaterFiniteOr(baseRoughness, 0.08f), 0.001f, 1.0f);
    result.effectiveFoam = 0.0f;
    result.roughness = safeRoughness;
    result.refractionWeight = 1.0f;
    result.diffuseWeight = 0.0f;
    if (safeFoam <= 1.0e-5f)
        return result;

    const float detailModulation = clamp(
        0.72f + safeDetail * 0.56f, 0.72f, 1.28f);
    result.effectiveFoam = saturate(safeFoam * detailModulation);
    result.roughness = saturate(safeRoughness
        + result.effectiveFoam * 0.55f);
    result.refractionWeight = clamp(
        1.0f - result.effectiveFoam * 0.92f, 0.08f, 1.0f);
    result.diffuseWeight = result.effectiveFoam;
    return result;
}

ApproximateRefractionResult EvaluateApproximateWaterRefraction(
    float2 uv,
    float2 screenNormal,
    float waterDepth,
    float opaqueDepth,
    float candidateOpaqueDepth,
    float distortionStrength,
    float maximumUvOffset,
    float thicknessOffset,
    float maximumThickness,
    float edgeMargin)
{
    ApproximateRefractionResult result;
    result.sampleUv = WaterSafeOriginalUv(uv);
    result.thickness = 0.0f;
    result.fallback = WaterRefractionNonFinite;
    const bool finite = WaterFinite2(uv) && WaterFinite2(screenNormal)
        && isfinite(waterDepth) && isfinite(opaqueDepth)
        && isfinite(candidateOpaqueDepth) && isfinite(distortionStrength)
        && isfinite(maximumUvOffset) && isfinite(thicknessOffset)
        && isfinite(maximumThickness) && isfinite(edgeMargin)
        && maximumThickness > 1.0e-6f;
    if (!finite)
        return result;

    const float rawThickness = opaqueDepth - waterDepth;
    result.thickness = clamp(rawThickness, 0.0f, maximumThickness);
    const float safeThicknessOffset = max(thicknessOffset, 0.0f);
    if (rawThickness <= safeThicknessOffset)
    {
        result.fallback = WaterRefractionZeroThickness;
        return result;
    }

    const float normalizedThickness = saturate(
        (rawThickness - safeThicknessOffset) / maximumThickness);
    float2 offset = screenNormal * distortionStrength
        * normalizedThickness;
    const float offsetLength = length(offset);
    const float boundedOffset = max(maximumUvOffset, 0.0f);
    if (offsetLength > boundedOffset && offsetLength > 1.0e-8f)
        offset *= boundedOffset / offsetLength;
    const float2 candidate = uv + offset;
    const float margin = clamp(edgeMargin, 0.0f, 0.499f);
    if (any(candidate < margin.xx)
        || any(candidate > (1.0f - margin).xx))
    {
        result.fallback = WaterRefractionOffScreen;
        return result;
    }
    if (candidateOpaqueDepth <= waterDepth + safeThicknessOffset)
    {
        result.fallback = WaterRefractionForeground;
        return result;
    }
    result.sampleUv = candidate;
    result.fallback = WaterRefractionValid;
    return result;
}

float WaterRefractionFrameNoise(uint2 pixel, uint frameIndex)
{
    uint hash = pixel.x * 0x8da6b343u;
    hash ^= pixel.y * 0xd8163841u;
    hash ^= frameIndex * 0xcb1ab31fu;
    hash ^= hash >> 16u;
    hash *= 0x7feb352du;
    hash ^= hash >> 15u;
    hash *= 0x846ca68bu;
    hash ^= hash >> 16u;
    return float(hash & 0x00ffffffu) / float(0x01000000u);
}

ApproximateRefractionResult EvaluateRayMarchedWaterRefraction(
    float2 uv,
    float2 screenDirection,
    float waterDepth,
    float opaqueDepthSamples[8],
    uint requestedSampleCount,
    float maximumUvOffset,
    float thicknessOffset,
    float maximumThickness,
    float stepScale,
    float jitter,
    float edgeMargin)
{
    ApproximateRefractionResult result;
    result.sampleUv = WaterSafeOriginalUv(uv);
    result.thickness = 0.0f;
    result.fallback = WaterRefractionNonFinite;
    const bool finite = WaterFinite2(uv)
        && WaterFinite2(screenDirection) && isfinite(waterDepth)
        && isfinite(maximumUvOffset) && isfinite(thicknessOffset)
        && isfinite(maximumThickness) && isfinite(stepScale)
        && isfinite(jitter) && isfinite(edgeMargin);
    if (!finite)
        return result;

    const uint sampleCount = clamp(requestedSampleCount, 1u, 8u);
    const float boundedUvOffset = clamp(maximumUvOffset, 0.0f, 0.5f);
    const float boundedThicknessOffset = clamp(
        thicknessOffset, 0.0f, 10.0f);
    const float boundedThickness = clamp(
        maximumThickness, 0.01f, 10000.0f);
    const float boundedStepScale = clamp(stepScale, 1.01f, 8.0f);
    const float boundedJitter = saturate(jitter);
    const float margin = clamp(edgeMargin, 0.0f, 0.499f);
    const float directionLength = length(screenDirection);
    const float2 direction = directionLength > 1.0e-8f
        ? screenDirection / directionLength : 0.0f.xx;
    bool rejectedForeground = false;
    [loop]
    for (uint index = 0u; index < sampleCount; ++index)
    {
        const float exponent = (float(index) + boundedJitter + 1.0f)
            / float(sampleCount);
        const float progress = saturate(
            (pow(boundedStepScale, exponent) - 1.0f)
                / (boundedStepScale - 1.0f));
        const float2 candidate = uv
            + direction * boundedUvOffset * progress;
        if (any(candidate < margin.xx)
            || any(candidate > (1.0f - margin).xx))
        {
            result.fallback = WaterRefractionOffScreen;
            return result;
        }
        const float sceneDepth = opaqueDepthSamples[index];
        if (!isfinite(sceneDepth))
            return result;
        if (sceneDepth <= waterDepth + boundedThicknessOffset)
        {
            rejectedForeground = true;
            continue;
        }
        const float rayDepth = waterDepth + boundedThicknessOffset
            + boundedThickness * progress;
        if (sceneDepth <= rayDepth + boundedThicknessOffset)
        {
            result.sampleUv = candidate;
            result.thickness = clamp(sceneDepth - waterDepth,
                0.0f, boundedThickness);
            result.fallback = WaterRefractionValid;
            return result;
        }
    }
    result.fallback = rejectedForeground
        ? WaterRefractionForeground : WaterRefractionNoIntersection;
    return result;
}

PRISM_VK_BINDING(21) Texture2D<float4> waterSceneColor : register(t5);
PRISM_VK_BINDING(22) Texture2D<float> waterOpaqueDepth : register(t6);
PRISM_VK_BINDING(23) Texture2D<float> waterCompositeDepth : register(t7);
PRISM_VK_BINDING(24) Texture2D<float4> waterMaterialNormal : register(t8);
PRISM_VK_BINDING(25) Texture2D<float4> waterMaterialMask : register(t9);
PRISM_VK_BINDING(34) RWTexture2D<float4> waterRefractionOutput
    : register(u2);
PRISM_VK_BINDING(50) SamplerState waterLinearClampSampler : register(s2);

PRISM_VK_BINDING(26) Texture2D<float4> waterCompositeRefraction
    : register(t10);
PRISM_VK_BINDING(27) Texture2D<float4> waterCompositeGBuffer1
    : register(t11);
PRISM_VK_BINDING(28) Texture2DArray<float4> waterCompositeSlopeMoments
    : register(t12);
PRISM_VK_BINDING(29) Texture2DArray waterCompositeShadowMap
    : register(t13);
PRISM_VK_BINDING(30) Texture2DArray<float4> waterCompositeShadowMoments
    : register(t14);
PRISM_VK_BINDING(31) Texture2D<float4> waterCompositeAtmosphere
    : register(t15);
PRISM_VK_BINDING(39) Texture2D<float4> waterCompositeCausticsNear
    : register(t23);
PRISM_VK_BINDING(40) Texture2D<float4> waterCompositeCausticsMiddle
    : register(t24);
PRISM_VK_BINDING(41) Texture2D<float4> waterCompositeVolumetric
    : register(t25);
PRISM_VK_BINDING(19) TextureCube<float4> waterCompositeEnvironment
    : register(t3);
PRISM_VK_BINDING(36) RWTexture2D<float4> waterCompositeOutput
    : register(u4);
PRISM_VK_BINDING(52) SamplerComparisonState waterCompositeShadowSampler
    : register(s4);

PRISM_VK_BINDING(37) RWTexture2D<float4> waterPublishHdr
    : register(u5);
PRISM_VK_BINDING(38) RWTexture2D<float2> waterPublishSceneMotion
    : register(u6);

float3 WaterRefractionStatusColor(uint fallback)
{
    if (fallback == WaterRefractionValid) return float3(0.0f, 1.0f, 0.0f);
    if (fallback == WaterRefractionOffScreen) return float3(1.0f, 0.4f, 0.0f);
    if (fallback == WaterRefractionForeground) return float3(1.0f, 0.0f, 0.0f);
    if (fallback == WaterRefractionZeroThickness) return float3(0.2f, 0.4f, 1.0f);
    if (fallback == WaterRefractionNoIntersection) return float3(0.7f, 0.0f, 1.0f);
    return float3(1.0f, 0.0f, 1.0f);
}

float3 EvaluateWaterDistanceTiers(float distanceMeters)
{
    const float nearEnd = max(waterDistanceRanges.x, 1.0f);
    const float middleEnd = max(waterDistanceRanges.y, nearEnd + 1.0f);
    const float farEnd = max(waterDistanceRanges.z, middleEnd + 1.0f);
    const float transition = clamp(waterDistanceRanges.w, 0.01f, 0.49f);
    const float nearHalfWidth = max(
        min(nearEnd, middleEnd - nearEnd) * transition, 1.0e-4f);
    const float farHalfWidth = max(
        min(middleEnd - nearEnd, farEnd - middleEnd) * transition,
        1.0e-4f);
    const float enteredMiddle = smoothstep(
        nearEnd - nearHalfWidth, nearEnd + nearHalfWidth,
        max(distanceMeters, 0.0f));
    const float enteredFar = smoothstep(
        middleEnd - farHalfWidth, middleEnd + farHalfWidth,
        max(distanceMeters, 0.0f));
    const float3 weights = max(float3(
        1.0f - enteredMiddle,
        enteredMiddle - enteredFar,
        enteredFar), 0.0f.xxx);
    return weights / max(dot(weights, 1.0f.xxx), 1.0e-6f);
}

float3 ReconstructWaterWorldPositionForTier(float2 uv, float deviceDepth)
{
    const float2 ndc = uv * float2(2.0f, -2.0f)
        + float2(-1.0f, 1.0f);
    const float4 worldHomogeneous = mul(
        float4(ndc, deviceDepth, 1.0f),
        waterInverseViewProjection);
    return worldHomogeneous.xyz
        / max(abs(worldHomogeneous.w), 1.0e-6f);
}

[numthreads(8, 8, 1)]
void WaterRefractionCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    waterRefractionOutput.GetDimensions(width, height);
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= width || pixel.y >= height)
        return;

    const float2 inverseExtent = rcp(float2(width, height));
    const float2 uv = (float2(pixel) + 0.5f) * inverseExtent;
    uint fullWidth, fullHeight;
    waterOpaqueDepth.GetDimensions(fullWidth, fullHeight);
    const int2 fullExtent = int2(fullWidth, fullHeight);
    const int2 fullPixel = min(int2(uv * float2(fullExtent)), fullExtent - 1);
    const float4 originalColor = waterSceneColor.SampleLevel(
        waterLinearClampSampler, uv, 0.0f);
    const float4 materialMask = waterMaterialMask.Load(int3(fullPixel, 0));
    if (materialMask.a <= 0.0f || waterModes1.x == 0u)
    {
        waterRefractionOutput[pixel] = float4(originalColor.rgb, 0.0f);
        return;
    }

    const float sourceOpaqueDevice = waterOpaqueDepth.Load(int3(fullPixel, 0));
    const float waterDevice = waterCompositeDepth.Load(int3(fullPixel, 0));
    const float3 waterWorldPosition =
        ReconstructWaterWorldPositionForTier(uv, waterDevice);
    const float3 distanceTiers = EvaluateWaterDistanceTiers(
        length(waterWorldPosition - cameraPosition));
    const float rayMarchTierWeight = saturate(
        distanceTiers.x + distanceTiers.y * 0.25f);
    const float refractionTierWeight = saturate(
        distanceTiers.x + distanceTiers.y * 0.65f
            + distanceTiers.z * 0.15f);
    const float maximumThickness = max(waterRefractionParameters0.w, 0.01f);
    // Device depth differences are not metres (perspective compresses the
    // entire distant ocean near 1). Optical extinction requires linear depth.
    const float waterLinearDepth = dot(waterWorldPosition - cameraPosition, cameraForward);
    const float sourceOpaqueDepth = max(dot(
        ReconstructWaterWorldPositionForTier(uv, sourceOpaqueDevice)
            - waterWorldPosition, cameraForward), 0.0f);
    const float3 normal = waterMaterialNormal.Load(int3(fullPixel, 0)).xyz * 2.0f - 1.0f;
    const float3 right = normalize(ReconstructWaterWorldPositionForTier(
        uv + float2(inverseExtent.x, 0.0f), waterDevice) - waterWorldPosition);
    const float3 down = normalize(ReconstructWaterWorldPositionForTier(
        uv + float2(0.0f, inverseExtent.y), waterDevice) - waterWorldPosition);
    const float2 screenNormal = float2(dot(normal, right), dot(normal, down));
    const float thicknessOffset = waterRefractionParameters0.z;
    const float maximumUvOffset = waterRefractionParameters0.y
        * refractionTierWeight;
    const float edgeMargin = max(inverseExtent.x, inverseExtent.y);
    ApproximateRefractionResult result;
    const bool rayMarch = waterModes0.w != 0u
        && rayMarchTierWeight > 0.01f;
    if (rayMarch)
    {
        float depthSamples[8];
        const uint sampleCount = clamp(uint(ceil(
            float(waterModes0.z) * rayMarchTierWeight)), 1u, 8u);
        const float jitter = WaterRefractionFrameNoise(pixel, waterModes2.w);
        const float directionLength = length(screenNormal);
        const float2 direction = directionLength > 1.0e-8f
            ? screenNormal / directionLength : 0.0f.xx;
        [unroll]
        for (uint index = 0u; index < 8u; ++index)
        {
            const float exponent = (float(index) + jitter + 1.0f)
                / float(sampleCount);
            const float progress = saturate(
                (pow(max(waterRefractionParameters1.x, 1.01f), exponent)
                    - 1.0f)
                / max(waterRefractionParameters1.x - 1.0f, 0.01f));
            const float2 sampleUv = uv
                + direction * maximumUvOffset * progress;
            const int2 samplePixel = clamp(int2(sampleUv * float2(fullExtent)), int2(0, 0), fullExtent - 1);
            const float deviceDepth = waterOpaqueDepth.Load(int3(samplePixel, 0));
            depthSamples[index] = dot(ReconstructWaterWorldPositionForTier(
                sampleUv, deviceDepth) - cameraPosition, cameraForward) - waterLinearDepth;
        }
        result = EvaluateRayMarchedWaterRefraction(
            uv, screenNormal, 0.0f, depthSamples, sampleCount,
            maximumUvOffset, thicknessOffset, maximumThickness,
            waterRefractionParameters1.x, jitter, edgeMargin);
    }
    else
    {
        const ApproximateRefractionResult preliminary =
            EvaluateApproximateWaterRefraction(
                uv, screenNormal, 0.0f, sourceOpaqueDepth,
                sourceOpaqueDepth, waterRefractionParameters0.x
                    * refractionTierWeight,
                maximumUvOffset, thicknessOffset, maximumThickness,
                edgeMargin);
        const int2 candidatePixel = clamp(int2(preliminary.sampleUv * float2(fullExtent)), int2(0, 0), fullExtent - 1);
        const float candidateDeviceDepth = waterOpaqueDepth.Load(int3(candidatePixel, 0));
        const float candidateDepth = dot(ReconstructWaterWorldPositionForTier(
            preliminary.sampleUv, candidateDeviceDepth) - cameraPosition, cameraForward) - waterLinearDepth;
        result = EvaluateApproximateWaterRefraction(
            uv, screenNormal, 0.0f, sourceOpaqueDepth,
            candidateDepth, waterRefractionParameters0.x
                * refractionTierWeight,
            maximumUvOffset, thicknessOffset, maximumThickness,
            edgeMargin);
    }

    const bool validHit = result.fallback == WaterRefractionValid;
    const float4 refracted = validHit
        ? waterSceneColor.SampleLevel(
            waterLinearClampSampler, result.sampleUv, 0.0f)
        : originalColor;
    const float normalizedThickness = saturate(
        (validHit ? result.thickness : sourceOpaqueDepth) / maximumThickness);
    const float3 safeColor = all(isfinite(refracted.rgb)) ? refracted.rgb : originalColor.rgb;
    // Sign encodes hit validity; abs(alpha) always retains optical thickness.
    // Debug selection is applied only in composite and never changes this data.
    waterRefractionOutput[pixel] = float4(safeColor,
        validHit ? normalizedThickness : -normalizedThickness);
}

float3 ReconstructWaterWorldPosition(float2 uv, float deviceDepth)
{
    const float2 ndc = uv * float2(2.0f, -2.0f)
        + float2(-1.0f, 1.0f);
    const float4 worldHomogeneous = mul(
        float4(ndc, deviceDepth, 1.0f),
        waterInverseViewProjection);
    return worldHomogeneous.xyz
        / max(abs(worldHomogeneous.w), 1.0e-6f);
}

float2 WaterAtmosphereUv(float3 direction, float3 sunDirection)
{
    const float zenith = acos(clamp(direction.y, -1.0f, 1.0f));
    const float2 horizontal = direction.xz;
    const float2 sunHorizontal = sunDirection.xz;
    float relativeAzimuth = 0.0f;
    if (dot(horizontal, horizontal) > 1.0e-6f
        && dot(sunHorizontal, sunHorizontal) > 1.0e-6f)
    {
        relativeAzimuth = acos(clamp(dot(
            normalize(horizontal), normalize(sunHorizontal)),
            -1.0f, 1.0f));
    }
    return float2(relativeAzimuth / 3.14159265358979323846f,
        zenith / 3.14159265358979323846f);
}

float WaterSlopeVariance(float3 worldPosition)
{
    float accumulatedVariance = 0.0f;
    [unroll]
    for (uint cascade = 0u; cascade < 4u; ++cascade)
    {
        const float2 uv = frac(worldPosition.xz
            * oceanCascadeUvScales[cascade]
            + float2(oceanCascadeUvOffsetX[cascade],
                oceanCascadeUvOffsetY[cascade]));
        const float4 moments = waterCompositeSlopeMoments.SampleLevel(
            waterLinearClampSampler,
            float3(uv, oceanCascadeLayerOrder[cascade]),
            0.0f);
        accumulatedVariance += max(
            moments.z - moments.x * moments.x
                + moments.w - moments.y * moments.y,
            0.0f);
    }
    return clamp(accumulatedVariance, 0.0f, 4.0f);
}

float WaterDirectionalShadow(float3 worldPosition)
{
    if (shadowsEnabled < 0.5f)
        return 1.0f;
    const float viewDepth = GetDirectionalShadowViewDepth(
        worldPosition, cameraPosition, cameraForward);
    if (viewDepth > cascadeSplits.z)
        return 1.0f;
    const uint cascade = SelectDirectionalShadowCascade(
        viewDepth, cascadeSplits);
    return SampleDirectionalShadowCascade(
        waterCompositeShadowMap,
        waterCompositeShadowSampler,
        waterCompositeShadowMoments,
        waterLinearClampSampler,
        worldPosition,
        lightViewProjections[cascade],
        cascade,
        shadowBias);
}

float WaterCausticCascadeEdge(float2 uv)
{
    const float inside = 0.5f
        - max(abs(uv.x - 0.5f), abs(uv.y - 0.5f));
    return smoothstep(0.0f,
        clamp(waterVolumetricParameters1.y, 0.01f, 0.49f) * 0.5f,
        inside);
}

float3 EvaluateWaterCaustics(float3 surfaceWorldPosition,
    float3 receiverWorldPosition, out float2 cascadeWeights)
{
    cascadeWeights = 0.0f.xx;
    if (waterModes1.y == 0u || waterModes1.z == 0u)
        return 0.0f.xxx;
    const float receiverDepth = surfaceWorldPosition.y
        - receiverWorldPosition.y;
    if (!isfinite(receiverDepth) || receiverDepth <= 0.0f)
        return 0.0f.xxx;
    const float resolution = max(float(waterModes1.w), 1.0f);
    const float2 coverages = max(waterCausticsParameters.xy, 1.0f.xx);
    const float2 texelSizes = coverages / resolution;
    const float2 nearCenter = floor(cameraPosition.xz
        / texelSizes.x + 0.5f) * texelSizes.x;
    const float2 middleCenter = floor(cameraPosition.xz
        / texelSizes.y + 0.5f) * texelSizes.y;
    const float2 nearUv = (receiverWorldPosition.xz - nearCenter)
        / coverages.x + 0.5f.xx;
    const float2 middleUv = (receiverWorldPosition.xz - middleCenter)
        / coverages.y + 0.5f.xx;
    const float nearWeight = WaterCausticCascadeEdge(nearUv);
    const float middleWeight = (1.0f - nearWeight)
        * WaterCausticCascadeEdge(middleUv);
    cascadeWeights = saturate(float2(nearWeight, middleWeight));
    if (dot(cascadeWeights, 1.0f.xx) <= 0.0f)
        return 0.0f.xxx;
    const float3 nearEnergy = waterCompositeCausticsNear.SampleLevel(
        waterLinearClampSampler, saturate(nearUv), 0.0f).rgb;
    const float3 middleEnergy = waterCompositeCausticsMiddle.SampleLevel(
        waterLinearClampSampler, saturate(middleUv), 0.0f).rgb;
    const float3 extinction = max(waterAbsorptionRoughness.xyz
        + waterScatteringIor.xyz, 0.0f.xxx);
    const float3 attenuation = exp(-min(
        extinction * min(receiverDepth, 1000.0f), 80.0f.xxx));
    return clamp((nearEnergy * cascadeWeights.x
        + middleEnergy * cascadeWeights.y) * attenuation,
        0.0f.xxx, 16.0f.xxx);
}

float3 WaterSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return isfinite(lengthSquared) && lengthSquared > 1.0e-8f
        ? value * rsqrt(lengthSquared) : fallback;
}

float3 WaterSunSpecular(
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float roughness,
    float fresnel)
{
    const float nDotL = saturate(dot(normal, lightDirection));
    const float nDotV = saturate(dot(normal, viewDirection));
    if (nDotL <= 0.0f || nDotV <= 0.0f)
        return 0.0f.xxx;
    const float3 halfVector = WaterSafeNormalize(
        lightDirection + viewDirection, normal);
    const float nDotH = saturate(dot(normal, halfVector));
    const float alpha = max(roughness * roughness, 0.0025f);
    const float alphaSquared = alpha * alpha;
    const float denominator = max(
        nDotH * nDotH * (alphaSquared - 1.0f) + 1.0f,
        1.0e-4f);
    const float distribution = alphaSquared
        / (3.14159265358979323846f * denominator * denominator);
    const float geometry = min(1.0f,
        min(2.0f * nDotH * nDotV,
            2.0f * nDotH * nDotL)
        / max(dot(viewDirection, halfVector), 1.0e-4f));
    return (distribution * geometry * fresnel
        / max(4.0f * nDotV * nDotL, 1.0e-4f)) * nDotL;
}

[numthreads(8, 8, 1)]
void WaterCompositeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    waterCompositeOutput.GetDimensions(width, height);
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= width || pixel.y >= height)
        return;

    const float4 opaque = waterSceneColor.Load(int3(pixel, 0));
    const float4 material2 = waterMaterialMask.Load(int3(pixel, 0));
    const float2 volumeUv = (float2(pixel) + 0.5f)
        / float2(width, height);
    const float4 volumetric = waterModes2.x != 0u
        ? waterCompositeVolumetric.SampleLevel(
            waterLinearClampSampler, volumeUv, 0.0f)
        : 0.0f.xxxx;
    const float3 cameraTransmittance = exp(-min(
        max(waterAbsorptionRoughness.xyz + waterScatteringIor.xyz, 0.0f.xxx)
            * abs(volumetric.a) * max(waterVolumetricParameters0.y, 0.1f),
        40.0f.xxx));
    if (waterModes0.y == 12u)
    {
        waterCompositeOutput[pixel] = float4(
            min(max(volumetric.rgb, 0.0f.xxx) * 8.0f, 1.0f.xxx), 1.0f);
        return;
    }
    if (waterModes0.y == 13u)
    {
        const bool accepted = volumetric.a > 0.0f;
        waterCompositeOutput[pixel] = float4(
            accepted ? float3(0.0f, 1.0f, saturate(volumetric.a))
                     : float3(1.0f, 0.0f, 0.0f),
            1.0f);
        return;
    }
    if (waterModes0.y == 1u)
    {
        waterCompositeOutput[pixel] = float4((material2.a > 0.0f ? 1.0f : 0.0f).xxx, 1.0f);
        return;
    }
    if (material2.a <= 0.0f)
    {
        const float receiverDepth = waterOpaqueDepth.Load(int3(pixel, 0));
        const float3 receiver = ReconstructWaterWorldPosition(volumeUv, receiverDepth);
        float2 receiverCascades;
        const float3 receiverCaustics = receiverDepth < 1.0f && receiver.y < 0.0f
            ? EvaluateWaterCaustics(float3(receiver.x, 0.0f, receiver.z),
                receiver, receiverCascades) * WaterDirectionalShadow(receiver)
                * max(directionalLightColor, 0.0f.xxx) * 0.12f
            : 0.0f.xxx;
        if (waterModes0.y == 10u)
        {
            waterCompositeOutput[pixel] = float4(saturate(receiverCaustics * 8.0f), 1.0f);
            return;
        }
        waterCompositeOutput[pixel] = float4(
            (max(opaque.rgb, 0.0f.xxx) + receiverCaustics) * cameraTransmittance
                + max(volumetric.rgb, 0.0f.xxx),
            1.0f);
        return;
    }

    const float2 uv = (float2(pixel) + 0.5f)
        / float2(width, height);
    const float waterDeviceDepth =
        waterCompositeDepth.Load(int3(pixel, 0));
    const float3 worldPosition = ReconstructWaterWorldPosition(
        uv, waterDeviceDepth);
    const float3 distanceTiers = EvaluateWaterDistanceTiers(
        length(worldPosition - cameraPosition));
    if (waterModes0.y == 9u)
    {
        waterCompositeOutput[pixel] = float4(distanceTiers, 1.0f);
        return;
    }
    const float4 material0 = waterMaterialNormal.Load(int3(pixel, 0));
    const float4 material1 = waterCompositeGBuffer1.Load(int3(pixel, 0));
    if (waterModes0.y >= 2u && waterModes0.y <= 5u)
    {
        const float3 debugColor = waterModes0.y == 2u
            ? saturate(length(worldPosition - cameraPosition) / 200.0f).xxx
            : waterModes0.y == 3u ? (uv.x < 0.75f ? material0.xyz : material0.aaa)
            : waterModes0.y == 4u ? saturate(material1.rgb) : saturate(material2.rgb);
        waterCompositeOutput[pixel] = float4(debugColor, 1.0f);
        return;
    }
    float3 normal = material0.xyz * 2.0f - 1.0f;
    const float normalLengthSquared = dot(normal, normal);
    normal = normalLengthSquared > 1.0e-6f
        ? normal * rsqrt(normalLengthSquared)
        : float3(0.0f, 1.0f, 0.0f);
    const float3 viewDirection = WaterSafeNormalize(
        cameraPosition - worldPosition, float3(0.0f, 0.0f, 1.0f));
    const bool insideWater = waterVolumetricParameters1.z > 0.5f;
    if (insideWater && dot(normal, viewDirection) < 0.0f)
        normal = -normal;
    const float3 lightDirection = WaterSafeNormalize(
        -directionalLightDirection, float3(0.0f, 1.0f, 0.0f));
    const float viewCosine = saturate(dot(normal, viewDirection));
    const float lightCosine = clamp(dot(normal, lightDirection), -1.0f, 1.0f);
    const float4 refraction = waterModes1.x != 0u
        ? waterCompositeRefraction.SampleLevel(
            waterLinearClampSampler, uv, 0.0f)
        : opaque;
    const float maximumThickness = max(
        waterRefractionParameters0.w, 0.01f);
    const float opaqueDeviceDepth = waterOpaqueDepth.Load(int3(pixel, 0));
    const float3 opaqueWorldPosition = ReconstructWaterWorldPosition(
        uv, opaqueDeviceDepth);
    const float fallbackThickness = min(max(dot(
        opaqueWorldPosition - worldPosition, cameraForward), 0.0f), maximumThickness);
    // From below, the camera segment already accounts for water extinction;
    // the transmitted segment beyond this surface is air, not another 80 m of water.
    const float thickness = insideWater ? 0.0f : (waterModes1.x != 0u
        ? saturate(abs(refraction.a)) * maximumThickness : fallbackThickness);
    if (waterModes0.y == 7u || waterModes0.y == 8u)
    {
        waterCompositeOutput[pixel] = float4(waterModes0.y == 7u
            ? saturate(thickness / maximumThickness).xxx
            : (refraction.a > 0.0f && waterModes1.x != 0u
                ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f)), 1.0f);
        return;
    }
    WaterOpticalResponse response = EvaluateWaterOpticalResponse(
        material1.rgb,
        material2.rgb,
        waterScatteringIor.w,
        waterPhaseThinBacklit.x,
        waterPhaseThinBacklit.y,
        waterPhaseThinBacklit.z,
        viewCosine,
        lightCosine,
        thickness);
    if (insideWater)
    {
        const float eta = max(waterScatteringIor.w, 1.0001f);
        const float transmittedSineSquared = eta * eta * (1.0f - viewCosine * viewCosine);
        if (transmittedSineSquared >= 1.0f)
            response.fresnel = 1.0f;
        else
        {
            const float transmittedCosine = sqrt(max(1.0f - transmittedSineSquared, 0.0f));
            const float rs = (eta * viewCosine - transmittedCosine)
                / max(eta * viewCosine + transmittedCosine, 1.0e-5f);
            const float rp = (viewCosine - eta * transmittedCosine)
                / max(viewCosine + eta * transmittedCosine, 1.0e-5f);
            response.fresnel = saturate(0.5f * (rs * rs + rp * rp));
        }
        response.thinLayer *= 1.0f - response.fresnel;
    }

    const float slopeVariance = WaterSlopeVariance(worldPosition);
    const float baseRoughness = clamp(material0.a
        + sqrt(slopeVariance) * 0.18f, 0.001f, 1.0f);
    const float localOpticsWeight = saturate(
        distanceTiers.x + distanceTiers.y);
    const float foamDetail = material1.a > 1.0e-5f
        ? lerp(0.5f, WaterFoamWorldDetail(worldPosition),
            localOpticsWeight)
        : 0.5f;
    const WaterFoamResponse foam = EvaluateWaterFoamResponse(
        material1.a,
        foamDetail,
        baseRoughness);
    const float roughness = foam.roughness;
    if (waterModes0.y == 6u)
    {
        waterCompositeOutput[pixel] = float4(
            foam.effectiveFoam.xxx, 1.0f);
        return;
    }
    const float3 reflectedDirection = reflect(-viewDirection, normal);
    const float3 broadenedReflection = WaterSafeNormalize(lerp(
        reflectedDirection, normal, roughness * roughness * 0.45f),
        normal);
    const float3 environment = waterCompositeEnvironment.SampleLevel(
        waterLinearClampSampler, broadenedReflection, 0.0f).rgb;
    const float3 atmosphere = waterCompositeAtmosphere.SampleLevel(
        waterLinearClampSampler,
        WaterAtmosphereUv(broadenedReflection, lightDirection),
        0.0f).rgb * max(atmosphereBrightness, 0.0f);
    const float3 reflectedEnvironment = insideWater
        ? max(oceanDeepWaterColor, 0.0f.xxx)
        : lerp(environment, atmosphere, saturate(physicalAtmosphereEnabled));
    const float shadow = WaterDirectionalShadow(worldPosition);
    const float3 directSpecular = WaterSunSpecular(
        normal, viewDirection, lightDirection, roughness,
        response.fresnel)
        * max(directionalLightColor, 0.0f.xxx)
        * max(directionalLightIntensity, 0.0f)
        * shadow;
    const float3 deepWater = max(oceanDeepWaterColor, 0.0f.xxx);
    const float3 deepScattering = max(oceanScatteringColor, 0.0f.xxx);
    const float refractionTierWeight = saturate(
        distanceTiers.x + distanceTiers.y * 0.65f
            + distanceTiers.z * 0.15f);
    const float3 tierRefraction = lerp(
        max(opaque.rgb, 0.0f.xxx), max(refraction.rgb, 0.0f.xxx),
        refractionTierWeight);
    const float3 transmitted = tierRefraction
        * response.transmittance * (1.0f - response.fresnel)
        * foam.refractionWeight;
    const float3 volume = (deepWater
            * (1.0f.xxx - response.transmittance)
        + deepScattering * response.singleScattering
            * max(directionalLightIntensity, 0.0f) * shadow)
        * (1.0f - response.fresnel);
    const float3 sideTransmission =
        (response.thinLayer + response.backlit)
        * max(directionalLightColor, 0.0f.xxx)
        * max(directionalLightIntensity, 0.0f) * shadow;
    const float3 reflected = reflectedEnvironment
        * max(iblIntensity * iblSpecularStrength, 0.0f)
        * response.fresnel + directSpecular;
    const float foamLight = max(ambientIntensity, 0.0f)
        + saturate(lightCosine) * max(directionalLightIntensity, 0.0f)
            * shadow;
    const float3 aeratedDiffuse = max(oceanFoamColor, 0.0f.xxx)
        * foam.diffuseWeight * min(foamLight, 4.0f);
    if (waterModes0.y == 11u)
    {
        float2 debugCascadeWeights = 0.0f.xx;
        EvaluateWaterCaustics(worldPosition,
            worldPosition - float3(0.0f, 1.0f, 0.0f),
            debugCascadeWeights);
        waterCompositeOutput[pixel] = float4(
            debugCascadeWeights.x,
            debugCascadeWeights.y,
            1.0f - saturate(dot(debugCascadeWeights, 1.0f.xx)),
            1.0f);
        return;
    }
    float2 causticCascadeWeights = 0.0f.xx;
    const float3 causticEnergy = EvaluateWaterCaustics(
        worldPosition, opaqueWorldPosition, causticCascadeWeights)
        * max(directionalLightColor, 0.0f.xxx) * shadow * 0.12f;
    if (waterModes0.y == 10u)
    {
        waterCompositeOutput[pixel] = float4(
            min(causticEnergy * 8.0f, 1.0f.xxx), 1.0f);
        return;
    }
    float3 color = (transmitted + volume + sideTransmission + reflected
        + aeratedDiffuse + causticEnergy) * cameraTransmittance
        + max(volumetric.rgb, 0.0f.xxx);
    if (!all(isfinite(color)))
        color = opaque.rgb;
    waterCompositeOutput[pixel] = float4(
        clamp(color, 0.0f.xxx, 65504.0f.xxx), 1.0f);
}

[numthreads(8, 8, 1)]
void WaterCompositePublishCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    waterPublishHdr.GetDimensions(width, height);
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= width || pixel.y >= height)
        return;

    // Reuse the existing t5/t8 declarations. The renderer's canonical
    // descriptor ranges reserve t0..t15; t16 aliases u0 on D3D12.
    const float4 composed = waterSceneColor.Load(int3(pixel, 0));
    waterPublishHdr[pixel] = all(isfinite(composed))
        ? composed : 0.0f.xxxx;
    // Preserve opaque motion and replace it only where dedicated water
    // visibility produced a valid displaced-surface sample.
    if (waterMaterialMask.Load(int3(pixel, 0)).a > 0.0f)
    {
        const float2 motion =
            waterMaterialNormal.Load(int3(pixel, 0)).xy;
        waterPublishSceneMotion[pixel] = WaterFinite2(motion)
            ? clamp(motion, -1.0f.xx, 1.0f.xx) : 0.0f.xx;
    }
}

struct RefractionFixtureInput
{
    float4 uvNormal;
    float4 depthsParameters;
    float4 boundsParameters;
    float4 fallbackColor;
    float4 refractedColor;
};

struct RefractionFixtureOutput
{
    float4 uvThicknessFallback;
    float4 resolvedColor;
};

PRISM_VK_BINDING(16) StructuredBuffer<RefractionFixtureInput>
    refractionFixtureInputs : register(t0);
PRISM_VK_BINDING(32) RWStructuredBuffer<RefractionFixtureOutput>
    refractionFixtureOutputs : register(u0);

[numthreads(8, 1, 1)]
void WaterRefractionAnalyticFixtureCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if (index >= 5u)
        return;
    const RefractionFixtureInput input = refractionFixtureInputs[index];
    const ApproximateRefractionResult result =
        EvaluateApproximateWaterRefraction(
            input.uvNormal.xy,
            input.uvNormal.zw,
            input.depthsParameters.x,
            input.depthsParameters.y,
            input.depthsParameters.z,
            input.depthsParameters.w,
            input.boundsParameters.x,
            input.boundsParameters.y,
            input.boundsParameters.z,
            input.boundsParameters.w);
    const bool validColor = all(isfinite(input.refractedColor));
    const bool useRefraction = result.fallback == WaterRefractionValid
        && validColor;
    float4 safeFallback = all(isfinite(input.fallbackColor))
        ? input.fallbackColor : 0.0f.xxxx;
    RefractionFixtureOutput output;
    output.uvThicknessFallback = float4(
        result.sampleUv, result.thickness, float(result.fallback));
    output.resolvedColor = useRefraction
        ? input.refractedColor : safeFallback;
    refractionFixtureOutputs[index] = output;
}

struct RayMarchFixtureInput
{
    float4 uvDirection;
    float4 depthParameters;
    float4 marchParameters;
    float4 depthSamples0;
    float4 depthSamples1;
    float4 fallbackColor;
    float4 refractedColor;
};

PRISM_VK_BINDING(17) StructuredBuffer<RayMarchFixtureInput>
    rayMarchFixtureInputs : register(t1);
PRISM_VK_BINDING(33) RWStructuredBuffer<RefractionFixtureOutput>
    rayMarchFixtureOutputs : register(u1);

[numthreads(8, 1, 1)]
void WaterRefractionRayMarchFixtureCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if (index >= 3u)
        return;
    const RayMarchFixtureInput input = rayMarchFixtureInputs[index];
    float samples[8] = {
        input.depthSamples0.x, input.depthSamples0.y,
        input.depthSamples0.z, input.depthSamples0.w,
        input.depthSamples1.x, input.depthSamples1.y,
        input.depthSamples1.z, input.depthSamples1.w};
    const ApproximateRefractionResult result =
        EvaluateRayMarchedWaterRefraction(
            input.uvDirection.xy,
            input.uvDirection.zw,
            input.depthParameters.x,
            samples,
            uint(input.depthParameters.y),
            input.depthParameters.z,
            input.depthParameters.w,
            input.marchParameters.x,
            input.marchParameters.y,
            input.marchParameters.z,
            input.marchParameters.w);
    RefractionFixtureOutput output;
    output.uvThicknessFallback = float4(
        result.sampleUv, result.thickness, float(result.fallback));
    const bool validColor = all(isfinite(input.refractedColor));
    const float4 safeFallback = all(isfinite(input.fallbackColor))
        ? input.fallbackColor : 0.0f.xxxx;
    output.resolvedColor = result.fallback == WaterRefractionValid
        && validColor ? input.refractedColor : safeFallback;
    rayMarchFixtureOutputs[index] = output;
}

struct WaterOpticalFixtureInput
{
    float4 absorptionIor;
    float4 scatteringPhase;
    float4 responseParameters;
    float4 strengths;
};

struct WaterOpticalFixtureOutput
{
    float4 transmittanceFresnel;
    float4 scatteringPhase;
    float4 thinLayer;
    float4 backlit;
};

PRISM_VK_BINDING(18) StructuredBuffer<WaterOpticalFixtureInput>
    waterOpticalFixtureInputs : register(t2);
PRISM_VK_BINDING(35) RWStructuredBuffer<WaterOpticalFixtureOutput>
    waterOpticalFixtureOutputs : register(u3);

[numthreads(4, 1, 1)]
void WaterOpticalAnalyticFixtureCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if (index >= 4u)
        return;
    const WaterOpticalFixtureInput input = waterOpticalFixtureInputs[index];
    const WaterOpticalResponse response = EvaluateWaterOpticalResponse(
        input.absorptionIor.xyz,
        input.scatteringPhase.xyz,
        input.absorptionIor.w,
        input.scatteringPhase.w,
        input.strengths.x,
        input.strengths.y,
        input.responseParameters.x,
        input.responseParameters.y,
        input.responseParameters.z);
    WaterOpticalFixtureOutput output;
    output.transmittanceFresnel = float4(
        response.transmittance, response.fresnel);
    output.scatteringPhase = float4(
        response.singleScattering, response.phase);
    output.thinLayer = float4(response.thinLayer, 0.0f);
    output.backlit = float4(response.backlit, 0.0f);
    waterOpticalFixtureOutputs[index] = output;
}
