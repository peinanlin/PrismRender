#include "../ShaderBindings.hlsli"

struct CausticPointLight
{
    float3 position;
    float range;
    float3 color;
    float intensity;
};

struct CausticDirectionalLight
{
    float3 direction;
    float intensity;
    float3 color;
    float padding;
};

// Exact SharedFrameConstants layout. The caustic producer consumes the same
// primary-light and spectral-address metadata as the water visibility pass.
PRISM_VK_BINDING(0) cbuffer CausticFrameConstants : register(b0)
{
    float3 causticCameraPosition;
    float causticPointLightCount;
    float3 causticPrimaryLightDirection;
    float causticPrimaryLightIntensity;
    float3 causticPrimaryLightColor;
    float causticAmbientIntensity;
    CausticPointLight causticPointLights[4];
    float4x4 causticLightViewProjections[3];
    float4 causticShadowSplits;
    float3 causticSkyZenithColor;
    float causticPbrEnabled;
    float3 causticSkyHorizonColor;
    float causticIblEnabled;
    float3 causticGroundColor;
    float causticIblIntensity;
    float causticIblDiffuseStrength;
    float causticIblSpecularStrength;
    float causticIblReflectionBlend;
    float causticIblHorizonSharpness;
    float causticIblSplitSumEnabled;
    float causticShadowBias;
    float causticShadowsEnabled;
    float causticGtaoEnabled;
    float causticDirectLightingEnabled;
    float causticPointLightsEnabled;
    float causticNormalMappingEnabled;
    float causticOcclusionEnabled;
    float causticEmissiveEnabled;
    float causticAlphaMaskEnabled;
    float causticClusteredLightingEnabled;
    float causticShadowFilterMode;
    float3 causticCameraForward;
    float causticCascadeBlendFraction;
    float3 causticDeepWaterColor;
    float causticPhysicalAtmosphereEnabled;
    float3 causticScatteringColor;
    float causticAtmosphereBrightness;
    float3 causticFoamColor;
    float causticScatteringStrength;
    CausticDirectionalLight causticAuxiliaryLights[2];
    float4 causticCascadePatchLengths;
    float4 causticCascadeLowerWavelengths;
    float4 causticCascadeUpperWavelengths;
    float4 causticCascadeUvScales;
    float4 causticCascadeUvOffsetX;
    float4 causticCascadeUvOffsetY;
    float4 causticCascadeFadeStarts;
    float4 causticCascadeFadeEnds;
    float4 causticCascadeLayerOrder;
};

// Matches WaterOpticsGpuConstants. The local-domain mapping is packed in
// waterCausticRefractionParameters1.yzw by the feature.
PRISM_VK_BINDING(5) cbuffer CausticWaterConstants : register(b5)
{
    float4 waterCausticAbsorptionRoughness;
    float4 waterCausticScatteringIor;
    float4 waterCausticPhaseThinBacklit;
    float4 waterCausticDistanceRanges;
    float4 waterCausticRefractionParameters0;
    float4 waterCausticRefractionParameters1;
    float4 waterCausticParameters;
    float4 waterCausticVolumetricParameters0;
    float4 waterCausticVolumetricParameters1;
    uint4 waterCausticModes0;
    uint4 waterCausticModes1;
    uint4 waterCausticModes2;
    float4x4 waterCausticInverseViewProjection;
};

PRISM_VK_BINDING(16) Texture2DArray<float4> causticSpectralGradient
    : register(t0);
PRISM_VK_BINDING(17) Texture2D<float4> causticLocalGradient
    : register(t1);
PRISM_VK_BINDING(32) RWTexture2D<float4> causticNearOutput : register(u0);
PRISM_VK_BINDING(33) RWTexture2D<float4> causticMiddleOutput : register(u1);
PRISM_VK_BINDING(48) SamplerState causticLinearWrapSampler : register(s0);
PRISM_VK_BINDING(49) SamplerState causticLinearClampSampler : register(s1);

float CausticFiniteOr(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float CausticEdgeWeight(float2 uv, float edgeFadeFraction)
{
    const float distanceInside = 0.5f
        - max(abs(uv.x - 0.5f), abs(uv.y - 0.5f));
    return smoothstep(0.0f,
        clamp(edgeFadeFraction, 0.01f, 0.49f) * 0.5f,
        distanceInside);
}

float EvaluateBoundedCausticEnergy(
    float3 surfaceNormal,
    float3 lightDirection,
    float receiverDepthMeters,
    float extinction,
    float coverageWeight,
    float intensity)
{
    if (!all(isfinite(surfaceNormal)) || !all(isfinite(lightDirection))
        || !isfinite(receiverDepthMeters) || receiverDepthMeters <= 0.0f
        || !isfinite(coverageWeight) || coverageWeight <= 0.0f)
    {
        return 0.0f;
    }
    const float normalLengthSquared = dot(surfaceNormal, surfaceNormal);
    const float lightLengthSquared = dot(lightDirection, lightDirection);
    if (normalLengthSquared <= 1.0e-8f || lightLengthSquared <= 1.0e-8f)
        return 0.0f;
    const float3 normal = surfaceNormal * rsqrt(normalLengthSquared);
    const float3 towardLight = -lightDirection * rsqrt(lightLengthSquared);
    const float incidence = saturate(dot(normal, towardLight));
    const float focusing = incidence * incidence
        * (0.35f + 0.65f * incidence * incidence);
    const float attenuation = exp(-min(
        max(CausticFiniteOr(extinction, 0.0f), 0.0f)
            * min(receiverDepthMeters, 1000.0f), 80.0f));
    return clamp(focusing * attenuation
        * saturate(coverageWeight)
        * max(CausticFiniteOr(intensity, 0.0f), 0.0f),
        0.0f, 16.0f);
}

float2 SampleCombinedCausticSlope(float2 worldPosition)
{
    float2 combinedSlope = 0.0f.xx;
    [unroll]
    for (uint cascade = 0u; cascade < 4u; ++cascade)
    {
        const float2 uv = worldPosition * causticCascadeUvScales[cascade]
            + float2(causticCascadeUvOffsetX[cascade],
                causticCascadeUvOffsetY[cascade]);
        const uint layer = min(uint(round(
            causticCascadeLayerOrder[cascade])), 3u);
        const float3 normal = causticSpectralGradient.SampleLevel(
            causticLinearWrapSampler, float3(uv, float(layer)), 0.0f).xyz;
        combinedSlope += -normal.xz / max(normal.y, 0.05f);
    }

    const float2 localCenter = waterCausticRefractionParameters1.yz;
    const float localDomainSize = max(
        waterCausticRefractionParameters1.w, 1.0f);
    const float2 localUv = (worldPosition - localCenter)
        / localDomainSize + 0.5f.xx;
    if (waterCausticPhaseThinBacklit.w > 0.5f
        && all(localUv >= 0.0f.xx) && all(localUv <= 1.0f.xx))
    {
        const float edge = CausticEdgeWeight(localUv, 0.12f);
        const float3 localNormal = causticLocalGradient.SampleLevel(
            causticLinearClampSampler, localUv, 0.0f).xyz;
        combinedSlope += edge * -localNormal.xz / max(localNormal.y, 0.05f);
    }
    return clamp(combinedSlope, -8.0f.xx, 8.0f.xx);
}

[numthreads(8, 8, 1)]
void WaterCausticAccumulateCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    causticNearOutput.GetDimensions(width, height);
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= width || pixel.y >= height)
        return;

    const float2 uv = (float2(pixel) + 0.5f) / float2(width, height);
    const float edgeFade = waterCausticVolumetricParameters1.y;
    const bool enabled = waterCausticModes1.y != 0u
        && waterCausticModes1.z != 0u;
    const float extinction = dot(
        max(waterCausticAbsorptionRoughness.xyz
            + waterCausticScatteringIor.xyz, 0.0f.xxx),
        float3(0.2126f, 0.7152f, 0.0722f));
    const float3 lightDirection = causticPrimaryLightDirection;
    [unroll]
    for (uint cascade = 0u; cascade < 2u; ++cascade)
    {
        const float coverage = cascade == 0u
            ? waterCausticParameters.x : waterCausticParameters.y;
        const float texelSize = coverage / max(float(width), 1.0f);
        const float2 snappedCenter = floor(
            causticCameraPosition.xz / texelSize + 0.5f) * texelSize;
        const float2 worldPosition = snappedCenter
            + (uv - 0.5f.xx) * coverage;
        const float2 slope = SampleCombinedCausticSlope(worldPosition);
        const float3 normal = normalize(float3(-slope.x, 1.0f, -slope.y));
        const float edge = enabled ? CausticEdgeWeight(uv, edgeFade) : 0.0f;
        // The map stores surface focusing at a representative shallow
        // receiver. Exact receiver depth/extinction is applied at composite.
        const float energy = EvaluateBoundedCausticEnergy(normal,
            lightDirection, 1.0f, extinction, edge,
            waterCausticParameters.z * causticPrimaryLightIntensity);
        const float dispersion = clamp(waterCausticParameters.w,
            0.0f, 0.05f);
        const bool chromatic = waterCausticModes1.z == 2u
            && waterCausticModes0.x >= 1u;
        const float slopeAxis = clamp(slope.x * 0.125f, -1.0f, 1.0f);
        const float3 dispersed = chromatic
            ? energy * clamp(float3(
                1.0f + dispersion * slopeAxis * 24.0f,
                1.0f,
                1.0f - dispersion * slopeAxis * 24.0f),
                0.0f.xxx, 2.0f.xxx)
            : energy.xxx;
        const float4 boundedEnergy = float4(
            min(dispersed, 16.0f.xxx), energy);
        if (cascade == 0u)
            causticNearOutput[pixel] = boundedEnergy;
        else
            causticMiddleOutput[pixel] = boundedEnergy;
    }
}

struct CausticFixtureInput
{
    float4 normalDepth;
    float4 lightCoverage;
    float4 extinctionIntensity;
};

PRISM_VK_BINDING(18) StructuredBuffer<CausticFixtureInput>
    causticFixtureInputs : register(t2);
PRISM_VK_BINDING(34) RWStructuredBuffer<float4>
    causticFixtureOutputs : register(u2);

[numthreads(64, 1, 1)]
void WaterCausticAnalyticFixtureCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint inputCount;
    uint inputStride;
    causticFixtureInputs.GetDimensions(inputCount, inputStride);
    const uint index = dispatchThreadId.x;
    if (index >= inputCount)
        return;
    const CausticFixtureInput input = causticFixtureInputs[index];
    const float energy = EvaluateBoundedCausticEnergy(
        input.normalDepth.xyz,
        input.lightCoverage.xyz,
        input.normalDepth.w,
        input.extinctionIntensity.x,
        input.lightCoverage.w,
        input.extinctionIntensity.y);
    causticFixtureOutputs[index] = float4(clamp(float3(
        energy, energy * input.extinctionIntensity.z,
        energy * input.extinctionIntensity.w), 0.0f.xxx, 16.0f.xxx), 1.0f);
}
