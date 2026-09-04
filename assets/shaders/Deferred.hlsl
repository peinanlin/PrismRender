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

struct ClusterPointLight
{
    float3 position;
    float range;
    float3 color;
    float intensity;
};

struct SpotShadowLight
{
    float4x4 viewProjection;
    float4 positionRange;
    float4 directionOuterCos;
    float4 colorIntensity;
    float4 parameters;
};

struct PointShadowLight
{
    float4 positionRange;
    float4 colorIntensity;
    float4 parameters;
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
};

PRISM_VK_BINDING(1) cbuffer ClusterConstants : register(b1)
{
    float4x4 clusterWorldToView;
    float4 clusterViewportProjection;
    float4 clusterDepthParameters;
    uint clusterTileCountX;
    uint clusterTileCountY;
    uint clusterDepthSliceCount;
    uint clusteredLightCount;
};

PRISM_VK_BINDING(2) cbuffer LocalLightConstants : register(b2)
{
    SpotShadowLight spotShadowLights[4];
    PointShadowLight pointShadowLights[4];
    float4 localLightCountsAndNear;
};

PRISM_VK_BINDING(16) Texture2D gbufferPositionRoughness : register(t0);
PRISM_VK_BINDING(17) Texture2D gbufferNormalMetallic : register(t1);
PRISM_VK_BINDING(18) Texture2D gbufferAlbedoOcclusion : register(t2);
PRISM_VK_BINDING(19) Texture2D gbufferEmissiveAlpha : register(t3);
PRISM_VK_BINDING(20) Texture2DArray shadowMap : register(t4);
PRISM_VK_BINDING(21) TextureCube irradianceTexture : register(t5);
PRISM_VK_BINDING(22) TextureCubeArray prefilteredEnvironmentTexture : register(t6);
PRISM_VK_BINDING(23) Texture2D brdfLutTexture : register(t7);
PRISM_VK_BINDING(24) Texture2D screenSpaceAmbientOcclusion : register(t8);
PRISM_VK_BINDING(25)
StructuredBuffer<ClusterPointLight>
    clusteredPointLights : register(t9);
PRISM_VK_BINDING(26)
StructuredBuffer<uint>
    clusterLightCounts : register(t10);
PRISM_VK_BINDING(27)
StructuredBuffer<uint>
    clusterLightIndices : register(t11);
PRISM_VK_BINDING(28)
Texture2DArray spotShadowMap : register(t12);
PRISM_VK_BINDING(29)
TextureCubeArray pointShadowMap : register(t13);
PRISM_VK_BINDING(30)
Texture2DArray<float4> varianceShadowMoments : register(t14);
PRISM_VK_BINDING(31)
Texture2D atmosphereSkyViewLut : register(t15);
PRISM_VK_BINDING(48) SamplerState linearClampSampler : register(s0);
PRISM_VK_BINDING(49) SamplerComparisonState shadowSampler : register(s1);

#include "ShadowFiltering.hlsli"

static const float PI = 3.14159265f;

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

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
    return GeometrySchlickGGX(saturate(dot(N, V)), roughnessValue) * GeometrySchlickGGX(saturate(dot(N, L)), roughnessValue);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(1.0f - saturate(cosTheta), 5.0f);
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

float ComputeSpotShadow(
    SpotShadowLight light,
    float3 worldPosition)
{
    if (light.parameters.z < 0.5f)
    {
        return 1.0f;
    }
    const float4 shadowPosition =
        mul(
            float4(worldPosition, 1.0f),
            light.viewProjection);
    const float3 projected =
        shadowPosition.xyz
        / max(shadowPosition.w, 0.0001f);
    const float2 uv =
        projected.xy * float2(0.5f, -0.5f)
        + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f
        || uv.y < 0.0f || uv.y > 1.0f
        || projected.z < 0.0f
        || projected.z > 1.0f)
    {
        return 1.0f;
    }

    uint width = 0;
    uint height = 0;
    uint layers = 0;
    uint levels = 0;
    spotShadowMap.GetDimensions(
        0,
        width,
        height,
        layers,
        levels);
    const float2 texelSize =
        1.0f / float2(width, height);
    const float referenceDepth =
        projected.z - light.parameters.w;
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            visibility +=
                spotShadowMap.SampleCmpLevelZero(
                    shadowSampler,
                    float3(
                        uv
                            + float2(x, y)
                                * texelSize,
                        light.parameters.y),
                    referenceDepth);
        }
    }
    return lerp(
        0.2f,
        1.0f,
        visibility / 9.0f);
}

float ComputePointShadow(
    uint sceneLightIndex,
    float3 worldPosition)
{
    const uint shadowCount =
        min(
            (uint)localLightCountsAndNear.y,
            4u);
    [loop]
    for (uint shadowIndex = 0;
         shadowIndex < shadowCount;
         ++shadowIndex)
    {
        const PointShadowLight shadow =
            pointShadowLights[shadowIndex];
        if ((uint)shadow.parameters.x
                != sceneLightIndex
            || shadow.parameters.z < 0.5f)
        {
            continue;
        }
        const float3 lightToSurface =
            worldPosition
            - shadow.positionRange.xyz;
        const float distanceAlongCubeFace =
            max(
                max(
                    abs(lightToSurface.x),
                    abs(lightToSurface.y)),
                abs(lightToSurface.z));
        const float nearPlane =
            localLightCountsAndNear.z;
        const float farPlane =
            shadow.positionRange.w;
        const float referenceDepth =
            farPlane
                / max(
                    farPlane - nearPlane,
                    0.0001f)
            - nearPlane * farPlane
                / max(
                    (farPlane - nearPlane)
                        * distanceAlongCubeFace,
                    0.0001f)
            - shadow.parameters.w;
        return lerp(
            0.2f,
            1.0f,
            pointShadowMap.SampleCmpLevelZero(
                shadowSampler,
                float4(
                    normalize(lightToSurface),
                    shadow.parameters.y),
                referenceDepth));
    }
    return 1.0f;
}

float3 EvaluateDirectPbrLighting(float3 normal, float3 viewDirection, float3 lightDirection, float3 lightColor, float3 baseColor, float metallicValue, float roughnessValue)
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
    float ndf = DistributionGGX(normal, halfVector, clampedRoughness);
    float geometry = GeometrySmith(normal, viewDirection, lightDirection, clampedRoughness);
    float3 fresnel = FresnelSchlick(HdotV, F0);
    float3 specular = (ndf * geometry * fresnel) / max(4.0f * NdotV * NdotL, 0.0001f);
    float3 diffuse = (1.0f.xxx - fresnel) * (1.0f - metallicValue) * baseColor / PI;
    return (diffuse + specular) * lightColor * NdotL;
}

float2 ComputeAtmosphereSkyUv(
    float3 direction,
    float3 sunDirection)
{
    static const float Pi = 3.14159265359f;
    const float zenith = acos(clamp(
        direction.y,
        -1.0,
        1.0));
    const float2 directionHorizontal = direction.xz;
    const float2 sunHorizontal = sunDirection.xz;
    float relativeAzimuth = 0.0;
    if (dot(directionHorizontal, directionHorizontal) > 0.000001
        && dot(sunHorizontal, sunHorizontal) > 0.000001)
    {
        relativeAzimuth = acos(clamp(
            dot(
                normalize(directionHorizontal),
                normalize(sunHorizontal)),
            -1.0,
            1.0));
    }
    return float2(relativeAzimuth / Pi, zenith / Pi);
}

float3 SamplePhysicalAtmosphere(float3 direction)
{
    const float3 sunDirection = normalize(
        -directionalLightDirection);
    return atmosphereSkyViewLut.SampleLevel(
        linearClampSampler,
        ComputeAtmosphereSkyUv(
            normalize(direction),
            sunDirection),
        0.0).rgb
        * max(atmosphereBrightness, 0.0);
}

float3 EvaluateIbl(float3 normal, float3 viewDirection, float3 baseColor, float metallicValue, float roughnessValue)
{
    static const float prefilterCubeCount = 4.0f;
    float3 reflectedDirection = reflect(-viewDirection, normal);
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallicValue);
    float NdotV = saturate(dot(normal, viewDirection));
    float3 diffuseEnvironment = irradianceTexture.SampleLevel(linearClampSampler, normal, 0).rgb;
    float3 diffuse = diffuseEnvironment * baseColor * (1.0f - metallicValue) * iblDiffuseStrength;

    float roughnessFactor = saturate(roughnessValue * roughnessValue);
    float3 blendedReflection = normalize(lerp(reflectedDirection, normal, saturate(iblReflectionBlend) * roughnessFactor));
    float prefilterLevel = saturate(roughnessValue) * (prefilterCubeCount - 1.0f);
    float prefilterIndexLow = floor(prefilterLevel);
    float prefilterIndexHigh = min(prefilterCubeCount - 1.0f, prefilterIndexLow + 1.0f);
    float prefilterLerp = saturate(prefilterLevel - prefilterIndexLow);
    float3 prefilteredLow = prefilteredEnvironmentTexture.SampleLevel(linearClampSampler, float4(blendedReflection, prefilterIndexLow), 0).rgb;
    float3 prefilteredHigh = prefilteredEnvironmentTexture.SampleLevel(linearClampSampler, float4(blendedReflection, prefilterIndexHigh), 0).rgb;
    float3 specularEnvironment = lerp(prefilteredLow, prefilteredHigh, prefilterLerp);
    if (physicalAtmosphereEnabled > 0.5)
    {
        const float atmosphereReflectionWeight = pow(
            saturate(1.0 - roughnessValue),
            2.0);
        specularEnvironment = lerp(
            specularEnvironment,
            SamplePhysicalAtmosphere(reflectedDirection),
            atmosphereReflectionWeight);
    }
    float2 brdf = brdfLutTexture.SampleLevel(linearClampSampler, float2(NdotV, saturate(roughnessValue)), 0).rg;
    float3 specular = specularEnvironment * (F0 * brdf.x + brdf.y) * iblSpecularStrength;
    return (diffuse + specular) * iblIntensity;
}

float4 DeferredLightingPS(VSOutput input) : SV_TARGET
{
    float4 positionRoughness = gbufferPositionRoughness.SampleLevel(linearClampSampler, input.uv, 0);
    float4 normalMetallic = gbufferNormalMetallic.SampleLevel(linearClampSampler, input.uv, 0);
    float4 albedoOcclusion = gbufferAlbedoOcclusion.SampleLevel(linearClampSampler, input.uv, 0);
    float4 emissiveAlpha = gbufferEmissiveAlpha.SampleLevel(linearClampSampler, input.uv, 0);
    if (emissiveAlpha.a <= 0.001f)
    {
        discard;
    }

    float3 worldPosition = positionRoughness.xyz;
    float roughness = positionRoughness.w;
    float3 normal = normalize(normalMetallic.xyz * 2.0f - 1.0f);
    float metallic = normalMetallic.w;
    float3 albedo = albedoOcclusion.rgb;
    float occlusion = albedoOcclusion.a;
    float gtao = 1.0f;
    if (gtaoEnabled > 0.5f)
    {
        gtao = screenSpaceAmbientOcclusion.SampleLevel(
            linearClampSampler,
            input.uv,
            0).r;
    }
    occlusion *= gtao;
    float3 viewDirection = normalize(cameraPosition - worldPosition);

    float3 lighting = albedo * ambientIntensity * occlusion;
    if (directLightingEnabled > 0.5f)
    {
        float3 directionalDirection = normalize(-directionalLightDirection);
        float shadowFactor = ComputeShadowFactor(worldPosition);
        const float directionalViewDepth =
            GetDirectionalShadowViewDepth(
                worldPosition,
                cameraPosition,
                cameraForward);
        const uint directionalCascadeIndex =
            SelectDirectionalShadowCascade(
                directionalViewDepth,
                cascadeSplits);
        lighting += EvaluateDirectPbrLighting(
            normal,
            viewDirection,
            directionalDirection,
            directionalLightColor
                * directionalLightIntensity
                * shadowFactor
                * GetCascadeDebugTint(directionalCascadeIndex),
            albedo,
            metallic,
            roughness);

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
            lighting += EvaluateDirectPbrLighting(
                normal,
                viewDirection,
                normalize(-auxiliaryLight.direction),
                auxiliaryLight.color * auxiliaryLight.intensity,
                albedo,
                metallic,
                roughness);
        }
    }

    if (pointLightsEnabled > 0.5f)
    {
        if (clusterDepthParameters.w > 0.5f)
        {
            const uint2 pixel =
                uint2(input.position.xy);
            const uint tileX =
                min(
                    clusterTileCountX - 1u,
                    pixel.x / 16u);
            const uint tileY =
                min(
                    clusterTileCountY - 1u,
                    pixel.y / 16u);
            const float nearPlane =
                clusterDepthParameters.x;
            const float viewDistance =
                max(
                    distance(
                        cameraPosition,
                        worldPosition),
                    nearPlane);
            const uint depthSlice =
                min(
                    clusterDepthSliceCount - 1u,
                    (uint)(
                        saturate(
                            log2(
                                viewDistance
                                / nearPlane)
                            / clusterDepthParameters.z)
                        * (float)
                            clusterDepthSliceCount));
            const uint clusterIndex =
                depthSlice
                    * clusterTileCountX
                    * clusterTileCountY
                + tileY * clusterTileCountX
                + tileX;
            const uint lightCount =
                min(
                    clusterLightCounts[
                        clusterIndex],
                    64u);
            [loop]
            for (uint listIndex = 0;
                 listIndex < lightCount;
                 ++listIndex)
            {
                const uint lightIndex =
                    clusterLightIndices[
                        clusterIndex * 64u
                        + listIndex];
                const ClusterPointLight light =
                    clusteredPointLights[
                        lightIndex];
                float3 lightVector =
                    light.position
                    - worldPosition;
                float distanceToLight =
                    length(lightVector);
                float attenuation =
                    saturate(
                        1.0f
                        - distanceToLight
                            / light.range);
                attenuation *= attenuation;
                lighting += EvaluateDirectPbrLighting(
                    normal,
                    viewDirection,
                    lightVector
                        / max(
                            distanceToLight,
                            0.0001f),
                    light.color
                        * light.intensity
                        * attenuation
                        * ComputePointShadow(
                            lightIndex,
                            worldPosition),
                    albedo,
                    metallic,
                    roughness);
            }
        }
        else
        {
            [unroll]
            for (int lightIndex = 0;
                 lightIndex < 4;
                 ++lightIndex)
            {
                if (lightIndex
                    >= (int)pointLightCount)
                {
                    break;
                }
                float3 lightVector =
                    pointLights[lightIndex]
                        .position
                    - worldPosition;
                float distanceToLight =
                    length(lightVector);
                float attenuation =
                    saturate(
                        1.0f
                        - distanceToLight
                            / pointLights[lightIndex]
                                  .range);
                attenuation *= attenuation;
                lighting += EvaluateDirectPbrLighting(
                    normal,
                    viewDirection,
                    lightVector
                        / max(
                            distanceToLight,
                            0.0001f),
                    pointLights[lightIndex]
                            .color
                        * pointLights[lightIndex]
                              .intensity
                        * attenuation
                        * ComputePointShadow(
                            (uint)lightIndex,
                            worldPosition),
                    albedo,
                    metallic,
                    roughness);
            }
        }
    }

    const uint spotLightCount =
        min(
            (uint)localLightCountsAndNear.x,
            4u);
    [loop]
    for (uint spotIndex = 0;
         spotIndex < spotLightCount;
         ++spotIndex)
    {
        const SpotShadowLight light =
            spotShadowLights[spotIndex];
        const float3 lightVector =
            light.positionRange.xyz
            - worldPosition;
        const float distanceToLight =
            length(lightVector);
        const float3 surfaceDirection =
            -lightVector
            / max(distanceToLight, 0.0001f);
        const float coneCosine =
            dot(
                surfaceDirection,
                normalize(
                    light.directionOuterCos.xyz));
        const float coneAttenuation =
            smoothstep(
                light.directionOuterCos.w,
                light.parameters.x,
                coneCosine);
        float distanceAttenuation =
            saturate(
                1.0f
                - distanceToLight
                    / light.positionRange.w);
        distanceAttenuation *=
            distanceAttenuation;
        lighting += EvaluateDirectPbrLighting(
            normal,
            viewDirection,
            lightVector
                / max(distanceToLight, 0.0001f),
            light.colorIntensity.rgb
                * light.colorIntensity.a
                * distanceAttenuation
                * coneAttenuation
                * ComputeSpotShadow(
                    light,
                    worldPosition),
            albedo,
            metallic,
            roughness);
    }

    if (iblEnabled > 0.5f)
    {
        lighting += EvaluateIbl(normal, viewDirection, albedo, metallic, roughness) * occlusion;
    }

    lighting += emissiveAlpha.rgb;
    return float4(max(lighting, 0.0f.xxx), emissiveAlpha.a);
}
