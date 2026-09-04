// Dedicated spectral-ocean entry points. The shared material and lighting
// implementation stays included so the RHI descriptor contract is identical
// while the WaveWorks Lab receives its own graphics PSOs.
#define PRISM_OCEAN_CASCADE_METADATA 1
#if defined(PRISM_WATER_OPTICS_CONSTANTS)
float GetWaterOpticsLocalDistanceWeight(float2 worldPosition);
#define PRISM_OCEAN_LOCAL_DISTANCE_WEIGHT(worldPosition) \
    GetWaterOpticsLocalDistanceWeight(worldPosition)
#endif
#include "Mesh.hlsl"

// Feature-owned optical material data. WaterVisibility.slang defines the
// feature switch so ordinary ocean/mesh entry points keep their old layouts.
// Explicit reserved vectors keep the DXIL/SPIR-V CBV contract at 256 bytes.
#if defined(PRISM_WATER_OPTICS_CONSTANTS)
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
    uint4 waterOpticsModes0;
    uint4 waterOpticsModes1;
    uint4 waterOpticsModes2;
    float4 waterOpticsReserved[4];
};
#else
static const float4 waterAbsorptionRoughness =
    float4(0.16f, 0.055f, 0.025f, 0.035f);
static const float4 waterScatteringIor =
    float4(0.018f, 0.065f, 0.09f, 1.333f);
#endif

float3 EvaluateWaterOpticsDistanceTiers(float distanceMeters)
{
#if defined(PRISM_WATER_OPTICS_CONSTANTS)
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
#else
    return float3(1.0f, 0.0f, 0.0f);
#endif
}

float GetWaterOpticsLocalDistanceWeight(float2 worldPosition)
{
    const float3 tiers = EvaluateWaterOpticsDistanceTiers(
        length(worldPosition - cameraPosition.xz));
    // Local displacement is useful in the near/middle tiers only. The
    // spectral displacement remains untouched for every distance.
    return saturate(tiers.x + tiers.y);
}

struct OceanPatchInstance
{
    float4 centerHalfExtent;
    float4 morphEdgeLod;
};

// The buffer is intentionally declared by the dedicated ocean shader rather
// than Mesh.hlsl.  This keeps ordinary mesh reflection unchanged while giving
// the adaptive path a stable, backend-neutral binding.
PRISM_VK_BINDING(42)
StructuredBuffer<OceanPatchInstance> oceanPatchInstances : register(t26);

float OceanHash(float2 value)
{
    return frac(sin(dot(value, float2(127.1f, 311.7f))) * 43758.5453f);
}

float OceanValueNoise(float2 value)
{
    const float2 cell = floor(value);
    const float2 local = frac(value);
    const float2 smoothLocal = local * local * (3.0f - 2.0f * local);
    const float a = OceanHash(cell);
    const float b = OceanHash(cell + float2(1.0f, 0.0f));
    const float c = OceanHash(cell + float2(0.0f, 1.0f));
    const float d = OceanHash(cell + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, smoothLocal.x),
        lerp(c, d, smoothLocal.x), smoothLocal.y);
}

float OceanFoamFbm(float2 value)
{
    return OceanValueNoise(value) * 0.50f
        + OceanValueNoise(value * 2.03f + 11.7f) * 0.31f
        + OceanValueNoise(value * 4.11f - 7.3f) * 0.19f;
}

float3 OceanFoamStructure(float2 worldPosition)
{
    // Match the WaveWorks material's 0.04/0.15/0.30 world-space density
    // octaves with project-owned procedural data. A low-frequency warp keeps
    // the masks non-periodic without imposing one mechanical stripe direction.
    const float2 warp = float2(
        OceanFoamFbm(worldPosition * 0.013f + 3.1f),
        OceanFoamFbm(worldPosition * 0.017f - 5.7f)) - 0.5f;
    const float lowFrequency = OceanFoamFbm(
        worldPosition * 0.04f + warp * 0.55f);
    const float highFrequency = OceanFoamFbm(
        worldPosition * 0.15f + warp * 1.10f + 17.0f);
    const float veryHighFrequency = OceanFoamFbm(
        worldPosition * 0.30f + warp * 1.85f - 23.0f);
    return float3(lowFrequency, highFrequency, veryHighFrequency);
}

float OceanFoamBubbleCoverage(
    float2 worldPosition,
    float3 densityStructure)
{
    // WaveWorks thresholds a filled bubble texture; it does not extract the
    // texture's isolines.  Domain-warped, filled clusters are the clean-room
    // procedural equivalent.  Keeping the two scales continuous avoids the
    // equal-width closed white contours produced by the previous wall mask.
    // Reuse the already evaluated density octaves as the warp. This keeps the
    // full-screen ocean path close to the previous procedural sample count.
    const float2 warp = densityStructure.xz - 0.5f;
    const float coarseBubbles = OceanFoamFbm(
        worldPosition * 0.25f + warp * 1.45f
            + float2(41.7f, -19.3f));
    const float fineBubbles = OceanFoamFbm(
        worldPosition * 0.95f - warp * 2.10f
            + float2(-8.1f, 27.4f));
    const float microPorosity = OceanValueNoise(
        worldPosition * 2.40f + warp * 3.20f + 7.9f);
    const float filledClusters = smoothstep(
        0.52f, 0.78f, coarseBubbles * 0.68f + fineBubbles * 0.32f);
    return saturate(filledClusters * lerp(0.62f, 1.0f, microPorosity));
}

float EvaluateOceanFoamLayers(
    float2 worldPosition,
    float persistentEnergy,
    float surfaceFolding,
    float waveHats,
    out float thinFoam,
    out float thickFoam,
    out float rimFoam)
{
    const float3 foamStructure = OceanFoamStructure(worldPosition);
    const float bubbleCoverage = OceanFoamBubbleCoverage(
        worldPosition, foamStructure);
    const float energy = saturate(persistentEnergy);
    // Match the reference ordering: turbulent energy first establishes a
    // continuous foam sheet, folding thickens it, and bubble coverage only
    // breaks up the optical result.  Noise cannot create foam by itself.
    const float historyGate = smoothstep(0.025f, 0.22f, energy);
    const float energyDrive = energy * 2.25f;
    const float highDensity = saturate(
        (foamStructure.y - 0.72f) * 1.15f
        + max(energyDrive - 0.20f, 0.0f));
    const float lowDensity = max(0.0f,
        (foamStructure.x - 0.68f) * 0.85f
        + min(0.78f, energy * 1.35f));
    const float breakingDensity = max(0.0f,
        (foamStructure.z - 0.48f) * 1.75f * saturate(waveHats));
    float continuousFoam = (highDensity + lowDensity) * historyGate
        + breakingDensity;
    continuousFoam *= 1.0f + 0.8f * saturate(surfaceFolding);
    continuousFoam = pow(saturate(continuousFoam), 0.72f);

    // A low-energy wet film remains continuous.  Dense aerated foam receives
    // the stronger filled-bubble modulation.  This produces sheets with
    // holes and age variation rather than an outlined cellular mesh.
    thinFoam = saturate(continuousFoam
        * lerp(0.32f, 0.82f, bubbleCoverage)
        * smoothstep(0.02f, 0.34f, energy + waveHats * 0.22f));
    thickFoam = saturate(smoothstep(0.46f, 0.90f, continuousFoam)
        * smoothstep(0.18f, 0.74f, bubbleCoverage)
        * (0.68f + 0.32f * saturate(surfaceFolding)));
    // rimFoam now represents damp, ageing film between the aerated clusters;
    // it deliberately contains no screen-space derivative outline.
    rimFoam = saturate(continuousFoam - thickFoam)
        * (1.0f - bubbleCoverage) * 0.36f;
    return saturate(thinFoam * 0.72f + thickFoam);
}

float OceanShadingFlag(uint bit)
{
    return OceanFlag(drawInstanceParams.y, bit);
}

float OceanDebugFlag(uint bit)
{
    return OceanFlag(drawInstanceParams.w, bit);
}

float3 EvaluateOceanDirectionalSpecular(
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float3 lightRadiance,
    float roughness,
    float useFresnel)
{
    const float3 halfVector = normalize(viewDirection + lightDirection);
    const float ndotl = saturate(dot(normal, lightDirection));
    const float ndotv = saturate(dot(normal, viewDirection));
    const float hdotv = saturate(dot(halfVector, viewDirection));
    const float distribution = DistributionGGX(
        normal, halfVector, roughness);
    const float masking = GeometrySmith(
        normal, viewDirection, lightDirection, roughness);
    const float3 fresnel = useFresnel > 0.5f
        ? FresnelSchlick(hdotv, float3(0.020f, 0.035f, 0.045f))
        : float3(1.0f, 1.0f, 1.0f);
    const float3 radiance = lightRadiance * ndotl;
    return distribution * masking * fresnel
        / max(4.0f * ndotv * ndotl, 0.0001f)
        * radiance;
}

float3 EvaluateOceanMomentSpecular(
    VSOutput input,
    float3 normal,
    float foam,
    float useFresnel,
    out float momentVariance)
{
    const float spectralPeriod = max(renderFeatureParams.y, 1.0f);
    float2 meanSlope = 0.0.xx;
    float2 secondSlope = 0.0.xx;
    const float2 undisplacedWorldPosition = input.texCoord * spectralPeriod;
    const float cameraDistance = length(
        undisplacedWorldPosition - cameraPosition.xz);
    [unroll]
    for (uint cascade = 0u; cascade < 4u; ++cascade)
    {
        if (OceanStageCascadeEnabled(
                drawInstanceParams.w,
                OceanMomentsCascadeFirstBit,
                cascade) < 0.5f)
        {
            continue;
        }
        const float patchLength =
            GetOceanCascadePatchLength(spectralPeriod, cascade);
        const float weight = GetOceanCascadeDistanceWeight(
            cameraDistance, patchLength, spectralPeriod, cascade);
        const float2 uv = GetOceanCascadeUv(
            input.texCoord * spectralPeriod,
            patchLength,
            cascade,
            renderFeatureParams4.x,
            renderFeatureParams4.y);
        const float2 dx = ddx(uv) * max(renderFeatureParams.z, 1.0f);
        const float2 dy = ddy(uv) * max(renderFeatureParams.z, 1.0f);
        const float mip =
            0.5f * log2(max(max(dot(dx, dx), dot(dy, dy)), 1.0f))
                + 0.25f;
        const float4 moments = spectralOceanMoments.SampleLevel(
            linearWrapSampler,
            float3(uv, float(GetOceanCascadeLayer(cascade))),
            mip);
        meanSlope += weight * moments.xy;
        secondSlope += weight * moments.zw;
    }
    // Each cascade owns a different wavelength band. Their first and second
    // moments therefore compose additively; normalizing by the number of
    // active cascades made the sun lobe artificially broad and dim.
    momentVariance = max(
        0.0f,
        secondSlope.x - meanSlope.x * meanSlope.x
            + secondSlope.y - meanSlope.y * meanSlope.y);
    const float roughness = clamp(
        0.035f + sqrt(momentVariance) * 1.35f + foam * 0.20f,
        0.035f,
        0.75f);
    const float3 viewDirection = normalize(
        cameraPosition - input.worldPosition);
    float3 specular = EvaluateOceanDirectionalSpecular(
        normal,
        viewDirection,
        normalize(-directionalLightDirection),
        directionalLightColor * directionalLightIntensity,
        roughness,
        useFresnel);
    [unroll]
    for (uint auxiliaryIndex = 0u;
         auxiliaryIndex < 2u;
         ++auxiliaryIndex)
    {
        const DirectionalLightData auxiliaryLight =
            auxiliaryDirectionalLights[auxiliaryIndex];
        if (auxiliaryLight.intensity <= 0.0f)
        {
            continue;
        }
        specular += EvaluateOceanDirectionalSpecular(
            normal,
            viewDirection,
            normalize(-auxiliaryLight.direction),
            auxiliaryLight.color * auxiliaryLight.intensity,
            roughness,
            useFresnel);
    }
    return specular;
}

float3 EvaluateOceanInWaterScattering(
    float3 normal,
    float3 viewDirection)
{
    const float waterFacing = saturate(normal.y * 0.5f + 0.5f);
    const float grazing = pow(
        1.0f - saturate(dot(normal, viewDirection)), 2.0f);
    // This is a low-frequency transmitted-light approximation, not diffuse
    // paint. Keep it strong enough to preserve readable trough midtones while
    // reflections and moment-filtered highlights remain the dominant cues.
    return max(oceanScatteringColor, 0.0f.xxx)
        * max(oceanScatteringStrength, 1.0f)
        * lerp(0.045f, 0.080f, waterFacing)
        * lerp(1.0f, 1.14f, grazing);
}

VSOutput BuildDedicatedOceanVertexOutput(VSInput input)
{
    VSOutput output;
    output.oceanLod = 0.0f;
    float3 localPosition = input.position;
    const float oceanPatchLength = max(renderFeatureParams.y, 1.0f);
    const float2 oceanCoordinate = localPosition.xz
        + renderFeatureParams2.xy;
    const float2 surfaceUv = oceanCoordinate / oceanPatchLength;
    if (renderFeatureParams2.w > 0.5f)
    {
        float3 displacement = 0.0f.xxx;
        const float cameraDistance = length(
            oceanCoordinate - cameraPosition.xz);
        [unroll]
        for (uint cascade = 0u; cascade < 4u; ++cascade)
        {
            if (OceanStageCascadeEnabled(
                    drawInstanceParams.y,
                    OceanDisplacementCascadeFirstBit,
                    cascade) < 0.5f)
            {
                continue;
            }
            const float patchLength = GetOceanCascadePatchLength(
                oceanPatchLength, cascade);
            const float2 uv = GetOceanCascadeUv(
                oceanCoordinate,
                patchLength,
                cascade,
                renderFeatureParams4.x,
                renderFeatureParams4.y);
            const float weight = GetOceanCascadeDistanceWeight(
                cameraDistance,
                patchLength,
                oceanPatchLength,
                cascade);
            // Adaptive tessellation already controls geometric sampling
            // density.  Like the WaveWorks domain shader, sample full
            // resolution displacement here so crests are not rounded by a
            // second, texture-mip LOD.  Pixel normals still use
            // derivative-aware mip selection below.
            const float mip = renderFeatureParams2.w > 1.5f
                ? 0.0f
                : GetOceanGeometryMip(
                    renderFeatureParams2.z,
                    renderFeatureParams.z,
                    patchLength);
            displacement += weight
                * spectralOceanDisplacement.SampleLevel(
                    linearWrapSampler,
                    float3(uv, float(GetOceanCascadeLayer(cascade))),
                    mip).xyz;
        }
        localPosition += displacement;
        if (renderFeatureParams3.w > 0.5f)
        {
            float2 localUv = 0.0f.xx;
            const float localWeight = GetOceanLocalDomainSample(
                oceanCoordinate,
                renderFeatureParams3.xy,
                renderFeatureParams3.z,
                localUv)
                * PRISM_OCEAN_LOCAL_DISTANCE_WEIGHT(oceanCoordinate);
            if (localWeight > 0.0f)
            {
                const float localMip = renderFeatureParams2.w > 1.5f
                    ? 0.0f
                    : GetOceanGeometryMip(
                        renderFeatureParams2.z,
                        renderFeatureParams.z,
                        renderFeatureParams3.z);
                localPosition += localWaveDisplacement.SampleLevel(
                    linearClampSampler, localUv, localMip).xyz
                    * localWeight;
            }
        }
    }
    output.position = mul(
        float4(localPosition, 1.0f), worldViewProjection);
    output.currentClipPosition = output.position;
    output.previousClipPosition = mul(
        float4(localPosition, 1.0f), previousWorldViewProjection);
    output.worldPosition = mul(
        float4(localPosition, 1.0f), world).xyz;
    output.worldNormal = normalize(mul(
        float4(input.normal, 0.0f), normalMatrix).xyz);
    float3 transformedTangent = normalize(mul(
        float4(input.tangent.xyz, 0.0f), world).xyz);
    transformedTangent = normalize(
        transformedTangent - output.worldNormal
            * dot(transformedTangent, output.worldNormal));
    output.worldTangent = float4(
        transformedTangent, input.tangent.w);
    output.texCoord = surfaceUv;
    output.vertexColor = input.color;
    output.shadowPosition = mul(
        float4(localPosition, 1.0f),
        mul(world, lightViewProjections[0]));
    return output;
}

VSOutput OceanVSMain(VSInput input)
{
    return BuildDedicatedOceanVertexOutput(input);
}

struct OceanPatchControlPoint
{
    // Keep the adaptive mesh undisplaced through the vertex/hull stages.
    // WaveWorks evaluates every newly tessellated point in the domain shader;
    // displacing here and interpolating there only smooths the original
    // triangle and cannot recover short-wavelength geometry.
    float3 localPosition : POSITION;
    float4 vertexColor : COLOR;
    float3 localNormal : NORMAL;
    float2 texCoord : TEXCOORD0;
    float4 tangent : TANGENT;
    float oceanLod : TEXCOORD1;
};

OceanPatchControlPoint OceanVSIndexedMain(
    VSInput input, uint instanceId : SV_InstanceID)
{
    // A value above one in renderFeatureParams2.w selects the adaptive patch
    // payload.  The default clipmap continues to use the ordinary indexed
    // object path, so the old FFT comparison scene is unaffected.
    VSInput patchInput = input;
    float patchLod = 0.0f;
    if (renderFeatureParams2.w > 1.5f)
    {
        const OceanPatchInstance patch = oceanPatchInstances[instanceId];
        float2 patchUv = input.position.xz * 0.5f + 0.5f;
        const float cells = max(renderFeatureParams4.w, 1.0f);
        float2 grid = round(patchUv * cells);
        const bool oddX = fmod(grid.x, 2.0f) > 0.5f;
        const bool oddY = fmod(grid.y, 2.0f) > 0.5f;
        if (OceanFlag(patch.morphEdgeLod.y, 0u) > 0.5f
            && grid.y < 0.5f && oddX)
            grid.x -= 1.0f;
        if (OceanFlag(patch.morphEdgeLod.y, 1u) > 0.5f
            && grid.x > cells - 0.5f && oddY)
            grid.y -= 1.0f;
        if (OceanFlag(patch.morphEdgeLod.y, 2u) > 0.5f
            && grid.y > cells - 0.5f && oddX)
            grid.x -= 1.0f;
        if (OceanFlag(patch.morphEdgeLod.y, 3u) > 0.5f
            && grid.x < 0.5f && oddY)
            grid.y -= 1.0f;
        patchUv = grid / cells;
        const float2 coarseGrid = floor(grid * 0.5f) * 2.0f;
        patchUv = lerp(patchUv, coarseGrid / cells,
            saturate(patch.morphEdgeLod.x * renderFeatureParams4.z));
        patchInput.position.xz = patch.centerHalfExtent.xy
            + (patchUv * 2.0f - 1.0f)
                * patch.centerHalfExtent.z;
        patchLod = patch.morphEdgeLod.z;
    }
    OceanPatchControlPoint output;
    output.localPosition = patchInput.position;
    output.vertexColor = patchInput.color;
    output.localNormal = patchInput.normal;
    output.texCoord = patchInput.texCoord;
    output.tangent = patchInput.tangent;
    output.oceanLod = patchLod;
    return output;
}

struct OceanSurfaceTessellationFactors
{
    float edge[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

OceanSurfaceTessellationFactors OceanSurfacePatchConstants(
    InputPatch<OceanPatchControlPoint, 3> patch)
{
    OceanSurfaceTessellationFactors factors;
    // The CPU quadtree already refines a 64x64 grid until its projected cell
    // edge meets maximumEdgeLengthPixels. Applying another factor of up to
    // eight to every cell duplicated that work and generated millions of
    // redundant triangles. Keep the hull/domain route available for backend
    // parity while the quadtree remains the single geometry-density owner.
    factors.edge[0] = 1.0f;
    factors.edge[1] = 1.0f;
    factors.edge[2] = 1.0f;
    factors.inside = 1.0f;
    return factors;
}

[shader("hull")]
[domain("tri")]
[outputtopology("triangle_cw")]
[partitioning("fractional_odd")]
[patchconstantfunc("OceanSurfacePatchConstants")]
[outputcontrolpoints(3)]
OceanPatchControlPoint OceanHullMain(
    InputPatch<OceanPatchControlPoint, 3> patch,
    uint controlPoint : SV_OutputControlPointID)
{
    return patch[controlPoint];
}

[shader("domain")]
[domain("tri")]
[partitioning("fractional_odd")]
VSOutput OceanDomainMain(
    OceanSurfaceTessellationFactors factors,
    const OutputPatch<OceanPatchControlPoint, 3> patch,
    float3 barycentric : SV_DomainLocation)
{
    VSInput tessellatedInput;
    tessellatedInput.position = patch[0].localPosition * barycentric.x
        + patch[1].localPosition * barycentric.y
        + patch[2].localPosition * barycentric.z;
    tessellatedInput.color = patch[0].vertexColor * barycentric.x
        + patch[1].vertexColor * barycentric.y
        + patch[2].vertexColor * barycentric.z;
    tessellatedInput.normal = normalize(
        patch[0].localNormal * barycentric.x
        + patch[1].localNormal * barycentric.y
        + patch[2].localNormal * barycentric.z);
    tessellatedInput.texCoord = patch[0].texCoord * barycentric.x
        + patch[1].texCoord * barycentric.y
        + patch[2].texCoord * barycentric.z;
    tessellatedInput.tangent = patch[0].tangent * barycentric.x
        + patch[1].tangent * barycentric.y
        + patch[2].tangent * barycentric.z;

    // BuildDedicatedOceanVertexOutput computes UVs from undisplaced world XZ
    // and samples all four cascades plus the local domain. Calling it here is
    // the essential WaveWorks ordering: interpolate, then displace.
    VSOutput output = BuildDedicatedOceanVertexOutput(tessellatedInput);
    output.oceanLod = patch[0].oceanLod;
    return output;
}

float4 OceanPSMain(VSOutput input) : SV_TARGET
{
    // The ocean path intentionally does not call generic PSMain: water has no
    // albedo/metallic/occlusion/emissive material textures, clustered point
    // lights, or alpha test. Avoiding those samples and branches is critical
    // at full-screen ocean coverage.
    float4 color = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (renderFeatureParams2.w > 0.5f)
    {
        float persistentEnergy = 0.0f;
        float surfaceFolding = 0.0f;
        float waveHats = 0.0f;
        const float4 normalFoam = SampleOceanNormalFoam(
            input, persistentEnergy, surfaceFolding, waveHats);
        const float3 normal = BuildOceanSurfaceNormal(
            input,
            normalFoam.xyz * 2.0f - 1.0f);
        const float3 viewDirection = normalize(
            cameraPosition - input.worldPosition);
        // Foam texture detail is anchored to the undisplaced surface domain.
        // Sampling it from displaced XZ shears the mask twice and makes the
        // pattern swim and bulge around rounded wave geometry.
        const float2 worldCell = input.texCoord
            * max(renderFeatureParams.y, 1.0f);
        // History is turbulent-energy coverage. Split it into a translucent
        // film, an opaque aerated core, and derivative-localized bubble rims
        // before evaluating material roughness and lighting.
        float thinFoam = 0.0f;
        float thickFoam = 0.0f;
        float rimFoam = 0.0f;
        const float opticalFoam = EvaluateOceanFoamLayers(
            worldCell,
            persistentEnergy,
            surfaceFolding,
            waveHats,
            thinFoam,
            thickFoam,
            rimFoam);
        float3 albedo = 0.0f.xxx;
        float metallicValue = 0.0f;
        float roughnessValue = 0.1f;
        float occlusionValue = 1.0f;
        float alphaValue = 1.0f;
        ApplyOceanOpticalMaterial(
            input.worldPosition,
            normal,
            0.0f,
            albedo,
            metallicValue,
            roughnessValue,
            occlusionValue,
            alphaValue);
        const float breakingActivity = saturate(
            waveHats * 0.85f + surfaceFolding * 0.35f);
        const float foamRoughnessCoverage = saturate(
            thinFoam * 0.34f + thickFoam * 0.72f);
        roughnessValue = lerp(
            roughnessValue, 0.52f, foamRoughnessCoverage);
        color.rgb = albedo * max(ambientIntensity, 0.0f) * 0.35f;
        if (iblEnabled > 0.5f
            && OceanShadingFlag(2u) > 0.5f)
        {
            color.rgb += EvaluateIbl(
                normal,
                viewDirection,
                albedo,
                metallicValue,
                roughnessValue,
                true) * occlusionValue;
        }
        color.rgb += EvaluateOceanInWaterScattering(
            normal, viewDirection);
        // Turbulent energy represents entrained bubbles, but it is not itself
        // optical coverage. Gate the subsurface brightening by the same
        // density mask as the visible foam; otherwise a smooth cyan sheet
        // exposes the complete advection/history field around a wake.
        color.rgb += lerp(
            max(oceanScatteringColor, 0.0f.xxx),
            max(oceanFoamColor, 0.0f.xxx),
            0.30f)
            * saturate(persistentEnergy * opticalFoam * 0.08f);
        float momentVariance = 0.0f;
        if (OceanShadingFlag(1u) > 0.5f)
        {
            color.rgb += EvaluateOceanMomentSpecular(
                input,
                normal,
                opticalFoam,
                OceanShadingFlag(0u),
                momentVariance)
                * (0.85f * max(drawInstanceParams.z, 0.0f))
                * ComputeShadowFactor(input.worldPosition);
        }
        else
        {
            // Still evaluate the moments for debug views and reflection LOD
            // when the specular lobe itself is disabled.
            float ignoredVariance = 0.0f;
            EvaluateOceanMomentSpecular(
                input,
                normal,
                opticalFoam,
                OceanShadingFlag(0u),
                ignoredVariance);
            momentVariance = ignoredVariance;
        }
        // Thin aerated water retains a blue-green tint; only the high-energy
        // core approaches the configured dry-foam color. Moment variance
        // mildly boosts visibility on rough crests without generating energy.
        const float momentVisibility = 0.88f
            + 0.12f * saturate(sqrt(momentVariance) * 3.0f);
        const float3 thinFoamColor = lerp(
            max(oceanScatteringColor, 0.0f.xxx),
            max(oceanFoamColor, 0.0f.xxx),
            0.38f);
        const float3 agedFoamColor = lerp(
            max(oceanScatteringColor, 0.0f.xxx),
            max(oceanFoamColor, 0.0f.xxx),
            0.62f);
        const float3 thickFoamColor = lerp(
            agedFoamColor,
            max(oceanFoamColor, 0.0f.xxx),
            breakingActivity * 0.55f);
        color.rgb = lerp(
            color.rgb,
            thinFoamColor,
            saturate((thinFoam * 0.34f + rimFoam * 0.18f)
                * momentVisibility
                * lerp(0.68f, 1.0f, breakingActivity)));
        color.rgb = lerp(
            color.rgb,
            thickFoamColor,
            saturate(thickFoam * 0.44f * momentVisibility
                * lerp(0.78f, 1.0f, breakingActivity)));

        // Debug views intentionally run after normal lighting and never
        // mutate simulation resources.  They are mutually composable, so a
        // capture can show cascade contribution plus foam or moments.
        if (OceanDebugFlag(0u) > 0.5f)
        {
            const float debugPeriod = max(renderFeatureParams.y, 1.0f);
            const float2 debugUndisplacedWorldPosition =
                input.texCoord * debugPeriod;
            const float cameraDistance = length(
                debugUndisplacedWorldPosition - cameraPosition.xz);
            const float4 cascadeColors[4] = {
                float4(0.95f, 0.20f, 0.18f, 1.0f),
                float4(0.20f, 0.85f, 0.30f, 1.0f),
                float4(0.20f, 0.45f, 1.0f, 1.0f),
                float4(0.85f, 0.25f, 0.95f, 1.0f)};
            float4 cascadeColor = 0.0f.xxxx;
            float cascadeWeight = 0.0f;
            float cascadeBoundary = 0.0f;
            [unroll]
            for (uint cascade = 0u; cascade < 4u; ++cascade)
            {
                const float debugPatchLength =
                    GetOceanCascadePatchLength(debugPeriod, cascade);
                const float weight = GetOceanCascadeDistanceWeight(
                    cameraDistance, debugPatchLength, debugPeriod, cascade);
                cascadeColor += weight * cascadeColors[cascade];
                cascadeWeight += weight;
                const float2 cell = input.worldPosition.xz
                    / debugPatchLength;
                const float2 edgeDistance = min(
                    frac(cell), 1.0f - frac(cell));
                const float edge = 1.0f - saturate(
                    min(edgeDistance.x, edgeDistance.y) * 28.0f);
                cascadeBoundary = max(
                    cascadeBoundary,
                    edge * saturate(weight * 1.8f));
            }
            color.rgb = lerp(
                color.rgb,
                cascadeColor.rgb / max(cascadeWeight, 0.001f),
                0.62f);
            color.rgb = lerp(
                color.rgb,
                float3(1.0f, 0.92f, 0.35f),
                cascadeBoundary * 0.82f);
        }
        if (OceanDebugFlag(1u) > 0.5f)
        {
            // A diagnostic view must expose the simulation carrier itself;
            // blending white over the lit ocean made small energy values look
            // like sun reflection and concealed over-generation. Black is no
            // foam, amber is the generation band, white is saturated history.
            color.rgb = float3(
                saturate(persistentEnergy),
                saturate(surfaceFolding),
                saturate(waveHats));
        }
        if (OceanDebugFlag(2u) > 0.5f)
        {
            const float momentHeat = saturate(sqrt(momentVariance) * 4.0f);
            color.rgb = lerp(
                color.rgb,
                float3(momentHeat, momentHeat * 0.35f, 1.0f - momentHeat),
                0.72f);
        }
        if (OceanDebugFlag(3u) > 0.5f)
        {
            const float lod = saturate(input.oceanLod / 12.0f);
            color.rgb = lerp(
                color.rgb,
                float3(lod, 1.0f - lod, 0.15f),
                0.55f);
        }
        if (OceanDebugFlag(4u) > 0.5f)
        {
            color.rgb = lerp(
                color.rgb,
                normal * 0.5f + 0.5f,
                0.82f);
        }
    }
    return float4(max(color.rgb, 0.0f.xxx), color.a);
}

GBufferOutput OceanGBufferPS(VSOutput input)
{
    // Independent deferred material encoding: no generic mesh textures or
    // alpha/clustered-light paths are evaluated for water.
    GBufferOutput output;
    float persistentEnergy = 0.0f;
    float surfaceFolding = 0.0f;
    float waveHats = 0.0f;
    const float4 normalFoam = SampleOceanNormalFoam(
        input, persistentEnergy, surfaceFolding, waveHats);
    const float3 normal = BuildOceanSurfaceNormal(
        input, normalFoam.xyz * 2.0f - 1.0f);
    float thinFoam = 0.0f;
    float thickFoam = 0.0f;
    float rimFoam = 0.0f;
    const float opticalFoam = EvaluateOceanFoamLayers(
        input.texCoord * max(renderFeatureParams.y, 1.0f),
        persistentEnergy,
        surfaceFolding,
        waveHats,
        thinFoam,
        thickFoam,
        rimFoam);
    float3 albedo = 0.0f.xxx;
    float metallicValue = 0.0f;
    float roughnessValue = 0.1f;
    float occlusionValue = 1.0f;
    float alphaValue = 1.0f;
    ApplyOceanOpticalMaterial(
        input.worldPosition,
        normal,
        opticalFoam,
        albedo,
        metallicValue,
        roughnessValue,
        occlusionValue,
        alphaValue);
    const float3 viewDirection = normalize(
        cameraPosition - input.worldPosition);
    const float3 scattering = EvaluateOceanInWaterScattering(
        normal, viewDirection);
    output.worldPositionRoughness = float4(
        input.worldPosition, roughnessValue);
    output.normalMetallic = float4(
        normalize(normal) * 0.5f + 0.5f, metallicValue);
    output.albedoOcclusion = float4(albedo, occlusionValue);
    output.emissiveAlpha = float4(scattering, alphaValue);
    const float2 currentNdc = input.currentClipPosition.xy
        / max(input.currentClipPosition.w, 0.0001f);
    const float2 previousNdc = input.previousClipPosition.xy
        / max(input.previousClipPosition.w, 0.0001f);
    output.motionVector = (currentNdc - previousNdc)
        * float2(0.5f, -0.5f);
    return output;
}

struct WaterVisibilityOutput
{
    float4 normalRoughness : SV_Target0;
    float4 absorptionFoam : SV_Target1;
    float4 scatteringMask : SV_Target2;
    float2 motionVector : SV_Target3;
};

WaterVisibilityOutput WaterVisibilityPS(VSOutput input)
{
    WaterVisibilityOutput output;
    float persistentEnergy = 0.0f;
    float surfaceFolding = 0.0f;
    float waveHats = 0.0f;
    const float4 normalFoam = SampleOceanNormalFoam(
        input, persistentEnergy, surfaceFolding, waveHats);
    const float3 reconstructedNormal = BuildOceanSurfaceNormal(
        input, normalFoam.xyz * 2.0f - 1.0f);
    const float inverseNormalLength = rsqrt(max(
        dot(reconstructedNormal, reconstructedNormal), 1.0e-8f));
    const float3 normal = reconstructedNormal * inverseNormalLength;

    float thinFoam = 0.0f;
    float thickFoam = 0.0f;
    float rimFoam = 0.0f;
    const float foam = EvaluateOceanFoamLayers(
        input.texCoord * max(renderFeatureParams.y, 1.0f),
        persistentEnergy,
        surfaceFolding,
        waveHats,
        thinFoam,
        thickFoam,
        rimFoam);

    const float3 absorptionPerMeter =
        max(waterAbsorptionRoughness.xyz, 0.0f.xxx);
    const float3 scatteringPerMeter =
        max(waterScatteringIor.xyz, 0.0f.xxx);
    const float roughness = clamp(
        waterAbsorptionRoughness.w
            + saturate(surfaceFolding) * 0.12f + foam * 0.28f,
        0.001f,
        1.0f);
    output.normalRoughness = float4(
        saturate(normal * 0.5f + 0.5f), roughness);
    output.absorptionFoam = float4(
        saturate(absorptionPerMeter), saturate(foam));
    output.scatteringMask = float4(
        saturate(scatteringPerMeter), 1.0f);

    const float currentW = max(abs(input.currentClipPosition.w), 0.0001f);
    const float previousW = max(abs(input.previousClipPosition.w), 0.0001f);
    const float2 currentNdc = input.currentClipPosition.xy / currentW;
    const float2 previousNdc = input.previousClipPosition.xy / previousW;
    output.motionVector = clamp(
        (currentNdc - previousNdc) * float2(0.5f, -0.5f),
        -1.0f.xx,
        1.0f.xx);
    return output;
}
