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
Texture2D<float4> fluidSceneColor : register(t0);
PRISM_VK_BINDING(17)
Texture2D<float> fluidSceneDepth : register(t1);
PRISM_VK_BINDING(18)
Texture2D<float> fluidRawDepth : register(t2);
PRISM_VK_BINDING(19)
Texture2D<float> fluidSmoothDepth : register(t3);
PRISM_VK_BINDING(20)
Texture2D<float> fluidThickness : register(t4);
PRISM_VK_BINDING(21)
Texture2D<float4> fluidNormal : register(t5);
PRISM_VK_BINDING(22)
Texture2D<float> fluidFoam : register(t6);
PRISM_VK_BINDING(23)
Texture2D<float> fluidMask : register(t7);
PRISM_VK_BINDING(24)
TextureCube<float4> fluidEnvironment : register(t8);
PRISM_VK_BINDING(25)
Texture2D<float4> fluidCaustics : register(t9);
PRISM_VK_BINDING(32)
RWTexture2D<float4> fluidCompositeOutput : register(u0);
PRISM_VK_BINDING(48)
SamplerState fluidLinearClampSampler : register(s0);

#define fluidResolution fluidResolutionInverseResolution.xy
#define fluidInverseResolution fluidResolutionInverseResolution.zw
#define fluidAbsorption fluidAbsorptionScattering.xyz
#define fluidScatteringStrength fluidAbsorptionScattering.w
#define fluidWaterColor fluidWaterColorIor.xyz
#define fluidIor fluidWaterColorIor.w
#define fluidRefractionScale fluidOpticalParameters.x
#define fluidReflectionStrength fluidOpticalParameters.y
#define fluidThicknessScale fluidOpticalParameters.z
#define fluidFoamIntensity fluidOpticalParameters.w
#define fluidFoamThicknessThreshold fluidFoamParameters.y
#define fluidCausticsIntensity fluidFoamParameters.w
#define fluidSurfaceCoverageThreshold fluidDensityParameters.w
#define fluidToonDiffuseSteps fluidToonParameters.x
#define fluidToonReflectionSteps fluidToonParameters.y
#define fluidOutlineDepthThreshold fluidToonParameters.z
#define fluidOutlineWidth fluidToonParameters.w
#define fluidSilhouetteSmoothingRadius \
    ((int)round(fluidLightDirectionOutline.w))
#define fluidShadingMode fluidCountsAndModes.y
#define fluidDisplayMode fluidCountsAndModes.z
#define fluidCausticsEnabled fluidCountsAndModes.w

bool IsSurfaceDepth(float depth)
{
    return depth > 1.0e-5f && isfinite(depth);
}

// Thickness is already an additive screen-space field. Normalize it by one
// rendered particle diameter and use it as final-surface confidence: weak
// single-sphere fringes disappear, while overlapping liquid remains. A hole
// reconstructed by FluidFilter has no raw sphere depth and is kept because it
// was explicitly bounded by valid samples on both sides.
bool IsCoherentSurfacePixel(int2 pixel)
{
    const float smoothDepth = fluidSmoothDepth.Load(
        int3(pixel, 0));
    if (!IsSurfaceDepth(smoothDepth))
    {
        return false;
    }
    if (fluidSurfaceCoverageThreshold <= 0.0f)
    {
        return true;
    }
    const float rawDepth = fluidRawDepth.Load(int3(pixel, 0));
    if (!IsSurfaceDepth(rawDepth))
    {
        return true;
    }
    const float rawThickness = max(
        fluidThickness.Load(int3(pixel, 0)),
        0.0f);
    const float normalizedCoverage = rawThickness / max(
        2.0f * fluidCameraPositionParticleRadius.w,
        1.0e-5f);
    return normalizedCoverage >= fluidSurfaceCoverageThreshold;
}

// The bilateral chain deliberately preserves the validity silhouette. That is
// correct for dense particle sets, but at the 64K real-time LOD the outermost
// row of sphere splats can still read as a string of beads. Apply an adaptive
// majority reconstruction only to pixels close to that silhouette. Interior
// pixels take the nine-tap fast path and retain the simulated wave detail.
bool ResolveSmoothedSilhouette(
    int2 pixel,
    out float resolvedDepth,
    out float silhouetteCoverage)
{
    const int2 resolution = int2(fluidResolution);
    const float centerDepth = fluidSmoothDepth.Load(
        int3(pixel, 0));
    const bool centerValid = IsSurfaceDepth(centerDepth);
    const int configuredRadius = clamp(
        fluidSilhouetteSmoothingRadius, 0, 8);
    if (configuredRadius == 0)
    {
        resolvedDepth = centerDepth;
        silhouetteCoverage = centerValid ? 1.0f : 0.0f;
        return centerValid && IsCoherentSurfacePixel(pixel);
    }

    int radius = configuredRadius;
    if (centerValid)
    {
        const float projectedParticleRadiusPixels =
            abs(fluidProjection[1][1])
            * fluidCameraPositionParticleRadius.w
            * fluidResolution.y * 0.5f
            / max(centerDepth, 1.0e-4f);
        radius = clamp(
            max(
                configuredRadius,
                (int)ceil(projectedParticleRadiusPixels * 0.25f)),
            1,
            8);
    }

    static const int2 probeOffsets[9] =
    {
        int2(0, 0),
        int2(-1, 0), int2(1, 0),
        int2(0, -1), int2(0, 1),
        int2(-1, -1), int2(1, -1),
        int2(-1, 1), int2(1, 1)
    };
    int probeCount = 0;
    [unroll]
    for (int probeIndex = 0; probeIndex < 9; ++probeIndex)
    {
        const int2 probePixel = clamp(
            pixel + probeOffsets[probeIndex] * radius,
            int2(0, 0),
            resolution - 1);
        probeCount += IsSurfaceDepth(fluidSmoothDepth.Load(
            int3(probePixel, 0))) ? 1 : 0;
    }
    if (centerValid && probeCount == 9)
    {
        resolvedDepth = centerDepth;
        silhouetteCoverage = 1.0f;
        return IsCoherentSurfacePixel(pixel);
    }
    if (!centerValid && probeCount == 0)
    {
        resolvedDepth = 0.0f;
        silhouetteCoverage = 0.0f;
        return false;
    }

    int validCount = 0;
    float weightedDepth = 0.0f;
    float totalWeight = 0.0f;
    [loop]
    for (int y = -8; y <= 8; ++y)
    {
        [loop]
        for (int x = -8; x <= 8; ++x)
        {
            if (abs(x) > radius || abs(y) > radius)
            {
                continue;
            }
            const int2 samplePixel = clamp(
                pixel + int2(x, y),
                int2(0, 0),
                resolution - 1);
            const float sampleDepth = fluidSmoothDepth.Load(
                int3(samplePixel, 0));
            if (!IsSurfaceDepth(sampleDepth))
            {
                continue;
            }
            const float weight = rcp(
                1.0f + (float)(x * x + y * y));
            weightedDepth += sampleDepth * weight;
            totalWeight += weight;
            ++validCount;
        }
    }
    const int diameter = radius * 2 + 1;
    silhouetteCoverage = (float)validCount
        / (float)(diameter * diameter);
    const float requiredCoverage = centerValid ? 0.46f : 0.62f;
    if (silhouetteCoverage < requiredCoverage
        || totalWeight <= 1.0e-6f)
    {
        resolvedDepth = 0.0f;
        return false;
    }
    if (centerValid && !IsCoherentSurfacePixel(pixel))
    {
        resolvedDepth = 0.0f;
        return false;
    }
    resolvedDepth = centerValid
        ? centerDepth
        : weightedDepth / totalWeight;
    return true;
}

float3 SampleBackground(float2 uv)
{
    float3 color = fluidSceneColor.SampleLevel(
        fluidLinearClampSampler, uv, 0.0f).rgb;
    if (fluidCausticsEnabled != 0u)
    {
        color += fluidCaustics.SampleLevel(
            fluidLinearClampSampler, uv, 0.0f).rgb
            * fluidCausticsIntensity;
    }
    return color;
}

float3 ReconstructViewPosition(
    float2 uv,
    float linearDepth)
{
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

float ProjectViewDepth(float3 viewPosition)
{
    const float4 clip = mul(
        float4(viewPosition, 1.0f),
        fluidProjection);
    return clip.z / max(clip.w, 1.0e-6f);
}

// The raw additive thickness and reconstructed normal still contain a small
// amount of particle-frequency energy. A nine-tap, depth-aware kernel smooths
// both attributes in the existing composite pass and also supplies thickness
// for the bounded holes closed by FluidFilter, avoiding a separate blur pass.
void SampleSurfaceAttributes(
    uint2 pixel,
    float centerDepth,
    out float3 filteredNormal,
    out float filteredThickness)
{
    static const int2 offsets[9] =
    {
        int2(0, 0),
        int2(-1, 0),
        int2(1, 0),
        int2(0, -1),
        int2(0, 1),
        int2(-1, -1),
        int2(1, -1),
        int2(-1, 1),
        int2(1, 1)
    };
    static const float spatialWeights[9] =
    {
        4.0f,
        2.0f, 2.0f, 2.0f, 2.0f,
        1.0f, 1.0f, 1.0f, 1.0f
    };

    const int2 resolution = int2(fluidResolution);
    // Depth has already gone through the coarse-to-fine bilateral chain.
    // Sample only a compact neighbourhood here: sparse taps one full projected
    // particle apart reintroduced a honeycomb normal/reflection pattern.
    const float projectedParticleRadiusPixels =
        abs(fluidProjection[1][1])
        * fluidCameraPositionParticleRadius.w
        * fluidResolution.y * 0.5f
        / max(centerDepth, 1.0e-4f);
    const int attributeSampleRadius = clamp(
        max(
            (int)fluidFilterParameters.w,
            (int)ceil(projectedParticleRadiusPixels * 0.5f)),
        1,
        15);
    const float rangeSigma = max(
        fluidFilterParameters.z,
        fluidCameraPositionParticleRadius.w * 2.0f);
    const float inverseRangeVariance = 0.5f
        / max(rangeSigma * rangeSigma, 1.0e-6f);
    float3 normalSum = 0.0f.xxx;
    float normalWeight = 0.0f;
    float thicknessSum = 0.0f;
    float thicknessWeight = 0.0f;
    [unroll]
    for (int sampleIndex = 0; sampleIndex < 9; ++sampleIndex)
    {
        const int2 samplePixel = clamp(
            int2(pixel)
                + offsets[sampleIndex] * attributeSampleRadius,
            int2(0, 0),
            resolution - 1);
        const float sampleDepth = fluidSmoothDepth.Load(
            int3(samplePixel, 0));
        if (!IsSurfaceDepth(sampleDepth))
        {
            continue;
        }
        const float depthDifference = sampleDepth - centerDepth;
        const float weight = spatialWeights[sampleIndex]
            * exp(-depthDifference * depthDifference
                * inverseRangeVariance);
        const float3 sampleNormal = fluidNormal.Load(
            int3(samplePixel, 0)).xyz;
        if (all(isfinite(sampleNormal))
            && dot(sampleNormal, sampleNormal) > 1.0e-5f)
        {
            normalSum += sampleNormal * weight;
            normalWeight += weight;
        }
        const float sampleThickness = fluidThickness.Load(
            int3(samplePixel, 0));
        if (sampleThickness > 0.0f && isfinite(sampleThickness))
        {
            thicknessSum += sampleThickness * weight;
            thicknessWeight += weight;
        }
    }
    filteredNormal = normalWeight > 1.0e-5f
        ? normalize(normalSum / normalWeight)
        : float3(0.0f, 0.0f, -1.0f);
    filteredThickness = thicknessWeight > 1.0e-5f
        ? thicknessSum / thicknessWeight
        : 0.0f;
}

float Quantize01(float value, float steps)
{
    // "Steps" is the number of visible bands, so three steps must produce
    // dark, middle, and bright values rather than four interval endpoints.
    const float safeIntervals = max(steps - 1.0f, 1.0f);
    return floor(saturate(value) * safeIntervals + 0.5f)
        / safeIntervals;
}

bool IsFluidOutline(float silhouetteCoverage)
{
    // Coverage falls continuously across the reconstructed majority kernel.
    // Mapping the toon outline to that field follows the smoothed silhouette
    // instead of retracing every raw particle circle.
    const float outlineThreshold = saturate(
        0.50f + max(fluidOutlineWidth, 0.0f) * 0.04f);
    return silhouetteCoverage < outlineThreshold;
}

float3 BuildDebugColor(
    uint2 pixel,
    float mask,
    float rawDepth,
    float smoothDepth,
    float thickness,
    float3 normal,
    float foam)
{
    if (fluidDisplayMode == 1u)
    {
        return (1.0f - exp(-rawDepth * 0.15f)).xxx;
    }
    if (fluidDisplayMode == 2u)
    {
        return (1.0f - exp(-smoothDepth * 0.15f)).xxx;
    }
    if (fluidDisplayMode == 3u)
    {
        return saturate(thickness * 2.0f).xxx;
    }
    if (fluidDisplayMode == 4u)
    {
        return normal * 0.5f + 0.5f;
    }
    if (fluidDisplayMode == 5u)
    {
        return lerp(
            float3(0.02f, 0.05f, 0.09f),
            float3(1.0f, 1.0f, 1.0f),
            foam);
    }
    if (fluidDisplayMode == 6u)
    {
        return mask.xxx;
    }
    return 0.0f.xxx;
}

[numthreads(8, 8, 1)]
void FluidCompositeCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= uint2(fluidResolution)))
    {
        return;
    }
    const float2 uv =
        (float2(pixel) + 0.5f) * fluidInverseResolution;
    const float4 sourceColor = fluidSceneColor.Load(
        int3(pixel, 0));
    const float rawMask = fluidMask.Load(int3(pixel, 0));

    // The particle diagnostics path deliberately schedules no thickness,
    // filter, normal, environment, or caustics resources. Reuse raw sphere
    // depth for inexpensive blue-particle lighting, and keep the uniform
    // return before every access to the omitted textures.
    if (fluidDisplayMode == 6u)
    {
        if (rawMask <= 0.5f)
        {
            fluidCompositeOutput[pixel] = sourceColor;
            return;
        }
        const int2 resolution = int2(fluidResolution);
        const int2 centerPixel = int2(pixel);
        const float centerDepth = fluidRawDepth.Load(
            int3(centerPixel, 0));
        const int2 leftPixel = clamp(
            centerPixel + int2(-1, 0),
            int2(0, 0),
            resolution - 1);
        const int2 rightPixel = clamp(
            centerPixel + int2(1, 0),
            int2(0, 0),
            resolution - 1);
        const int2 upPixel = clamp(
            centerPixel + int2(0, -1),
            int2(0, 0),
            resolution - 1);
        const int2 downPixel = clamp(
            centerPixel + int2(0, 1),
            int2(0, 0),
            resolution - 1);
        const float leftDepth = fluidMask.Load(
                int3(leftPixel, 0)) > 0.5f
            ? fluidRawDepth.Load(int3(leftPixel, 0))
            : centerDepth;
        const float rightDepth = fluidMask.Load(
                int3(rightPixel, 0)) > 0.5f
            ? fluidRawDepth.Load(int3(rightPixel, 0))
            : centerDepth;
        const float upDepth = fluidMask.Load(
                int3(upPixel, 0)) > 0.5f
            ? fluidRawDepth.Load(int3(upPixel, 0))
            : centerDepth;
        const float downDepth = fluidMask.Load(
                int3(downPixel, 0)) > 0.5f
            ? fluidRawDepth.Load(int3(downPixel, 0))
            : centerDepth;
        const float inverseDiameter = 0.5f / max(
            fluidCameraPositionParticleRadius.w,
            1.0e-4f);
        const float3 previewNormal = normalize(float3(
            (leftDepth - rightDepth) * inverseDiameter,
            (upDepth - downDepth) * inverseDiameter,
            -1.0f));
        const float3 previewLight = normalize(
            float3(-0.35f, -0.55f, -0.75f));
        const float diffuse = saturate(dot(
            previewNormal,
            previewLight));
        const float rim = pow(
            1.0f - saturate(-previewNormal.z),
            2.0f);
        float3 particleColor = lerp(
            float3(0.015f, 0.10f, 0.30f),
            float3(0.10f, 0.62f, 0.95f),
            0.22f + 0.78f * diffuse);
        particleColor += rim * float3(0.08f, 0.22f, 0.32f);
        fluidCompositeOutput[pixel] = float4(
            particleColor,
            sourceColor.a);
        return;
    }

    const float3 background = SampleBackground(uv);
    float smoothDepth = 0.0f;
    float silhouetteCoverage = 0.0f;
    const float mask = ResolveSmoothedSilhouette(
        int2(pixel),
        smoothDepth,
        silhouetteCoverage) ? 1.0f : 0.0f;

    if (mask <= 0.5f)
    {
        fluidCompositeOutput[pixel] =
            float4(background, sourceColor.a);
        return;
    }

    const float rawDepth = fluidRawDepth.Load(
        int3(pixel, 0));
    float3 normalView = 0.0f.xxx;
    float rawThickness = 0.0f;
    SampleSurfaceAttributes(
        pixel,
        smoothDepth,
        normalView,
        rawThickness);
    const float thickness = rawThickness * fluidThicknessScale;
    float foam = 0.0f;
    if (fluidFoamIntensity > 0.0f)
    {
        foam = saturate(
            fluidFoam.Load(int3(pixel, 0))
            * fluidFoamIntensity);
    }
    if (!all(isfinite(normalView)))
    {
        normalView = float3(0.0f, 0.0f, -1.0f);
    }

    if (fluidDisplayMode != 0u)
    {
        fluidCompositeOutput[pixel] = float4(
            BuildDebugColor(
                pixel,
                mask,
                rawDepth,
                smoothDepth,
                thickness,
                normalView,
                foam),
            1.0f);
        return;
    }

    const float3 viewPosition = ReconstructViewPosition(
        uv, smoothDepth);
    const float fluidDeviceDepth = ProjectViewDepth(viewPosition);
    const float centerSceneDepth = fluidSceneDepth.Load(
        int3(pixel, 0));
    if (centerSceneDepth + 1.0e-4f < fluidDeviceDepth)
    {
        fluidCompositeOutput[pixel] =
            float4(background, sourceColor.a);
        return;
    }
    const float3 incidentView = normalize(viewPosition);
    const float distortionAmount = fluidRefractionScale
        * saturate(thickness * 0.65f + 0.05f);
    const float2 refractionOffset =
        normalView.xy * distortionAmount;
    const float2 distanceToViewportEdge = min(
        uv,
        1.0f.xx - uv);
    // Clamping an out-of-bounds refraction coordinate duplicated the last
    // bright row/column over a wide area when the camera approached or
    // entered the water. Fade only the unsafe part of the distortion so the
    // lookup remains continuous and never turns a screen edge into a white
    // smear.
    const float2 safeOffsetRatio = distanceToViewportEdge
        / max(abs(refractionOffset), fluidInverseResolution);
    const float refractionEdgeFade = smoothstep(
        0.0f,
        1.0f,
        saturate(min(safeOffsetRatio.x, safeOffsetRatio.y)));
    float2 refractedUv = saturate(
        uv + refractionOffset * refractionEdgeFade);
    const uint2 refractedPixel = min(
        uint2(refractedUv * fluidResolution),
        uint2(fluidResolution) - 1u);
    const float refractedSceneDepth = fluidSceneDepth.Load(
        int3(refractedPixel, 0));
    if (refractedSceneDepth + 1.0e-4f < fluidDeviceDepth)
    {
        // Do not pull a foreground object through the water silhouette.
        refractedUv = uv;
    }
    const float3 refractedColor = SampleBackground(refractedUv);

    const float3 transmittance = exp(
        -max(fluidAbsorption, 0.0f.xxx)
        * max(thickness, 0.0f));
    const float3 scatteredColor = fluidWaterColor
        * (1.0f - transmittance)
        * saturate(0.55f + fluidScatteringStrength);

    const float3 reflectedView = normalize(reflect(
        incidentView, normalView));
    const float3 reflectedWorld = normalize(mul(
        float4(reflectedView, 0.0f),
        fluidInverseView).xyz);
    const float3 environmentColor = fluidEnvironment.SampleLevel(
        fluidLinearClampSampler,
        reflectedWorld,
        0.0f).rgb;
    const float f0 = pow(
        (fluidIor - 1.0f) / (fluidIor + 1.0f),
        2.0f);
    const float viewFacing = saturate(dot(
        -incidentView, normalView));
    float fresnel = f0
        + (1.0f - f0) * pow(1.0f - viewFacing, 5.0f);

    const float3 normalWorld = normalize(mul(
        float4(normalView, 0.0f),
        fluidInverseView).xyz);
    const float3 lightDirection = normalize(
        fluidLightDirectionOutline.xyz);
    float diffuse = saturate(dot(
        normalWorld, -lightDirection));
    float3 waterColor = refractedColor * transmittance
        + scatteredColor;

    if (fluidShadingMode == 1u)
    {
        // Match the reference cartoon shader's view-facing bands. Keeping a
        // small directional-light contribution preserves readable waves, but
        // never lets an interior depth ripple collapse to nearly black.
        const float facingBand = Quantize01(
            viewFacing, fluidToonDiffuseSteps);
        const float diffuseBand = Quantize01(
            diffuse, fluidToonDiffuseSteps);
        const float toonBrightness = lerp(
            0.44f,
            1.05f,
            saturate(facingBand * 0.78f + diffuseBand * 0.22f));
        fresnel = Quantize01(
            fresnel, fluidToonReflectionSteps);
        const float3 quantizedColor = saturate(
            fluidWaterColor * toonBrightness);
        const float refractedMix = lerp(
            0.01f,
            0.05f,
            facingBand) * exp(-max(thickness, 0.0f) * 0.50f);
        waterColor = lerp(
            quantizedColor,
            refractedColor,
            saturate(refractedMix));
    }

    // A small broad reflection term keeps face-on water readable, while a
    // bounded energy-conserving lerp prevents residual particle normals from
    // turning into additive white rings at grazing angles.
    const float environmentWeight = saturate(
        fluidReflectionStrength
        * (fluidShadingMode == 1u
            ? lerp(0.02f, 0.12f, fresnel)
            : lerp(0.08f, 1.0f, fresnel)));
    const float viewportEdgeDistance = min(
        min(uv.x, 1.0f - uv.x),
        min(uv.y, 1.0f - uv.y));
    // Reconstructed normals are one-sided and least reliable where a large
    // close-up splat is clipped by the viewport. Keep the water tint there,
    // but suppress the high-energy cubemap reflection that previously made
    // the lower/right edge flash white.
    const float reflectionEdgeConfidence = smoothstep(
        0.0f,
        0.02f,
        viewportEdgeDistance);
    float3 result = lerp(
        waterColor,
        environmentColor,
        environmentWeight * reflectionEdgeConfidence);
    const float3 foamColor = fluidShadingMode == 1u
        ? float3(0.80f, 0.94f, 1.0f)
        : float3(0.94f, 0.98f, 1.0f);
    // Density alone marks most of a coherent free surface as low-density.
    // Restrict cartoon foam to optically thin liquid so it remains on spray,
    // crests, and narrow sheets instead of whitening the whole water body.
    const float thinFoam = fluidShadingMode == 1u
        ? 1.0f - smoothstep(
            fluidFoamThicknessThreshold,
            fluidFoamThicknessThreshold * 3.0f,
            thickness)
        : 1.0f;
    const float foamBlend = foam * thinFoam
        * (fluidShadingMode == 1u ? 0.82f : 1.0f);
    result = lerp(result, foamColor, foamBlend);
    if (fluidShadingMode == 1u
        && IsFluidOutline(silhouetteCoverage))
    {
        result *= 0.035f;
    }
    fluidCompositeOutput[pixel] =
        float4(result, sourceColor.a);
}
