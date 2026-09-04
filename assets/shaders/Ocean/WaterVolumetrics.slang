#include "../ShaderBindings.hlsli"

struct VolumePointLight
{
    float3 position;
    float range;
    float3 color;
    float intensity;
};

struct VolumeDirectionalLight
{
    float3 direction;
    float intensity;
    float3 color;
    float padding;
};

// Exact SharedFrameConstants layout. Keeping this block identical lets the
// volumetric stage consume the renderer-owned camera, sun, and shadow data.
PRISM_VK_BINDING(0) cbuffer VolumeFrameConstants : register(b0)
{
    float3 volumeCameraPosition;
    float volumePointLightCount;
    float3 volumePrimaryLightDirection;
    float volumePrimaryLightIntensity;
    float3 volumePrimaryLightColor;
    float volumeAmbientIntensity;
    VolumePointLight volumePointLights[4];
    float4x4 volumeLightViewProjections[3];
    float4 volumeShadowSplits;
    float3 volumeSkyZenithColor;
    float volumePbrEnabled;
    float3 volumeSkyHorizonColor;
    float volumeIblEnabled;
    float3 volumeGroundColor;
    float volumeIblIntensity;
    float volumeIblDiffuseStrength;
    float volumeIblSpecularStrength;
    float volumeIblReflectionBlend;
    float volumeIblHorizonSharpness;
    float volumeIblSplitSumEnabled;
    float volumeShadowBias;
    float volumeShadowsEnabled;
    float volumeGtaoEnabled;
    float volumeDirectLightingEnabled;
    float volumePointLightsEnabled;
    float volumeNormalMappingEnabled;
    float volumeOcclusionEnabled;
    float volumeEmissiveEnabled;
    float volumeAlphaMaskEnabled;
    float volumeClusteredLightingEnabled;
    float volumeShadowFilterMode;
    float3 volumeCameraForward;
    float volumeCascadeBlendFraction;
    float3 volumeDeepWaterColor;
    float volumePhysicalAtmosphereEnabled;
    float3 volumeScatteringColor;
    float volumeAtmosphereBrightness;
    float3 volumeFoamColor;
    float volumeScatteringStrength;
    VolumeDirectionalLight volumeAuxiliaryLights[2];
    float4 volumeCascadePatchLengths;
    float4 volumeCascadeLowerWavelengths;
    float4 volumeCascadeUpperWavelengths;
    float4 volumeCascadeUvScales;
    float4 volumeCascadeUvOffsetX;
    float4 volumeCascadeUvOffsetY;
    float4 volumeCascadeFadeStarts;
    float4 volumeCascadeFadeEnds;
    float4 volumeCascadeLayerOrder;
};

PRISM_VK_BINDING(5) cbuffer VolumeWaterConstants : register(b5)
{
    float4 volumeAbsorptionRoughness;
    float4 volumeScatteringIor;
    float4 volumePhaseThinBacklit;
    float4 volumeDistanceRanges;
    float4 volumeRefractionParameters0;
    float4 volumeRefractionParameters1;
    float4 volumeCausticsParameters;
    float4 volumeParameters0;
    float4 volumeParameters1;
    uint4 volumeModes0;
    uint4 volumeModes1;
    uint4 volumeModes2;
    float4x4 volumeInverseViewProjection;
};

PRISM_VK_BINDING(16) Texture2D<float> volumeOpaqueDepth : register(t0);
PRISM_VK_BINDING(17) Texture2D<float> volumeCompositeDepth : register(t1);
PRISM_VK_BINDING(18) Texture2D<float4> volumeWaterMask : register(t2);
PRISM_VK_BINDING(19) Texture2D<float4> volumeCausticsNear : register(t3);
PRISM_VK_BINDING(20) Texture2D<float4> volumeCausticsMiddle : register(t4);
PRISM_VK_BINDING(21) Texture2DArray volumeShadowMap : register(t5);
PRISM_VK_BINDING(32) RWTexture2D<float4> volumeCurrentOutput : register(u0);
PRISM_VK_BINDING(48) SamplerState volumeLinearClampSampler : register(s0);
PRISM_VK_BINDING(49) SamplerComparisonState volumeShadowSampler : register(s1);

float3 VolumeReconstructWorld(float2 uv, float depth)
{
    const float2 ndc = uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    const float4 homogeneous = mul(
        float4(ndc, depth, 1.0f), volumeInverseViewProjection);
    return homogeneous.xyz / max(abs(homogeneous.w), 1.0e-6f);
}

float VolumePhase(float cosineTheta, float g)
{
    const float safeG = clamp(g, -0.95f, 0.95f);
    const float denominator = max(1.0f + safeG * safeG
        - 2.0f * safeG * cosineTheta, 1.0e-4f);
    return (1.0f - safeG * safeG)
        / (12.566370614359172f * denominator * sqrt(denominator));
}

float3 IntegrateVolumeSegment(float3 extinction, float3 source, float distanceMeters)
{
    if (!all(isfinite(extinction)) || !all(isfinite(source))
        || !isfinite(distanceMeters) || distanceMeters <= 0.0f)
        return 0.0f.xxx;
    const float3 safeExtinction = max(extinction, 0.0f.xxx);
    const float3 attenuation = exp(-min(
        safeExtinction * distanceMeters, 40.0f.xxx));
    float3 integral;
    [unroll]
    for (uint channel = 0u; channel < 3u; ++channel)
        integral[channel] = safeExtinction[channel] > 1.0e-5f
            ? (1.0f - attenuation[channel]) / safeExtinction[channel]
            : distanceMeters;
    return clamp(max(source, 0.0f.xxx) * integral, 0.0f.xxx, 65504.0f.xxx);
}

float VolumeShadow(float3 worldPosition)
{
    if (volumeShadowsEnabled < 0.5f)
        return 1.0f;
    const float distanceToCamera = length(worldPosition - volumeCameraPosition);
    uint cascade = distanceToCamera > volumeShadowSplits.y ? 2u
        : (distanceToCamera > volumeShadowSplits.x ? 1u : 0u);
    const float4 lightClip = mul(float4(worldPosition, 1.0f),
        volumeLightViewProjections[cascade]);
    if (abs(lightClip.w) <= 1.0e-6f)
        return 1.0f;
    const float3 lightNdc = lightClip.xyz / lightClip.w;
    const float2 uv = lightNdc.xy * float2(0.5f, -0.5f) + 0.5f.xx;
    if (any(uv <= 0.0f.xx) || any(uv >= 1.0f.xx))
        return 1.0f;
    return volumeShadowMap.SampleCmpLevelZero(volumeShadowSampler,
        float3(uv, float(cascade)), lightNdc.z - volumeShadowBias);
}

float3 VolumeCausticContribution(float3 worldPosition)
{
    if (volumeModes1.y == 0u || worldPosition.y >= 0.0f)
        return 0.0f.xxx;
    const float2 coverages = max(volumeCausticsParameters.xy, 1.0f.xx);
    const float edgeFade = clamp(volumeParameters1.y, 0.01f, 0.49f);
    float3 energy = 0.0f.xxx;
    [unroll]
    for (uint cascade = 0u; cascade < 2u; ++cascade)
    {
        const float coverage = coverages[cascade];
        const float texelSize = coverage / float(max(volumeModes1.w, 1u));
        const float2 center = floor(volumeCameraPosition.xz / texelSize + 0.5f) * texelSize;
        const float2 uv = (worldPosition.xz - center)
            / coverage + 0.5f.xx;
        if (all(uv >= 0.0f.xx) && all(uv <= 1.0f.xx))
        {
            const float inside = 0.5f
                - max(abs(uv.x - 0.5f), abs(uv.y - 0.5f));
            const float weight = smoothstep(0.0f,
                edgeFade * 0.5f, inside);
            const float3 sampleEnergy = cascade == 0u
                ? volumeCausticsNear.SampleLevel(
                    volumeLinearClampSampler, uv, 0.0f).rgb
                : volumeCausticsMiddle.SampleLevel(
                    volumeLinearClampSampler, uv, 0.0f).rgb;
            energy = max(energy, sampleEnergy * weight);
        }
    }
    return min(energy, 16.0f.xxx);
}

[numthreads(8, 8, 1)]
void WaterVolumetricAccumulateCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    volumeCurrentOutput.GetDimensions(width, height);
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= width || pixel.y >= height)
        return;
    const bool enabled = volumeModes2.x != 0u
        && volumeParameters1.z > 0.5f;
    if (!enabled)
    {
        volumeCurrentOutput[pixel] = 0.0f.xxxx;
        return;
    }

    const float2 uv = (float2(pixel) + 0.5f) / float2(width, height);
    uint fullWidth, fullHeight;
    volumeOpaqueDepth.GetDimensions(fullWidth, fullHeight);
    const int2 fullPixel = min(int2(uv * float2(fullWidth, fullHeight)),
        int2(fullWidth, fullHeight) - 1);
    // Never interpolate foreground and background device depths.
    const float opaqueDepth = volumeOpaqueDepth.Load(int3(fullPixel, 0));
    const float waterDepth = volumeCompositeDepth.Load(int3(fullPixel, 0));
    const float waterMask = volumeWaterMask.Load(int3(fullPixel, 0)).a;
    float boundaryDepth = opaqueDepth;
    if (waterMask > 0.0f)
        boundaryDepth = min(boundaryDepth, waterDepth);
    const float3 boundary = VolumeReconstructWorld(uv, boundaryDepth);
    const float3 ray = boundary - volumeCameraPosition;
    const float rayLength = min(length(ray), max(volumeParameters0.y, 0.1f));
    if (!all(isfinite(boundary)) || !isfinite(rayLength)
        || rayLength <= 1.0e-3f)
    {
        volumeCurrentOutput[pixel] = 0.0f.xxxx;
        return;
    }
    const float3 direction = ray / max(length(ray), 1.0e-5f);
    const uint sampleCount = clamp(volumeModes2.y, 1u, 128u);
    const float stepLength = rayLength / float(sampleCount);
    const float3 absorption = max(volumeAbsorptionRoughness.xyz, 0.0f.xxx);
    const float3 scattering = max(volumeScatteringIor.xyz, 0.0f.xxx);
    const float3 extinction = absorption + scattering;
    const float3 lightDirection = -volumePrimaryLightDirection
        * rsqrt(max(dot(volumePrimaryLightDirection, volumePrimaryLightDirection), 1.0e-8f));
    const float phase = VolumePhase(dot(direction, lightDirection),
        volumePhaseThinBacklit.x);
    float3 transmittance = 1.0f.xxx;
    float3 radiance = 0.0f.xxx;
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        const float t = (float(sampleIndex) + 0.5f) * stepLength;
        const float3 worldPosition = volumeCameraPosition + direction * t;
        const float shadow = VolumeShadow(worldPosition);
        // Attenuate incident sunlight along its water path as well as along
        // the camera ray; otherwise deep water receives surface-strength sun.
        const float sunlightDistance = max(-worldPosition.y, 0.0f)
            / max(lightDirection.y, 0.05f);
        const float3 incomingTransmittance = exp(-min(
            extinction * sunlightDistance, 40.0f.xxx))
            * (lightDirection.y > 0.0f ? 0.98f : 0.0f);
        const float3 caustic = VolumeCausticContribution(worldPosition)
            * 0.08f;
        const float3 source = scattering * phase
            * max(volumePrimaryLightColor, 0.0f.xxx)
            * max(volumePrimaryLightIntensity, 0.0f)
            * shadow * incomingTransmittance + scattering * caustic;
        radiance += transmittance * IntegrateVolumeSegment(
            extinction, source, stepLength);
        transmittance *= exp(-min(extinction * stepLength, 40.0f.xxx));
    }
    const float3 bounded = all(isfinite(radiance))
        ? clamp(radiance, 0.0f.xxx, 65504.0f.xxx) : 0.0f.xxx;
    volumeCurrentOutput[pixel] = float4(
        bounded, rayLength / max(volumeParameters0.y, 0.1f));
}

PRISM_VK_BINDING(23) Texture2D<float4> volumeTemporalCurrent : register(t7);
PRISM_VK_BINDING(24) Texture2D<float4> volumeTemporalHistory : register(t8);
PRISM_VK_BINDING(25) Texture2D<float2> volumeTemporalMotion : register(t9);
PRISM_VK_BINDING(28) Texture2D<float2> volumeTemporalWaterMotion : register(t12);
PRISM_VK_BINDING(34) RWTexture2D<float4> volumeTemporalOutput : register(u2);

[numthreads(8, 8, 1)]
void WaterVolumetricTemporalCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    volumeTemporalOutput.GetDimensions(width, height);
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= width || pixel.y >= height)
        return;
    const float2 uv = (float2(pixel) + 0.5f) / float2(width, height);
    const float4 current = volumeTemporalCurrent.Load(int3(pixel, 0));
    const bool waterPixel = volumeWaterMask.SampleLevel(
        volumeLinearClampSampler, uv, 0.0f).a > 0.0f;
    const float2 motion = waterPixel
        ? volumeTemporalWaterMotion.SampleLevel(volumeLinearClampSampler, uv, 0.0f)
        : volumeTemporalMotion.SampleLevel(volumeLinearClampSampler, uv, 0.0f);
    const float2 previousUv = uv - motion;
    const bool validUv = all(previousUv > 0.0f.xx)
        && all(previousUv < 1.0f.xx) && all(isfinite(motion));
    const float4 history = validUv && volumeParameters1.w > 0.5f
        ? volumeTemporalHistory.SampleLevel(
            volumeLinearClampSampler, previousUv, 0.0f)
        : current;
    const float depthDeltaMeters = abs(current.a - abs(history.a))
        * max(volumeParameters0.y, 0.1f);
    const bool accept = volumeParameters1.w > 0.5f && validUv
        && all(isfinite(history))
        && depthDeltaMeters <= max(volumeParameters0.w, 0.001f);
    const float weight = accept
        ? clamp(volumeParameters0.z, 0.0f, 0.99f) : 0.0f;
    float3 minimumRadiance = current.rgb;
    float3 maximumRadiance = current.rgb;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float3 neighbor = volumeTemporalCurrent.Load(int3(clamp(
                int2(pixel) + int2(x, y), int2(0, 0), int2(width, height) - 1), 0)).rgb;
            minimumRadiance = min(minimumRadiance, neighbor);
            maximumRadiance = max(maximumRadiance, neighbor);
        }
    const float4 result = float4(lerp(current.rgb,
        clamp(history.rgb, minimumRadiance, maximumRadiance), weight),
        accept ? current.a : -current.a);
    volumeTemporalOutput[pixel] = all(isfinite(result))
        ? result : current;
}

PRISM_VK_BINDING(26) Texture2D<float4> volumeReconstructInput : register(t10);
PRISM_VK_BINDING(27) Texture2D<float> volumeReconstructDepth : register(t11);
PRISM_VK_BINDING(35) RWTexture2D<float4> volumeReconstructOutput
    : register(u3);

[numthreads(8, 8, 1)]
void WaterVolumetricReconstructCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    volumeReconstructOutput.GetDimensions(width, height);
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= width || pixel.y >= height)
        return;
    const float2 uv = (float2(pixel) + 0.5f) / float2(width, height);
    const float centerDeviceDepth = volumeReconstructDepth.Load(int3(pixel, 0));
    const float centerDepth = min(length(VolumeReconstructWorld(
        uv, centerDeviceDepth) - volumeCameraPosition), volumeParameters0.y);
    uint sourceWidth;
    uint sourceHeight;
    volumeReconstructInput.GetDimensions(sourceWidth, sourceHeight);
    const int2 sourceCenter = min(int2(uv * float2(sourceWidth, sourceHeight)),
        int2(sourceWidth, sourceHeight) - 1);
    const int radius = int(min(volumeModes2.z, 3u));
    float4 accumulated = 0.0f.xxxx;
    float totalWeight = 0.0f;
    float4 closest = volumeReconstructInput.Load(int3(sourceCenter, 0));
    float closestDelta = abs(abs(closest.a) * volumeParameters0.y - centerDepth);
    [loop]
    for (int y = -radius; y <= radius; ++y)
    {
        [loop]
        for (int x = -radius; x <= radius; ++x)
        {
            const int2 coordinate = clamp(sourceCenter + int2(x, y),
                int2(0, 0), int2(sourceWidth, sourceHeight) - 1);
            const float4 sample = volumeReconstructInput.Load(int3(coordinate, 0));
            const float sampleDepth = abs(sample.a) * volumeParameters0.y;
            const float delta = abs(sampleDepth - centerDepth);
            if (delta < closestDelta) { closest = sample; closestDelta = delta; }
            const float depthWeight = exp(-abs(sampleDepth - centerDepth)
                / max(volumeParameters0.w, 0.001f));
            const float spatialWeight = exp(-0.5f * float(x * x + y * y));
            const float weight = depthWeight * spatialWeight;
            accumulated += sample * weight;
            totalWeight += weight;
        }
    }
    const float4 center = volumeReconstructInput.Load(int3(sourceCenter, 0));
    // A sub-pixel foreground may have no representative low-resolution tap.
    // rescale the closest tap's finite optical integral to the true ray length,
    // rather than copying an unrelated long background ray onto the object.
    const float3 extinction = max(volumeAbsorptionRoughness.xyz + volumeScatteringIor.xyz, 0.0f.xxx);
    const float sourceDepth = max(abs(closest.a) * volumeParameters0.y, 1.0e-4f);
    const float3 targetIntegral = IntegrateVolumeSegment(extinction, 1.0f.xxx, centerDepth);
    const float3 sourceIntegral = IntegrateVolumeSegment(extinction, 1.0f.xxx, sourceDepth);
    const float3 fallback = closest.rgb * min(targetIntegral / max(sourceIntegral, 1.0e-4f.xxx), 1.0f.xxx);
    const float4 result = float4(totalWeight > 1.0e-5f
        ? accumulated.rgb / totalWeight : fallback,
        (center.a < 0.0f ? -1.0f : 1.0f)
            * centerDepth / max(volumeParameters0.y, 0.1f));
    volumeReconstructOutput[pixel] = all(isfinite(result))
        ? result : 0.0f.xxxx;
}

struct VolumeFixtureInput
{
    float4 extinctionDistance;
    float4 scatteringPhase;
};

PRISM_VK_BINDING(22) StructuredBuffer<VolumeFixtureInput>
    volumeFixtureInputs : register(t6);
PRISM_VK_BINDING(33) RWStructuredBuffer<float4>
    volumeFixtureOutputs : register(u1);

[numthreads(64, 1, 1)]
void WaterVolumetricAnalyticFixtureCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint count;
    uint stride;
    volumeFixtureInputs.GetDimensions(count, stride);
    if (dispatchThreadId.x >= count)
        return;
    const VolumeFixtureInput input = volumeFixtureInputs[dispatchThreadId.x];
    const float3 extinction = max(input.extinctionDistance.xyz, 0.0f.xxx);
    const float distanceMeters = max(input.extinctionDistance.w, 0.0f);
    const float3 scattering = max(input.scatteringPhase.xyz, 0.0f.xxx);
    const float phase = max(input.scatteringPhase.w, 0.0f);
    const float3 integrated = IntegrateVolumeSegment(
        extinction, scattering * phase, distanceMeters);
    volumeFixtureOutputs[dispatchThreadId.x] = float4(
        all(isfinite(integrated))
            ? clamp(integrated, 0.0f.xxx, 65504.0f.xxx)
            : 0.0f.xxx,
        distanceMeters);
}
