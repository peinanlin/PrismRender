#include "ShaderBindings.hlsli"

struct PointLight
{
    float3 position;
    float range;
    float3 color;
    float intensity;
};

struct DirectionalLightData
{
    float3 direction;
    float intensity;
    float3 color;
    float padding;
};

PRISM_VK_BINDING(0) cbuffer FrameConstants : register(b0)
{
    float3 cameraPosition;
    float pointLightCount;
    float3 directionalLightDirection;
    float directionalLightIntensity;
    float3 directionalLightColor;
    float ambientIntensity;
    PointLight pointLights[4];
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
    DirectionalLightData auxiliaryDirectionalLights[2];
#if defined(PRISM_OCEAN_CASCADE_METADATA)
    float4 oceanCascadePatchLengths;
    float4 oceanCascadeLowerWavelengths;
    float4 oceanCascadeUpperWavelengths;
    float4 oceanCascadeUvScales;
    float4 oceanCascadeUvOffsetX;
    float4 oceanCascadeUvOffsetY;
    float4 oceanCascadeFadeStarts;
    float4 oceanCascadeFadeEnds;
    float4 oceanCascadeLayerOrder;
#endif
};

struct ClusterPointLight
{
    float3 position;
    float range;
    float3 color;
    float intensity;
};

#if defined(PRISM_OCEAN_CASCADE_METADATA)
// The dedicated forward ocean path only evaluates directional lights. These
// constants keep unreachable generic clustered-light helpers well-formed.
static const float4 clusterDepthParameters = 0.0f.xxxx;
static const uint clusterTileCountX = 0u;
static const uint clusterTileCountY = 0u;
static const uint clusterDepthSliceCount = 0u;
static const uint clusteredLightCount = 0u;
#else
PRISM_VK_BINDING(3) cbuffer ClusterConstants : register(b3)
{
    float4x4 clusterWorldToView;
    float4 clusterViewportProjection;
    float4 clusterDepthParameters;
    uint clusterTileCountX;
    uint clusterTileCountY;
    uint clusterDepthSliceCount;
    uint clusteredLightCount;
};
#endif

PRISM_VK_BINDING(1) cbuffer ObjectConstants : register(b1)
{
    float4x4 world;
    float4x4 normalMatrix;
    float4x4 worldViewProjection;
    float4x4 previousWorldViewProjection;
    float4 renderFeatureParams;
    float4 renderFeatureParams2;
    float4 renderFeatureParams3;
    float4 renderFeatureParams4;
    float4 drawInstanceParams;
};

struct ObjectData
{
    float4x4 world;
    float4x4 normalMatrix;
    float4x4 worldViewProjection;
    float4x4 previousWorldViewProjection;
    float4 renderFeatureParams;
    float4 renderFeatureParams2;
    float4 renderFeatureParams3;
    float4 renderFeatureParams4;
    float4 drawInstanceParams;
};

#if defined(PRISM_OCEAN_CASCADE_METADATA)
// OceanSurface provides its optical material directly from FrameConstants.
// Keep compile-time fallbacks for the unreachable generic mesh helpers so
// Slang can parse this shared module without retaining the mesh material CBV.
static const float4 albedoColor = 1.0f.xxxx;
static const float3 emissiveColor = 0.0f.xxx;
static const float metallic = 0.0f;
static const float roughness = 0.5f;
static const float useAlbedoTexture = 0.0f;
static const float useMetallicRoughnessTexture = 0.0f;
static const float useNormalTexture = 0.0f;
static const float useOcclusionTexture = 0.0f;
static const float useEmissiveTexture = 0.0f;
static const float occlusionStrength = 1.0f;
static const float normalScale = 1.0f;
static const float emissiveStrength = 0.0f;
static const float alphaCutoff = 0.5f;
static const float alphaMode = 0.0f;
#else
PRISM_VK_BINDING(2) cbuffer MaterialConstants : register(b2)
{
    float4 albedoColor;
    float3 emissiveColor;
    float metallic;
    float roughness;
    float useAlbedoTexture;
    float useMetallicRoughnessTexture;
    float useNormalTexture;
    float useOcclusionTexture;
    float useEmissiveTexture;
    float occlusionStrength;
    float normalScale;
    float emissiveStrength;
    float alphaCutoff;
    float alphaMode;
};
#endif

#if defined(PRISM_OCEAN_CASCADE_METADATA)
static const uint terrainPageTable[256] = {0u};
#else
PRISM_VK_BINDING(4) cbuffer TerrainVirtualPageTable : register(b4)
{
    uint terrainPageTable[256];
};
#endif

#if !defined(PRISM_OCEAN_CASCADE_METADATA)
PRISM_VK_BINDING(16) Texture2D albedoTexture : register(t0);
PRISM_VK_BINDING(17) Texture2D metallicRoughnessTexture : register(t1);
PRISM_VK_BINDING(18) Texture2D normalTexture : register(t2);
PRISM_VK_BINDING(19) Texture2D occlusionTexture : register(t3);
PRISM_VK_BINDING(20) Texture2D emissiveTexture : register(t4);
#endif
PRISM_VK_BINDING(21) Texture2DArray shadowMap : register(t5);
PRISM_VK_BINDING(22) TextureCube environmentTexture : register(t6);
PRISM_VK_BINDING(23) TextureCube irradianceTexture : register(t7);
PRISM_VK_BINDING(24) TextureCubeArray prefilteredEnvironmentTexture : register(t8);
PRISM_VK_BINDING(25) Texture2D brdfLutTexture : register(t9);
#if !defined(PRISM_OCEAN_CASCADE_METADATA)
PRISM_VK_BINDING(26)
StructuredBuffer<ClusterPointLight> clusteredPointLights : register(t10);
PRISM_VK_BINDING(27)
StructuredBuffer<uint> clusterLightCounts : register(t11);
PRISM_VK_BINDING(28)
StructuredBuffer<uint> clusterLightIndices : register(t12);
#endif
PRISM_VK_BINDING(29)
Texture2DArray<float4> varianceShadowMoments : register(t13);
#if !defined(PRISM_OCEAN_CASCADE_METADATA)
PRISM_VK_BINDING(30)
Texture2D featureSurfaceTexture0 : register(t14);
PRISM_VK_BINDING(31)
Texture2D featureSurfaceTexture1 : register(t15);
PRISM_VK_BINDING(32)
StructuredBuffer<ObjectData> indexedObjectData : register(t16);
PRISM_VK_BINDING(33)
StructuredBuffer<uint> instanceObjectIndices : register(t17);
#endif
PRISM_VK_BINDING(34)
Texture2D atmosphereSkyViewLut : register(t18);
#if !defined(PRISM_OCEAN_CASCADE_METADATA)
PRISM_VK_BINDING(35)
Texture2D terrainHeightTexture : register(t19);
#endif
PRISM_VK_BINDING(36)
Texture2DArray<float4> spectralOceanDisplacement : register(t20);
PRISM_VK_BINDING(37)
Texture2DArray<float4> spectralOceanGradient : register(t21);
PRISM_VK_BINDING(38)
Texture2DArray<float4> spectralOceanMoments : register(t22);
PRISM_VK_BINDING(39)
Texture2DArray<float4> spectralOceanFoam : register(t23);
PRISM_VK_BINDING(40)
Texture2D<float4> localWaveDisplacement : register(t24);
PRISM_VK_BINDING(41)
Texture2D<float4> localWaveGradient : register(t25);
PRISM_VK_BINDING(48) SamplerState linearWrapSampler : register(s0);
PRISM_VK_BINDING(49) SamplerComparisonState shadowSampler : register(s1);
PRISM_VK_BINDING(50) SamplerState linearClampSampler : register(s2);

#if defined(PRISM_OCEAN_CASCADE_METADATA)
// Generic mesh entry points are not exported by OceanSurface, but their
// helper bodies still have to parse. Alias unused 2D material/terrain inputs
// to the already-bound atmosphere LUT without adding ocean descriptors.
#define albedoTexture atmosphereSkyViewLut
#define metallicRoughnessTexture atmosphereSkyViewLut
#define normalTexture atmosphereSkyViewLut
#define occlusionTexture atmosphereSkyViewLut
#define emissiveTexture atmosphereSkyViewLut
#define featureSurfaceTexture0 atmosphereSkyViewLut
#define featureSurfaceTexture1 atmosphereSkyViewLut
#define terrainHeightTexture atmosphereSkyViewLut
#endif

#include "ShadowFiltering.hlsli"

struct VSInput
{
    float3 position : POSITION;
    float4 color : COLOR;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD;
    float4 tangent : TANGENT;
};

struct VSInstancedInput
{
    float3 position : POSITION;
    float4 color : COLOR;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD;
    float4 tangent : TANGENT;
    float4 instanceWorld0 : INSTANCEWORLD0;
    float4 instanceWorld1 : INSTANCEWORLD1;
    float4 instanceWorld2 : INSTANCEWORLD2;
    float4 instanceWorld3 : INSTANCEWORLD3;
    float4 instanceWvp0 : INSTANCEWVP0;
    float4 instanceWvp1 : INSTANCEWVP1;
    float4 instanceWvp2 : INSTANCEWVP2;
    float4 instanceWvp3 : INSTANCEWVP3;
    float4 instancePreviousWvp0 : INSTANCEPREVWVP0;
    float4 instancePreviousWvp1 : INSTANCEPREVWVP1;
    float4 instancePreviousWvp2 : INSTANCEPREVWVP2;
    float4 instancePreviousWvp3 : INSTANCEPREVWVP3;
};

// Must match OceanRuntimeFlagLayout. The two packed integers are represented
// as numeric floats in the existing object constant payload and remain below
// the exact 24-bit integer range on every backend.
static const uint OceanDisplacementCascadeFirstBit = 4u;
static const uint OceanGradientCascadeFirstBit = 8u;
static const uint OceanFoldingCascadeFirstBit = 5u;
static const uint OceanFoamHistoryCascadeFirstBit = 9u;
static const uint OceanMomentsCascadeFirstBit = 13u;

float OceanFlag(float packedFlags, uint bit)
{
    return fmod(floor(packedFlags / exp2(float(bit))), 2.0f);
}

float OceanStageCascadeEnabled(
    float packedFlags,
    uint firstBit,
    uint cascade)
{
#if defined(PRISM_OCEAN_CASCADE_METADATA)
    return OceanFlag(packedFlags, firstBit + min(cascade, 3u));
#else
    // The legacy FFT path predates the isolation payload and keeps its
    // original behavior. Only the dedicated spectral shader consumes masks.
    return 1.0f;
#endif
}

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
    float4 vertexColor : COLOR;
    float4 shadowPosition : TEXCOORD3;
    float4 worldTangent : TEXCOORD4;
    float4 currentClipPosition : TEXCOORD5;
    float4 previousClipPosition : TEXCOORD6;
    nointerpolation float oceanLod : TEXCOORD7;
};

float GetOceanCascadeDistanceWeight(
    float cameraDistance,
    float patchLength,
    float coarsestPatchLength,
    uint cascade)
{
#if defined(PRISM_OCEAN_CASCADE_METADATA)
    const float fadeStart = oceanCascadeFadeStarts[cascade];
    const float fadeEnd = oceanCascadeFadeEnds[cascade];
    if (fadeEnd <= fadeStart)
        return 1.0f;
    return 1.0f - smoothstep(
        fadeStart, fadeEnd, cameraDistance);
#else
    if (patchLength >= coarsestPatchLength * 0.999f)
        return 1.0f;
    // Keep fine wind-wave bands alive through the middle distance. The old
    // 3L--6L annulus removed the 15.625 m cascade by roughly 94 m, producing
    // a visible smooth band across the ocean. A long linear footprint fade is
    // stable under camera motion and matches the reference sample's ~30L
    // cascade visibility while the coarsest horizon band remains permanent.
    static const float OceanCascadeVisibilityPeriods = 30.0f;
    const float fadeDistance = max(
        patchLength * OceanCascadeVisibilityPeriods, 1.0f);
    return saturate(1.0f - cameraDistance / fadeDistance);
#endif
}

float GetOceanCascadePatchLength(
    float coarsestPatchLength,
    uint cascade)
{
#if defined(PRISM_OCEAN_CASCADE_METADATA)
    return max(oceanCascadePatchLengths[cascade], 1.0f);
#else
    // Four cascades are ordered fine-to-coarse at a 4x period ratio. The
    // coarsest period is runtime metadata, so changing Simulation Period no
    // longer leaves shader sampling pinned to the 1000 m reference preset.
    return max(coarsestPatchLength, 1.0f)
        * exp2((float(cascade) - 3.0f) * 2.0f);
#endif
}

uint GetOceanCascadeLayer(uint cascade)
{
#if defined(PRISM_OCEAN_CASCADE_METADATA)
    return min(uint(round(oceanCascadeLayerOrder[cascade])), 3u);
#else
    return cascade;
#endif
}

float GetOceanGeometryMip(
    float cellSizeMeters,
    float textureResolution,
    float patchLength)
{
    // A vertex grid cannot represent wavelengths below roughly one cell.
    // High frequencies remain available to derivative-aware pixel normals.
    return max(0.0f, log2(max(
        cellSizeMeters * textureResolution / max(patchLength, 1.0f),
        1.0f)));
}

float2 GetOceanCascadeUv(
    float2 undisplacedWorldPosition,
    float patchLength,
    uint cascade,
    float warpAmplitude,
    float warpFrequency)
{
#if defined(PRISM_OCEAN_CASCADE_METADATA)
    float2 uv = undisplacedWorldPosition
        * oceanCascadeUvScales[cascade]
        + float2(oceanCascadeUvOffsetX[cascade],
            oceanCascadeUvOffsetY[cascade]);
#else
    float2 uv = undisplacedWorldPosition / max(patchLength, 1.0f);
#endif
    if (warpAmplitude > 0.0f && warpFrequency > 0.0f)
    {
        // Warping is measured in this cascade's UV units. Applying it after
        // the mapping keeps 0.03 equal to 0.03 on every cascade.
        // Frequency is radians per cascade UV, matching the documented
        // WaveWorks parameter contract. Multiplying by 2*pi and normalizing
        // the offset created dense circular/fingerprint artifacts in the
        // near field. Both components use the original UV simultaneously.
        uv += warpAmplitude * float2(
            cos(uv.y * warpFrequency),
            sin(uv.x * warpFrequency));
    }
    return uv;
}

float GetOceanLocalDomainSample(
    float2 undisplacedWorldPosition,
    float2 domainCenter,
    float domainSize,
    out float2 localUv)
{
    localUv = (undisplacedWorldPosition - domainCenter)
        / max(domainSize, 1.0f) + 0.5f.xx;
    if (any(localUv < 0.0f.xx) || any(localUv > 1.0f.xx))
    {
        return 0.0f;
    }

    // Match the absorbing strip used by LocalWave.slang. One common lookup
    // keeps displacement, gradients and foam registered to the same domain
    // boundary, even after the vertex has been displaced.
    static const float OceanLocalBoundaryFraction = 0.12f;
    const float2 edgeDistance = min(localUv, 1.0f - localUv);
    return smoothstep(0.0f, OceanLocalBoundaryFraction,
        min(edgeDistance.x, edgeDistance.y));
}

#ifndef PRISM_OCEAN_LOCAL_DISTANCE_WEIGHT
#define PRISM_OCEAN_LOCAL_DISTANCE_WEIGHT(worldPosition) 1.0f
#endif

VSOutput BuildVertexOutput(
    VSInput input,
    ObjectData objectData)
{
    VSOutput output;
    output.oceanLod = 0.0f;
    float3 localPosition = input.position;
    float3 localNormal = input.normal;
    float2 surfaceUv = input.texCoord;
    float terrainRidge = input.color.r;
    if (objectData.renderFeatureParams.x > 0.5f
        && objectData.renderFeatureParams.x < 1.5f
        && objectData.renderFeatureParams3.w > 0.5f)
    {
        const float terrainWorldSize = max(
            objectData.renderFeatureParams.y,
            1.0f);
        surfaceUv = saturate(
            localPosition.xz / terrainWorldSize + 0.5f);
        const float4 terrainSample =
            terrainHeightTexture.SampleLevel(
                linearClampSampler,
                surfaceUv,
                0.0f);
        const float terrainHeightScale =
            objectData.renderFeatureParams3.z;
        localPosition.y +=
            objectData.renderFeatureParams3.y
            + terrainSample.x * terrainHeightScale;
        const float slopeScale =
            terrainHeightScale / terrainWorldSize;
        localNormal = normalize(float3(
            -terrainSample.y * slopeScale,
            1.0f,
            -terrainSample.z * slopeScale));
        terrainRidge = terrainSample.w;
    }
    if (objectData.renderFeatureParams.x > 1.5f
        && objectData.renderFeatureParams.x < 2.5f)
    {
        // Anchor the periodic FFT field in world space. The clipmap mesh may
        // snap to the camera without making the waves swim with the mesh.
        const float oceanPatchLength = max(
            objectData.renderFeatureParams.y,
            1.0f);
        const float2 oceanCoordinate = localPosition.xz
            + objectData.renderFeatureParams2.xy;
        surfaceUv = oceanCoordinate / oceanPatchLength;
        if (objectData.renderFeatureParams2.w > 0.5f)
        {
            float3 displacement = 0.0.xxx;
            const float cameraDistance = length(
                oceanCoordinate - cameraPosition.xz);
            [unroll]
            for (uint cascade = 0u; cascade < 4u; ++cascade)
            {
                if (OceanStageCascadeEnabled(
                        objectData.drawInstanceParams.y,
                        OceanDisplacementCascadeFirstBit,
                        cascade) < 0.5f)
                {
                    continue;
                }
                const float cascadePatchLength =
                    GetOceanCascadePatchLength(oceanPatchLength, cascade);
                const float2 cascadeUv = GetOceanCascadeUv(
                    oceanCoordinate,
                    cascadePatchLength,
                    cascade,
                    objectData.renderFeatureParams4.x,
                    objectData.renderFeatureParams4.y);
                const float cascadeWeight = GetOceanCascadeDistanceWeight(
                    cameraDistance,
                    cascadePatchLength,
                    oceanPatchLength,
                    cascade);
                const float displacementMip = GetOceanGeometryMip(
                    objectData.renderFeatureParams2.z,
                    objectData.renderFeatureParams.z,
                    cascadePatchLength);
                displacement += cascadeWeight
                    * spectralOceanDisplacement.SampleLevel(
                        linearWrapSampler,
                        float3(cascadeUv,
                            float(GetOceanCascadeLayer(cascade))),
                    displacementMip).xyz;
            }
            // The four textures contain separate frequency intervals. Their
            // displacement composes additively; averaging here flattened the
            // surface to one quarter of the intended WaveWorks-style energy.
            localPosition += displacement;
            if (objectData.renderFeatureParams3.w > 0.5f)
            {
                float2 localUv = 0.0f.xx;
                const float localDomainWeight = GetOceanLocalDomainSample(
                    oceanCoordinate,
                    objectData.renderFeatureParams3.xy,
                    objectData.renderFeatureParams3.z,
                    localUv)
                    * PRISM_OCEAN_LOCAL_DISTANCE_WEIGHT(oceanCoordinate);
                if (localDomainWeight > 0.0f)
                {
                    const float localDisplacementMip = GetOceanGeometryMip(
                        objectData.renderFeatureParams2.z,
                        objectData.renderFeatureParams.z,
                        objectData.renderFeatureParams3.z);
                    localPosition += localWaveDisplacement.SampleLevel(
                        linearClampSampler, localUv,
                        localDisplacementMip).xyz
                        * localDomainWeight;
                }
            }
        }
        else
        {
            const float4 displacement =
                featureSurfaceTexture0.SampleLevel(
                    linearWrapSampler,
                    surfaceUv,
                    objectData.renderFeatureParams2.z);
            localPosition += displacement.xyz;
        }
    }
    output.position = mul(
        float4(localPosition, 1.0f),
        objectData.worldViewProjection);
    output.currentClipPosition = output.position;
    output.previousClipPosition =
        mul(
            float4(localPosition, 1.0f),
            objectData.previousWorldViewProjection);
    output.worldPosition = mul(
        float4(localPosition, 1.0f),
        objectData.world).xyz;
    output.worldNormal = normalize(
        mul(
            float4(localNormal, 0.0f),
            objectData.normalMatrix).xyz);
    float3 transformedTangent = normalize(
        mul(
            float4(input.tangent.xyz, 0.0f),
            objectData.world).xyz);
    transformedTangent = normalize(
        transformedTangent
        - output.worldNormal
            * dot(transformedTangent, output.worldNormal));
    output.worldTangent = float4(
        transformedTangent,
        input.tangent.w);
    output.texCoord = surfaceUv;
    output.vertexColor = input.color;
    output.vertexColor.r = terrainRidge;
    output.shadowPosition = mul(
        float4(localPosition, 1.0f),
        mul(
            objectData.world,
            lightViewProjections[0]));
    return output;
}

VSOutput VSMain(VSInput input)
{
    ObjectData objectData;
    objectData.world = world;
    objectData.normalMatrix = normalMatrix;
    objectData.worldViewProjection =
        worldViewProjection;
    objectData.previousWorldViewProjection =
        previousWorldViewProjection;
    objectData.renderFeatureParams =
        renderFeatureParams;
    objectData.renderFeatureParams2 =
        renderFeatureParams2;
    objectData.renderFeatureParams3 =
        renderFeatureParams3;
    objectData.renderFeatureParams4 =
        renderFeatureParams4;
    objectData.drawInstanceParams =
        drawInstanceParams;
    return BuildVertexOutput(input, objectData);
}

#if !defined(PRISM_OCEAN_CASCADE_METADATA)
VSOutput VSIndexedMain(
    VSInput input,
    uint instanceId : SV_InstanceID)
{
    const uint objectIndex =
        instanceObjectIndices[
            uint(drawInstanceParams.x)
            + instanceId];
    return BuildVertexOutput(
        input,
        indexedObjectData[objectIndex]);
}
#endif

VSOutput VSInstancedMain(VSInstancedInput input)
{
    VSOutput output;
    float4 localPosition = float4(input.position, 1.0f);
    float4 localNormal = float4(input.normal, 0.0f);

    output.position = float4(
        dot(localPosition, input.instanceWvp0),
        dot(localPosition, input.instanceWvp1),
        dot(localPosition, input.instanceWvp2),
        dot(localPosition, input.instanceWvp3));
    output.currentClipPosition = output.position;
    output.previousClipPosition = float4(
        dot(localPosition, input.instancePreviousWvp0),
        dot(localPosition, input.instancePreviousWvp1),
        dot(localPosition, input.instancePreviousWvp2),
        dot(localPosition, input.instancePreviousWvp3));

    output.worldPosition = float3(
        dot(localPosition, input.instanceWorld0),
        dot(localPosition, input.instanceWorld1),
        dot(localPosition, input.instanceWorld2));

    output.worldNormal = normalize(float3(
        dot(localNormal, input.instanceWorld0),
        dot(localNormal, input.instanceWorld1),
        dot(localNormal, input.instanceWorld2)));
    output.worldTangent = float4(normalize(float3(
        dot(float4(input.tangent.xyz, 0.0f), input.instanceWorld0),
        dot(float4(input.tangent.xyz, 0.0f), input.instanceWorld1),
        dot(float4(input.tangent.xyz, 0.0f), input.instanceWorld2))), input.tangent.w);
    output.texCoord = input.texCoord;
    output.vertexColor = input.color;
    output.shadowPosition = mul(float4(output.worldPosition, 1.0f), lightViewProjections[0]);
    return output;
}

float ComputeShadowFactor(float3 worldPosition)
{
    if (shadowsEnabled < 0.5f)
    {
        return 1.0f;
    }

    const float viewDepth = GetDirectionalShadowViewDepth(
        worldPosition,
        cameraPosition,
        cameraForward);
    if (viewDepth > cascadeSplits.z)
    {
        return 1.0f;
    }

    const uint cascadeIndex =
        SelectDirectionalShadowCascade(
            viewDepth,
            cascadeSplits);
    float visibility = SampleDirectionalShadowCascade(
        shadowMap,
        shadowSampler,
        varianceShadowMoments,
        linearClampSampler,
        worldPosition,
        lightViewProjections[cascadeIndex],
        cascadeIndex,
        shadowBias);

    const float cascadeNear = cascadeIndex == 0u
        ? 0.0f
        : cascadeSplits[cascadeIndex - 1u];
    const float cascadeFar = cascadeSplits[cascadeIndex];
    const float blendWidth = max(
        (cascadeFar - cascadeNear)
            * saturate(cascadeBlendFraction),
        0.0001f);
    const float blend = saturate(
        (viewDepth - (cascadeFar - blendWidth))
        / blendWidth);
    if (blend > 0.0f)
    {
        const float nextVisibility = cascadeIndex < 2u
            ? SampleDirectionalShadowCascade(
                shadowMap,
                shadowSampler,
                varianceShadowMoments,
                linearClampSampler,
                worldPosition,
                lightViewProjections[cascadeIndex + 1u],
                cascadeIndex + 1u,
                shadowBias)
            : 1.0f;
        visibility = lerp(
            visibility,
            nextVisibility,
            blend);
    }
    return lerp(0.25f, 1.0f, visibility);
}

static const float PI = 3.14159265f;

float DistributionGGX(float3 N, float3 H, float roughnessValue)
{
    float a = roughnessValue * roughnessValue;
    float a2 = a * a;
    float NdotH = saturate(dot(N, H));
    float NdotH2 = NdotH * NdotH;
    float denominator = NdotH2 * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * denominator * denominator, 0.0001f);
}

float GeometrySchlickGGX(float NdotV, float roughnessValue)
{
    float r = roughnessValue + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotV / max(NdotV * (1.0f - k) + k, 0.0001f);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughnessValue)
{
    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));
    float ggxV = GeometrySchlickGGX(NdotV, roughnessValue);
    float ggxL = GeometrySchlickGGX(NdotL, roughnessValue);
    return ggxV * ggxL;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(1.0f - saturate(cosTheta), 5.0f);
}

float3 ApplyAtmosphereBrightness(
    float3 radiance,
    float brightness)
{
    // Treat values above one as exposure stops with a reduced slope, then
    // roll bright atmospheric scattering into a luminance shoulder. This
    // preserves a compact white sun while preventing a high UI brightness
    // value from turning the low-resolution Mie lobe into a hard-edged blob.
    const float safeBrightness = max(brightness, 0.0f);
    const float brightnessScale = safeBrightness <= 1.0f
        ? safeBrightness
        : 1.0f + log2(safeBrightness) * 0.45f;
    const float3 scaledRadiance =
        max(radiance, 0.0f.xxx) * brightnessScale;
    const float luminance = dot(
        scaledRadiance,
        float3(0.2126f, 0.7152f, 0.0722f));
    const float shoulderStart = 0.70f;
    const float shoulderRange = 0.85f;
    const float excess = max(luminance - shoulderStart, 0.0f);
    const float compressedLuminance = luminance <= shoulderStart
        ? luminance
        : shoulderStart
            + excess / (1.0f + excess / shoulderRange);
    return scaledRadiance
        * (compressedLuminance / max(luminance, 0.00001f));
}

float3 SampleAtmosphereSky(
    float2 uv,
    float brightness,
    float sunProximity)
{
    const float highBrightness = saturate(
        (brightness - 1.0f) / 3.0f);
    float3 radiance = atmosphereSkyViewLut.SampleLevel(
        linearClampSampler, uv, 0.0f).rgb;
    const float localMieCompression =
        saturate(sunProximity)
        * highBrightness;
    const float localLuminance = dot(
        radiance,
        float3(0.2126f, 0.7152f, 0.0722f));
    radiance /= 1.0f
        + localLuminance
            * localMieCompression
            * 7.5f;
    return ApplyAtmosphereBrightness(
        radiance,
        brightness);
}

float3 SrgbToLinear(float3 color)
{
    return pow(saturate(color), 2.2f);
}

float3 EvaluateDirectPbrLighting(
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float3 lightColor,
    float3 baseColor,
    float metallicValue,
    float roughnessValue)
{
    float3 halfVector = normalize(viewDirection + lightDirection);
    float NdotL = saturate(dot(normal, lightDirection));
    float NdotV = saturate(dot(normal, viewDirection));
    float HdotV = saturate(dot(halfVector, viewDirection));

    if (NdotL <= 0.0f || NdotV <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float clampedRoughness = clamp(roughnessValue, 0.08f, 1.0f);
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallicValue);
    float NDF = DistributionGGX(normal, halfVector, clampedRoughness);
    float G = GeometrySmith(normal, viewDirection, lightDirection, clampedRoughness);
    float3 F = FresnelSchlick(HdotV, F0);

    float3 numerator = NDF * G * F;
    float denominator = max(4.0f * NdotV * NdotL, 0.0001f);
    float3 specular = numerator / denominator;

    float3 kS = F;
    float3 kD = (1.0f.xxx - kS) * (1.0f - metallicValue);
    float3 diffuse = kD * baseColor / PI;
    return (diffuse + specular) * lightColor * NdotL;
}

float3 EvaluateDirectBlinnPhongLighting(
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float3 lightColor,
    float3 diffuseColor,
    float3 specularColor,
    float shininess)
{
    float NdotL = saturate(dot(normal, lightDirection));
    if (NdotL <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float3 halfVector = normalize(viewDirection + lightDirection);
    float specularPower = pow(saturate(dot(normal, halfVector)), max(shininess, 1.0f));
    float3 diffuse = diffuseColor * NdotL;
    float3 specular = specularColor * specularPower * NdotL;
    return (diffuse + specular) * lightColor;
}

float3 EvaluateIbl(
    float3 normal,
    float3 viewDirection,
    float3 baseColor,
    float metallicValue,
    float roughnessValue,
    bool isOceanSurface)
{
    static const float prefilterCubeCount = 4.0f;
    float3 reflectedDirection = reflect(-viewDirection, normal);
    float3 diffuseEnvironment = environmentTexture.SampleLevel(linearWrapSampler, normal, 0).rgb;

    // Water reflects roughly two percent of normally incident light.  Using
    // the generic dielectric 0.04 value made the entire ocean read as a pale
    // coated surface instead of deep water with a narrow grazing reflection.
    float3 F0 = isOceanSurface
        ? float3(0.020f, 0.020f, 0.020f)
        : lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallicValue);
    float NdotV = saturate(dot(normal, viewDirection));
    float3 diffuse = diffuseEnvironment * baseColor * (1.0f - metallicValue) * iblDiffuseStrength;
    float3 specularEnvironment = environmentTexture.SampleLevel(linearWrapSampler, reflectedDirection, 0).rgb;
    float3 specular = 0.0f.xxx;

    if (iblSplitSumEnabled > 0.5f)
    {
        diffuseEnvironment = irradianceTexture.SampleLevel(linearWrapSampler, normal, 0).rgb;
        diffuse = diffuseEnvironment * baseColor * (1.0f - metallicValue) * iblDiffuseStrength;

        float roughnessFactor = saturate(roughnessValue * roughnessValue);
        float reflectionBlend = saturate(iblReflectionBlend) * roughnessFactor;
        float3 blendedReflection = normalize(lerp(reflectedDirection, normal, reflectionBlend));
        float prefilterLevel = saturate(roughnessValue) * (prefilterCubeCount - 1.0f);
        float prefilterIndexLow = floor(prefilterLevel);
        float prefilterIndexHigh = min(prefilterCubeCount - 1.0f, prefilterIndexLow + 1.0f);
        float prefilterLerp = saturate(prefilterLevel - prefilterIndexLow);
        float3 prefilteredLow = prefilteredEnvironmentTexture.SampleLevel(linearWrapSampler, float4(blendedReflection, prefilterIndexLow), 0).rgb;
        float3 prefilteredHigh = prefilteredEnvironmentTexture.SampleLevel(linearWrapSampler, float4(blendedReflection, prefilterIndexHigh), 0).rgb;
        specularEnvironment = lerp(prefilteredLow, prefilteredHigh, prefilterLerp);
        if (isOceanSurface
            && physicalAtmosphereEnabled > 0.5f)
        {
            // The Ocean Lab renders its sky from the live atmosphere LUT. A
            // startup/fallback cubemap can have a different horizon and sun
            // direction, which turns a low-roughness ocean into a large
            // left/right brightness split. Reflect the same sky that is
            // visible above the water while retaining the split-sum BRDF LUT.
            const float pi = 3.14159265f;
            const float3 sunDirection = normalize(
                -directionalLightDirection);
            const float3 atmosphereDirection = normalize(
                lerp(
                    reflectedDirection,
                    normal,
                    reflectionBlend * 0.35f));
            const float zenith = acos(clamp(
                atmosphereDirection.y,
                -1.0f,
                1.0f));
            const float2 directionHorizontal =
                atmosphereDirection.xz;
            const float2 sunHorizontal =
                sunDirection.xz;
            float relativeAzimuth = 0.0f;
            if (dot(directionHorizontal, directionHorizontal)
                    > 0.000001f
                && dot(sunHorizontal, sunHorizontal)
                    > 0.000001f)
            {
                relativeAzimuth = acos(clamp(
                    dot(
                        normalize(directionHorizontal),
                        normalize(sunHorizontal)),
                    -1.0f,
                    1.0f));
            }
            specularEnvironment = SampleAtmosphereSky(
                float2(
                    relativeAzimuth / pi,
                    zenith / pi),
                atmosphereBrightness,
                smoothstep(
                    0.99980f,
                    0.99998f,
                    dot(
                        atmosphereDirection,
                        sunDirection)));
        }
        float2 brdf = brdfLutTexture.SampleLevel(
            linearWrapSampler,
            float2(NdotV, clamp(roughnessValue, 0.0f, 1.0f)),
            0).rg;
        specular = specularEnvironment * (F0 * brdf.x + brdf.y) * iblSpecularStrength;
    }
    else
    {
        float3 fresnel = FresnelSchlick(NdotV, F0);
        float specularStrength = lerp(1.0f, 0.2f, clamp(roughnessValue, 0.0f, 1.0f));
        specular = specularEnvironment * fresnel * specularStrength * iblSpecularStrength;
    }

    return (diffuse + specular) * iblIntensity;
}

float3 BuildSurfaceNormal(VSOutput input)
{
    float3 normal = normalize(input.worldNormal);
    if (useNormalTexture < 0.5f || normalMappingEnabled < 0.5f)
    {
        return normal;
    }

    float3 tangent = normalize(input.worldTangent.xyz);
    tangent = normalize(tangent - normal * dot(tangent, normal));
    float3 bitangent = normalize(cross(normal, tangent)) * input.worldTangent.w;
    float3x3 tbn = float3x3(tangent, bitangent, normal);
    float3 sampledNormal = normalTexture.Sample(linearWrapSampler, input.texCoord).xyz * 2.0f - 1.0f;
    sampledNormal.xy *= normalScale;
    return normalize(mul(sampledNormal, tbn));
}

float GetOceanSurfaceMip(VSOutput input)
{
    const float textureResolution = max(
        renderFeatureParams.z,
        1.0);
    const float2 footprintX = ddx(input.texCoord)
        * textureResolution;
    const float2 footprintY = ddy(input.texCoord)
        * textureResolution;
    const float footprintSquared = max(
        dot(footprintX, footprintX),
        dot(footprintY, footprintY));
    const float screenMip = max(
        0.0,
        0.5 * log2(max(footprintSquared, 1.0)));
    return max(renderFeatureParams2.z, screenMip);
}

float3 BuildOceanSurfaceNormal(
    VSOutput input,
    float3 localOceanNormal)
{
    const float3 surfaceUp = normalize(input.worldNormal);
    float3 surfaceTangent = normalize(input.worldTangent.xyz);
    surfaceTangent = normalize(
        surfaceTangent
        - surfaceUp * dot(surfaceTangent, surfaceUp));
    const float3 surfaceForward = normalize(
        cross(surfaceTangent, surfaceUp))
        * input.worldTangent.w;
    return normalize(
        localOceanNormal.x * surfaceTangent
        + localOceanNormal.y * surfaceUp
        + localOceanNormal.z * surfaceForward);
}

float GetOceanWhitecapsThreshold(bool localWaves)
{
    const uint packedThresholds = (uint)round(renderFeatureParams.w);
    const uint encodedThreshold = localWaves
        ? ((packedThresholds >> 10u) & 0x3ffu)
        : (packedThresholds & 0x3ffu);
    return float(encodedThreshold) / 1023.0f;
}

float4 SampleOceanNormalFoam(
    VSOutput input,
    out float persistentEnergy,
    out float surfaceFolding,
    out float waveHats)
{
    persistentEnergy = 0.0f;
    surfaceFolding = 0.0f;
    waveHats = 0.0f;
    if (renderFeatureParams2.w > 0.5f)
    {
        const float spectralPeriod = max(renderFeatureParams.y, 1.0f);
        float2 combinedSlope = 0.0.xx;
        float foamEnergy = 0.0f;
        float localFoamCoverage = 0.0f;
        float foldingEnergy = 0.0f;
        float localFoldingEnergy = 0.0f;
        const float2 undisplacedWorldPosition =
            input.texCoord * spectralPeriod;
        const float cameraDistance = length(
            undisplacedWorldPosition - cameraPosition.xz);
        [unroll]
        for (uint cascade = 0u; cascade < 4u; ++cascade)
        {
            const float cascadePatchLength =
                GetOceanCascadePatchLength(spectralPeriod, cascade);
            const float textureResolution = max(
                renderFeatureParams.z, 1.0f);
            const float2 cascadeUv = GetOceanCascadeUv(
                undisplacedWorldPosition,
                cascadePatchLength,
                cascade,
                renderFeatureParams4.x,
                renderFeatureParams4.y);
            const float2 footprintX = ddx(cascadeUv)
                * textureResolution;
            const float2 footprintY = ddy(cascadeUv)
                * textureResolution;
            const float footprintSquared = max(
                dot(footprintX, footprintX),
                dot(footprintY, footprintY));
            const float cascadeMip =
                0.5f * log2(max(footprintSquared, 1.0f)) + 0.25f;
            const float cascadeWeight = GetOceanCascadeDistanceWeight(
                cameraDistance,
                cascadePatchLength,
                spectralPeriod,
                cascade);
            const float gradientEnabled = OceanStageCascadeEnabled(
                drawInstanceParams.y,
                OceanGradientCascadeFirstBit,
                cascade);
            const float foldingEnabled = OceanStageCascadeEnabled(
                drawInstanceParams.w,
                OceanFoldingCascadeFirstBit,
                cascade);
            const float foamHistoryEnabled = OceanStageCascadeEnabled(
                drawInstanceParams.w,
                OceanFoamHistoryCascadeFirstBit,
                cascade);
            float4 gradient = float4(0.0f, 1.0f, 0.0f, 0.0f);
            if (gradientEnabled > 0.5f || foldingEnabled > 0.5f)
            {
                gradient = spectralOceanGradient.SampleLevel(
                    linearWrapSampler,
                    float3(cascadeUv,
                        float(GetOceanCascadeLayer(cascade))),
                    cascadeMip);
            }
            const float inverseUp = rcp(max(gradient.y, 0.05f));
            // Cascades are disjoint frequency bands, not alternate LOD
            // estimates of the same signal. Add their slopes; averaging them
            // removes three quarters of the near-field normal energy.
            if (gradientEnabled > 0.5f)
            {
                combinedSlope += cascadeWeight
                    * -gradient.xz * inverseUp;
            }
            // Immediate crest hats are dominated by fine folds. Persistent
            // energy remains a separate, scale-aware signal below.
            if (foldingEnabled > 0.5f)
            {
                foldingEnergy += cascadeWeight * exp2(-float(cascade))
                    * max(gradient.w, 0.0f);
            }
            if (foamHistoryEnabled > 0.5f)
            {
                const float cascadeFoam = cascadeWeight
                    * spectralOceanFoam.SampleLevel(
                        linearWrapSampler,
                        float3(cascadeUv,
                            float(GetOceanCascadeLayer(cascade))),
                        cascadeMip).x;
                foamEnergy += saturate(cascadeFoam)
                    * pow(0.70f, float(cascade));
            }
        }
        if (renderFeatureParams3.w > 0.5f)
        {
            float2 localUv = 0.0f.xx;
            const float localDomainWeight = GetOceanLocalDomainSample(
                undisplacedWorldPosition,
                renderFeatureParams3.xy,
                renderFeatureParams3.z,
                localUv)
                * PRISM_OCEAN_LOCAL_DISTANCE_WEIGHT(
                    undisplacedWorldPosition);
            if (localDomainWeight > 0.0f)
            {
                const float4 localGradient = localWaveGradient.SampleLevel(
                    linearClampSampler, localUv, 0.0f);
                const float4 localDisplacement =
                    localWaveDisplacement.SampleLevel(
                        linearClampSampler, localUv, 0.0f);
                const float localNormalLengthSquared = dot(
                    localGradient.xyz, localGradient.xyz);
                // A configured but inactive local-wave simulation publishes a
                // cleared gradient texture. Treating its zero vector as a
                // normal and normalizing it produced NaNs across the complete
                // local domain, which appeared as giant white/black triangles.
                if (localNormalLengthSquared > 0.000001f)
                {
                    const float3 localNormal = localGradient.xyz
                        * rsqrt(localNormalLengthSquared);
                    // Local waves are an additive disturbance field. Replacing
                    // the spectral normal with this value erased all short
                    // wind waves inside the 200 m domain and exposed a visible
                    // square boundary. Compose slopes so the four spectral
                    // bands remain continuous through the local-wave region.
                    const float localInverseUp = rcp(max(
                        localNormal.y, 0.05f));
                    combinedSlope += localDomainWeight
                        * -localNormal.xz * localInverseUp;
                }
                // The local map stores turbulent energy. Remove the low-level
                // numerical/history floor before it becomes optical coverage;
                // only a generated breaking wake survives this transfer.
                localFoamCoverage = smoothstep(
                    0.015f, 0.20f, saturate(localGradient.w))
                    * localDomainWeight;
                // LocalWave stores the current folding candidate separately
                // from persistent energy, matching the WaveWorks grad/fold
                // versus foam-energy split.
                localFoldingEnergy = max(localDisplacement.w, 0.0f)
                    * localDomainWeight;
            }
        }
        const float3 outputNormal = normalize(float3(
            -combinedSlope.x, 1.0f, -combinedSlope.y));
        // Simulation history stores turbulent energy, not final white color.
        // Convert it to optical coverage and add immediate breaking crests.
        // A thresholded transfer prevents tiny persistent values from becoming
        // a uniform white film over every near-field texel.
        persistentEnergy = saturate(foamEnergy + localFoamCoverage);
        surfaceFolding = foldingEnergy + localFoldingEnergy;
        const float spectralHats = 10.0f * max(
            foldingEnergy - GetOceanWhitecapsThreshold(false), 0.0f);
        // The local solver is impulse driven.  Folding alone includes the
        // circular pressure wave around a source, so require freshly generated
        // turbulent energy before exposing an immediate white cap.  This keeps
        // the Kelvin shoulders and rejects the mechanical chain of white discs.
        const float localBreakingGate = smoothstep(
            0.003f, 0.055f, localFoamCoverage);
        const float localHats = 10.0f * max(
            localFoldingEnergy - GetOceanWhitecapsThreshold(true), 0.0f)
            * localBreakingGate;
        waveHats = saturate(spectralHats + localHats);
        const float persistentCoverage = smoothstep(
            0.005f, 0.20f, persistentEnergy);
        const float opticalFoam = 1.0f
            - (1.0f - persistentCoverage) * (1.0f - waveHats)
                * (1.0f - localFoamCoverage);
        return float4(outputNormal * 0.5f + 0.5f,
            saturate(opticalFoam));
    }
    static const float OceanDetailScale = 4.13f;
    static const float OceanDetailStrength = 0.38f;
    const float surfaceMip = GetOceanSurfaceMip(input);
    const float4 baseSample =
        featureSurfaceTexture1.SampleLevel(
            linearWrapSampler,
            input.texCoord,
            surfaceMip);
    const float2x2 detailRotation = float2x2(
        0.819152f, -0.573576f,
        0.573576f, 0.819152f);
    const float2 detailUv = mul(
        input.texCoord,
        detailRotation)
        * OceanDetailScale
        + float2(0.173f, -0.117f);
    const float4 detailSample =
        featureSurfaceTexture1.SampleLevel(
            linearWrapSampler,
            detailUv,
            min(surfaceMip + 1.5f, 7.0f));
    const float3 baseNormal =
        baseSample.xyz * 2.0f - 1.0f;
    const float3 detailNormal =
        detailSample.xyz * 2.0f - 1.0f;
    const float2 combinedSlope =
        baseNormal.xz / max(baseNormal.y, 0.24f)
        + detailNormal.xz
            / max(detailNormal.y, 0.32f)
            * OceanDetailStrength;
    const float3 combinedNormal = normalize(float3(
        combinedSlope.x,
        1.0f,
        combinedSlope.y));
    const float combinedFoam = max(
        baseSample.a,
        detailSample.a * 0.52f);
    return float4(
        combinedNormal * 0.5f + 0.5f,
        combinedFoam);
}

float4 SampleOceanNormalFoam(VSOutput input)
{
    float persistentEnergy = 0.0f;
    float surfaceFolding = 0.0f;
    float waveHats = 0.0f;
    return SampleOceanNormalFoam(
        input, persistentEnergy, surfaceFolding, waveHats);
}

void ApplyOceanOpticalMaterial(
    float3 worldPosition,
    float3 surfaceNormal,
    float foam,
    inout float3 albedo,
    inout float metallicValue,
    inout float roughnessValue,
    inout float occlusionValue,
    inout float alphaValue)
{
    const float3 viewDirection = normalize(
        cameraPosition - worldPosition);
    const float ndotv = saturate(dot(
        surfaceNormal,
        viewDirection));
    const float crest = saturate(
        (1.0 - saturate(surfaceNormal.y)) * 0.65);
    const float grazingScatter = pow(
        1.0 - ndotv,
        2.0);
    const float scatterWeight = saturate(
        0.075 + crest * 0.30 + grazingScatter * 0.10);
    const float foamWeight = smoothstep(
        0.12,
        0.75,
        saturate(foam));

    // Real deep water has little diffuse albedo. Most of its apparent color
    // comes from volume scattering and reflected sky, not a bright cyan base.
    albedo = lerp(
        max(oceanDeepWaterColor, 0.0.xxx),
        max(oceanScatteringColor, 0.0.xxx),
        scatterWeight);
    albedo = lerp(
        albedo,
        max(oceanFoamColor, 0.0.xxx),
        foamWeight);
    metallicValue = 0.0;
    roughnessValue = lerp(0.085, 0.48, foamWeight);
    occlusionValue = 1.0;
    alphaValue = 1.0;
}

float3 SampleTerrainVirtualAlbedo(
    float3 worldPosition,
    float3 surfaceNormal,
    float ridgeFeature);

float4 PSMain(VSOutput input) : SV_TARGET
{
    float3 normal = BuildSurfaceNormal(input);
    float4 oceanNormalFoam = 0.0.xxxx;
    if (renderFeatureParams.x > 1.5f
        && renderFeatureParams.x < 2.5f)
    {
        oceanNormalFoam = SampleOceanNormalFoam(input);
        normal = BuildOceanSurfaceNormal(
            input,
            oceanNormalFoam.xyz * 2.0 - 1.0);
    }
    float3 viewDirection = normalize(cameraPosition - input.worldPosition);

    float3 textureSample = albedoTexture.Sample(linearWrapSampler, input.texCoord).rgb;
    float alphaSample = albedoTexture.Sample(linearWrapSampler, input.texCoord).a;
    float3 metallicRoughnessSample = metallicRoughnessTexture.Sample(linearWrapSampler, input.texCoord).rgb;
    float occlusionSample = occlusionTexture.Sample(linearWrapSampler, input.texCoord).r;
    float3 emissiveSample = emissiveTexture.Sample(linearWrapSampler, input.texCoord).rgb;
    float3 albedo = albedoColor.rgb * input.vertexColor.rgb;
    float alpha = albedoColor.a * input.vertexColor.a;
    if (useAlbedoTexture > 0.5f)
    {
        albedo *= SrgbToLinear(textureSample);
        alpha *= alphaSample;
    }

    if (alphaMaskEnabled > 0.5f && alphaMode > 0.5f && alphaMode < 1.5f && alpha < alphaCutoff)
    {
        discard;
    }

    float clampedMetallic = saturate(metallic);
    float clampedRoughness = clamp(roughness, 0.08f, 1.0f);
    if (useMetallicRoughnessTexture > 0.5f)
    {
        clampedRoughness = clamp(clampedRoughness * metallicRoughnessSample.g, 0.08f, 1.0f);
        clampedMetallic = saturate(clampedMetallic * metallicRoughnessSample.b);
    }
    float occlusion = 1.0f;
    if (occlusionEnabled > 0.5f && useOcclusionTexture > 0.5f)
    {
        occlusion = lerp(1.0f, occlusionSample, saturate(occlusionStrength));
    }
    if (renderFeatureParams.x > 1.5f
        && renderFeatureParams.x < 2.5f)
    {
        ApplyOceanOpticalMaterial(
            input.worldPosition,
            normal,
            oceanNormalFoam.a,
            albedo,
            clampedMetallic,
            clampedRoughness,
            occlusion,
            alpha);
    }
    else if (renderFeatureParams.x > 0.5f
             && renderFeatureParams.x < 1.5f)
    {
        albedo = SampleTerrainVirtualAlbedo(
            input.worldPosition,
            normalize(input.worldNormal),
            input.vertexColor.r);
        clampedMetallic = 0.02f;
        clampedRoughness = lerp(
            0.62f,
            0.92f,
            saturate(input.worldNormal.y));
        occlusion = 1.0f;
        alpha = 1.0f;
    }

    float shadowFactor = ComputeShadowFactor(input.worldPosition);
    float3 lighting = albedo * ambientIntensity * occlusion;

    float3 directionalDirection = normalize(-directionalLightDirection);
    const float directionalViewDepth =
        GetDirectionalShadowViewDepth(
            input.worldPosition,
            cameraPosition,
            cameraForward);
    const uint directionalCascadeIndex =
        SelectDirectionalShadowCascade(
            directionalViewDepth,
            cascadeSplits);
    float3 directionalColor = directionalLightColor
        * directionalLightIntensity
        * shadowFactor
        * GetCascadeDebugTint(directionalCascadeIndex);
    const bool oceanSurface = renderFeatureParams.x > 1.5f
        && renderFeatureParams.x < 2.5f;
    // The dedicated ocean shader evaluates the sun with filtered slope
    // moments. Running the generic material lobe as well turns an aligned
    // low-roughness water plane into a screen-sized white highlight.
    if (!oceanSurface
        && directLightingEnabled > 0.5f && pbrEnabled > 0.5f)
    {
        lighting += EvaluateDirectPbrLighting(
            normal,
            viewDirection,
            directionalDirection,
            directionalColor,
            albedo,
            clampedMetallic,
            clampedRoughness);
    }
    else if (!oceanSurface && directLightingEnabled > 0.5f)
    {
        lighting += EvaluateDirectBlinnPhongLighting(
            normal,
            viewDirection,
            directionalDirection,
            directionalColor,
            albedo,
            lerp(float3(0.04f, 0.04f, 0.04f), albedo, clampedMetallic),
            lerp(16.0f, 128.0f, 1.0f - clampedRoughness));
    }

    // Auxiliary directional lights are deliberately direct-light-only fills.
    // The primary sun above remains the only atmosphere and shadow source.
    if (!oceanSurface && directLightingEnabled > 0.5f)
    {
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
            const float3 auxiliaryDirection = normalize(
                -auxiliaryLight.direction);
            const float3 auxiliaryRadiance = auxiliaryLight.color
                * auxiliaryLight.intensity;
            if (pbrEnabled > 0.5f)
            {
                lighting += EvaluateDirectPbrLighting(
                    normal,
                    viewDirection,
                    auxiliaryDirection,
                    auxiliaryRadiance,
                    albedo,
                    clampedMetallic,
                    clampedRoughness);
            }
            else
            {
                lighting += EvaluateDirectBlinnPhongLighting(
                    normal,
                    viewDirection,
                    auxiliaryDirection,
                    auxiliaryRadiance,
                    albedo,
                    lerp(float3(0.04f, 0.04f, 0.04f),
                        albedo,
                        clampedMetallic),
                    lerp(16.0f, 128.0f,
                        1.0f - clampedRoughness));
            }
        }
    }

#if !defined(PRISM_OCEAN_CASCADE_METADATA)
    if (pointLightsEnabled > 0.5f
        && clusteredLightingEnabled > 0.5f
        && clusterDepthParameters.w > 0.5f)
    {
        const uint2 pixel = uint2(input.position.xy);
        const uint tileX = min(clusterTileCountX - 1u, pixel.x / 16u);
        const uint tileY = min(clusterTileCountY - 1u, pixel.y / 16u);
        const float nearPlane = clusterDepthParameters.x;
        const float viewDistance = max(
            distance(cameraPosition, input.worldPosition),
            nearPlane);
        const uint depthSlice = min(
            clusterDepthSliceCount - 1u,
            (uint)(saturate(
                log2(viewDistance / nearPlane)
                    / clusterDepthParameters.z)
                * (float)clusterDepthSliceCount));
        const uint clusterIndex =
            depthSlice * clusterTileCountX * clusterTileCountY
            + tileY * clusterTileCountX + tileX;
        const uint lightCount = min(
            clusterLightCounts[clusterIndex],
            64u);
        [loop]
        for (uint listIndex = 0u;
             listIndex < lightCount;
             ++listIndex)
        {
            const uint lightIndex = clusterLightIndices[
                clusterIndex * 64u + listIndex];
            const ClusterPointLight light =
                clusteredPointLights[lightIndex];
            const float3 lightVector =
                light.position - input.worldPosition;
            const float distanceToLight = length(lightVector);
            const float3 lightDirection =
                lightVector / max(distanceToLight, 0.0001f);
            float attenuation = saturate(
                1.0f - distanceToLight / light.range);
            attenuation *= attenuation;
            const float3 lightColor =
                light.color * light.intensity * attenuation;
            if (pbrEnabled > 0.5f)
            {
                lighting += EvaluateDirectPbrLighting(
                    normal,
                    viewDirection,
                    lightDirection,
                    lightColor,
                    albedo,
                    clampedMetallic,
                    clampedRoughness);
            }
            else
            {
                lighting += EvaluateDirectBlinnPhongLighting(
                    normal,
                    viewDirection,
                    lightDirection,
                    lightColor,
                    albedo,
                    lerp(float3(0.04f, 0.04f, 0.04f), albedo, clampedMetallic),
                    lerp(16.0f, 128.0f, 1.0f - clampedRoughness));
            }
        }
    }
#endif
    else if (pointLightsEnabled > 0.5f)
    {
        [unroll]
        for (int lightIndex = 0; lightIndex < 4; ++lightIndex)
        {
            if (lightIndex >= (int)pointLightCount)
            {
                break;
            }

            float3 lightVector = pointLights[lightIndex].position - input.worldPosition;
            float distanceToLight = length(lightVector);
            float3 lightDirection = lightVector / max(distanceToLight, 0.0001f);
            float attenuation = saturate(1.0f - distanceToLight / pointLights[lightIndex].range);
            attenuation *= attenuation;

            float3 lightColor = pointLights[lightIndex].color * pointLights[lightIndex].intensity * attenuation;
            if (pbrEnabled > 0.5f)
            {
                lighting += EvaluateDirectPbrLighting(
                    normal,
                    viewDirection,
                    lightDirection,
                    lightColor,
                    albedo,
                    clampedMetallic,
                    clampedRoughness);
            }
            else
            {
                lighting += EvaluateDirectBlinnPhongLighting(
                    normal,
                    viewDirection,
                    lightDirection,
                    lightColor,
                    albedo,
                    lerp(float3(0.04f, 0.04f, 0.04f), albedo, clampedMetallic),
                    lerp(16.0f, 128.0f, 1.0f - clampedRoughness));
            }
        }
    }

    if (pbrEnabled > 0.5f && iblEnabled > 0.5f)
    {
        const bool isOceanSurface =
            renderFeatureParams.x > 1.5f
            && renderFeatureParams.x < 2.5f;
        lighting += EvaluateIbl(
            normal,
            viewDirection,
            albedo,
            clampedMetallic,
            clampedRoughness,
            isOceanSurface) * occlusion;
    }

    if (renderFeatureParams.x > 1.5f
        && renderFeatureParams.x < 2.5f)
    {
        const float waterFacing = saturate(normal.y * 0.5f + 0.5f);
        const float grazing = pow(
            1.0f - saturate(dot(normal, viewDirection)),
            2.0f);
        // Low-frequency in-water scattering is a subtle transmitted-light
        // term. The previous 18--30% floor was added after diffuse and IBL,
        // washing the whole surface cyan and hiding spectral normals.
        lighting += max(
                oceanScatteringColor,
                0.0f.xxx)
            * max(oceanScatteringStrength, 1.0f)
            * lerp(0.022f, 0.048f, waterFacing)
            * lerp(1.0f, 1.08f, grazing);
    }

    float3 emissive = emissiveColor;
    if (emissiveEnabled > 0.5f && useEmissiveTexture > 0.5f)
    {
        emissive *= SrgbToLinear(emissiveSample);
    }

    if (emissiveEnabled > 0.5f)
    {
        lighting += emissive * emissiveStrength;
    }
    return float4(max(lighting, 0.0f.xxx), alpha);
}

struct GBufferOutput
{
    float4 worldPositionRoughness : SV_TARGET0;
    float4 normalMetallic : SV_TARGET1;
    float4 albedoOcclusion : SV_TARGET2;
    float4 emissiveAlpha : SV_TARGET3;
    float2 motionVector : SV_TARGET4;
};

float TerrainVirtualHash(uint2 coordinate)
{
    uint value = coordinate.x * 0x8da6b343u
        ^ coordinate.y * 0xd8163841u ^ 0xcb1ab31fu;
    value ^= value >> 13u;
    value *= 0x85ebca6bu;
    value ^= value >> 16u;
    return (float)(value & 0xffffu) / 65535.0f;
}

float3 EvaluateTerrainVirtualFallback(
    uint2 virtualPixel,
    uint pageSize)
{
    const uint2 virtualPage = virtualPixel / pageSize;
    const uint2 localPixel = virtualPixel % pageSize;
    const float noise = TerrainVirtualHash(virtualPixel);
    const float broad = TerrainVirtualHash(uint2(
        virtualPage.x * 7u + localPixel.x / 16u,
        virtualPage.y * 11u + localPixel.y / 16u));
    const float moss = 0.65f * noise + 0.35f * broad;
    const float path = abs(sin(
        ((float)virtualPixel.x + (float)virtualPixel.y * 0.37f)
        * 0.035f));
    const float pathMask = saturate((path - 0.72f) * 5.0f);
    return float3(
        lerp(0.12f + moss * 0.10f, 0.32f, pathMask),
        lerp(0.28f + moss * 0.26f, 0.25f, pathMask),
        lerp(0.075f + moss * 0.07f, 0.14f, pathMask));
}

float3 SampleTerrainVirtualAlbedo(
    float3 worldPosition,
    float3 surfaceNormal,
    float ridgeFeature)
{
    const float terrainSize = max(renderFeatureParams.y, 1.0);
    float3 terrainColor = float3(0.72f, 0.74f, 0.76f);
    if (renderFeatureParams3.x > 0.5f)
    {
        // Continuous material palette used when the diagnostic VT atlas is
        // disabled. It keeps terrain morphology readable without UV patterns.
        float3 detailColor = float3(0.24f, 0.40f, 0.14f);
        if (renderFeatureParams.z > 0.5f)
        {
            const uint virtualPageCount = (uint)renderFeatureParams.z;
            const uint physicalPageGrid = max((uint)renderFeatureParams.w, 1u);
            const float2 virtualUv = saturate(
                worldPosition.xz / terrainSize + 0.5);
            const float2 virtualPagePosition =
                virtualUv * virtualPageCount;
            const uint2 pageCoordinate = min(
                (uint2)virtualPagePosition,
                virtualPageCount - 1u);
            const uint pageIndex =
                pageCoordinate.y * virtualPageCount
                + pageCoordinate.x;
            const uint encodedPhysicalPage =
                terrainPageTable[pageIndex];
            uint atlasWidth;
            uint atlasHeight;
            featureSurfaceTexture0.GetDimensions(
                atlasWidth, atlasHeight);
            const uint pageSize = max(
                atlasWidth / physicalPageGrid, 1u);
            const uint virtualPixelCount =
                virtualPageCount * pageSize;
            const uint2 virtualPixel = min(
                (uint2)(virtualUv * (float)virtualPixelCount),
                virtualPixelCount - 1u);
            // This deterministic color is the demo's mip-tail equivalent.
            detailColor = EvaluateTerrainVirtualFallback(
                virtualPixel, pageSize);
            if (encodedPhysicalPage != 0u)
            {
                const uint physicalPage = encodedPhysicalPage - 1u;
                const uint2 physicalCoordinate = uint2(
                    physicalPage % physicalPageGrid,
                    physicalPage / physicalPageGrid);
                const float2 localUv = frac(virtualPagePosition);
                const float2 insetUv =
                    (localUv * ((float)pageSize - 2.0) + 1.0)
                    / (float)pageSize;
                const float2 atlasUv =
                    (float2(physicalCoordinate) + insetUv)
                    / physicalPageGrid;
                detailColor = featureSurfaceTexture0.Sample(
                    linearClampSampler, atlasUv).rgb;
            }
        }
        const float slope = 1.0 - saturate(surfaceNormal.y);
        const float rockMask = smoothstep(0.24, 0.62, slope);
        const float terrainHeightScale = sqrt(
            max(terrainSize / 512.0f, 1.0f));
        const float snowMask = smoothstep(
            48.0f * terrainHeightScale,
            72.0f * terrainHeightScale,
            worldPosition.y)
            * smoothstep(0.32, 0.0, slope);
        const float valleyMoisture =
            1.0 - smoothstep(
                4.0f * terrainHeightScale,
                22.0f * terrainHeightScale,
                worldPosition.y);
        // The CPU erosion filter stores its signed ridge map in vertexColor.r:
        // zero is a gully crease, one is a ridge, and 0.5 is neutral terrain.
        const float drainageMask =
            1.0f - smoothstep(0.16f, 0.46f, ridgeFeature);
        const float ridgeMask = smoothstep(0.66f, 0.92f, ridgeFeature);
        detailColor = lerp(
            detailColor,
            float3(0.055, 0.20, 0.09),
            valleyMoisture * 0.55);
        detailColor = lerp(
            detailColor,
            float3(0.075f, 0.125f, 0.065f),
            drainageMask * 0.34f);
        detailColor = lerp(
            detailColor,
            float3(0.38f, 0.34f, 0.28f),
            ridgeMask * 0.16f);
        detailColor = lerp(
            detailColor,
            float3(0.32, 0.29, 0.25),
            rockMask);
        terrainColor = lerp(
            detailColor,
            float3(0.82, 0.86, 0.90),
            snowMask);
    }
    if (renderFeatureParams2.z > 0.5f)
    {
        const float tileWorldSize = max(renderFeatureParams2.x, 1.0f);
        const float borderWidth = max(renderFeatureParams2.w, 0.25f);
        const float2 tileUv = frac(
            (worldPosition.xz + terrainSize * 0.5f)
            / tileWorldSize);
        const float edgeDistance = min(
            min(tileUv.x, 1.0f - tileUv.x),
            min(tileUv.y, 1.0f - tileUv.y));
        const float tileLine = 1.0f - smoothstep(
            0.0f,
            borderWidth / tileWorldSize,
            edgeDistance);
        if (renderFeatureParams2.z < 1.5f)
        {
            const float3 lodColor = renderFeatureParams2.y < 0.5f
                ? float3(1.0f, 0.42f, 0.08f)
                : float3(0.08f, 0.72f, 1.0f);
            terrainColor = lerp(
                terrainColor,
                lodColor,
                tileLine * 0.88f);
        }
        else
        {
            // 2 visible, 3 LOD-rejected, 4 frustum-culled,
            // 5 Hi-Z-culled, 6 disabled. Visible tiles retain their material.
            const float debugReason = renderFeatureParams2.z;
            float3 stateColor = float3(0.08f, 0.10f, 0.08f);
            float stateTint = 0.0f;
            if (debugReason < 3.5f)
            {
                stateColor = float3(0.30f, 0.32f, 0.34f);
                stateTint = debugReason > 2.5f ? 0.62f : 0.0f;
            }
            else if (debugReason < 4.5f)
            {
                stateColor = float3(0.72f, 0.055f, 0.045f);
                stateTint = 0.62f;
            }
            else if (debugReason < 5.5f)
            {
                stateColor = float3(0.035f, 0.20f, 0.82f);
                stateTint = 0.64f;
            }
            else
            {
                stateColor = float3(0.18f, 0.19f, 0.20f);
                stateTint = 0.72f;
            }
            terrainColor = lerp(
                terrainColor,
                stateColor,
                stateTint);
            terrainColor = lerp(
                terrainColor,
                stateColor,
                tileLine * 0.92f);
        }
    }
    return terrainColor;
}

GBufferOutput GBufferPS(VSOutput input)
{
    GBufferOutput output;

    float3 normal = BuildSurfaceNormal(input);
    float4 oceanNormalFoam = 0.0.xxxx;
    if (renderFeatureParams.x > 1.5f
        && renderFeatureParams.x < 2.5f)
    {
        oceanNormalFoam = SampleOceanNormalFoam(input);
        normal = BuildOceanSurfaceNormal(
            input,
            oceanNormalFoam.xyz * 2.0 - 1.0);
    }
    float3 textureSample = albedoTexture.Sample(linearWrapSampler, input.texCoord).rgb;
    float alphaSample = albedoTexture.Sample(linearWrapSampler, input.texCoord).a;
    float3 metallicRoughnessSample = metallicRoughnessTexture.Sample(linearWrapSampler, input.texCoord).rgb;
    float occlusionSample = occlusionTexture.Sample(linearWrapSampler, input.texCoord).r;
    float3 emissiveSample = emissiveTexture.Sample(linearWrapSampler, input.texCoord).rgb;

    float3 albedo = albedoColor.rgb * input.vertexColor.rgb;
    float alpha = albedoColor.a * input.vertexColor.a;
    if (useAlbedoTexture > 0.5f)
    {
        albedo *= SrgbToLinear(textureSample);
        alpha *= alphaSample;
    }

    if (alphaMaskEnabled > 0.5f && alphaMode > 0.5f && alphaMode < 1.5f && alpha < alphaCutoff)
    {
        discard;
    }

    float clampedMetallic = saturate(metallic);
    float clampedRoughness = clamp(roughness, 0.08f, 1.0f);
    if (useMetallicRoughnessTexture > 0.5f)
    {
        clampedRoughness = clamp(clampedRoughness * metallicRoughnessSample.g, 0.08f, 1.0f);
        clampedMetallic = saturate(clampedMetallic * metallicRoughnessSample.b);
    }

    float occlusion = 1.0f;
    if (occlusionEnabled > 0.5f && useOcclusionTexture > 0.5f)
    {
        occlusion = lerp(1.0f, occlusionSample, saturate(occlusionStrength));
    }

    float3 emissive = emissiveColor;
    if (emissiveEnabled > 0.5f && useEmissiveTexture > 0.5f)
    {
        emissive *= SrgbToLinear(emissiveSample);
    }
    emissive = emissiveEnabled > 0.5f ? emissive * emissiveStrength : 0.0f.xxx;

    if (renderFeatureParams.x > 0.5f
        && renderFeatureParams.x < 1.5f)
    {
        albedo = SampleTerrainVirtualAlbedo(
            input.worldPosition,
            normalize(input.worldNormal),
            input.vertexColor.r);
        clampedMetallic = 0.02;
        clampedRoughness = lerp(
            0.62, 0.92,
            saturate(input.worldNormal.y));
        occlusion = 1.0;
        emissive = 0.0.xxx;
        alpha = 1.0;
    }
    else if (renderFeatureParams.x > 1.5f
             && renderFeatureParams.x < 2.5f)
    {
        const float foam = saturate(oceanNormalFoam.a);
        ApplyOceanOpticalMaterial(
            input.worldPosition,
            normal,
            foam,
            albedo,
            clampedMetallic,
            clampedRoughness,
            occlusion,
            alpha);
        // Deferred lighting has no surface-type channel. Carry the water's
        // low-frequency volume scattering through the emissive G-buffer lane;
        // specular sky reflection remains in the regular PBR path.
        const float3 viewDirection = normalize(
            cameraPosition - input.worldPosition);
        const float waterFacing = saturate(normal.y * 0.5f + 0.5f);
        const float grazing = pow(
            1.0f - saturate(dot(normal, viewDirection)),
            2.0f);
        emissive = max(
                oceanScatteringColor,
                0.0f.xxx)
            * max(oceanScatteringStrength, 1.0f)
            * lerp(0.022f, 0.048f, waterFacing)
            * lerp(1.0f, 1.08f, grazing);
    }

    output.worldPositionRoughness = float4(input.worldPosition, clampedRoughness);
    output.normalMetallic = float4(normalize(normal) * 0.5f + 0.5f, clampedMetallic);
    output.albedoOcclusion = float4(albedo, occlusion);
    output.emissiveAlpha = float4(emissive, alpha);
    const float2 currentNdc =
        input.currentClipPosition.xy
        / max(
            input.currentClipPosition.w,
            0.0001f);
    const float2 previousNdc =
        input.previousClipPosition.xy
        / max(
            input.previousClipPosition.w,
            0.0001f);
    output.motionVector =
        (currentNdc - previousNdc)
        * float2(0.5f, -0.5f);
    return output;
}
