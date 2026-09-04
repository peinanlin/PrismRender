#include "ShaderBindings.hlsli"

struct PointLight
{
    float3 position;
    float range;
    float3 color;
    float intensity;
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
};

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

PRISM_VK_BINDING(3) cbuffer ShadowPassConstants : register(b3)
{
    uint cascadeIndex;
};

PRISM_VK_BINDING(16)
Texture2D albedoTexture : register(t0);
PRISM_VK_BINDING(17)
Texture2D terrainHeightTexture : register(t1);
PRISM_VK_BINDING(48)
SamplerState linearWrapSampler : register(s0);
PRISM_VK_BINDING(49)
SamplerState linearClampSampler : register(s1);

struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(3)]] float2 texCoord : TEXCOORD;
};

struct VSInstancedInput
{
    float3 position : POSITION;
    float4 instanceWorld0 : INSTANCEWORLD0;
    float4 instanceWorld1 : INSTANCEWORLD1;
    float4 instanceWorld2 : INSTANCEWORLD2;
    float4 instanceWorld3 : INSTANCEWORLD3;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float3 localPosition = input.position;
    if (renderFeatureParams.x > 0.5f
        && renderFeatureParams.x < 1.5f
        && renderFeatureParams3.w > 0.5f)
    {
        const float2 terrainUv = saturate(
            localPosition.xz / max(renderFeatureParams.y, 1.0f)
            + 0.5f);
        localPosition.y += renderFeatureParams3.y
            + terrainHeightTexture.SampleLevel(
                linearClampSampler,
                terrainUv,
                0.0f).x * renderFeatureParams3.z;
    }
    output.position = mul(
        mul(float4(localPosition, 1.0f), world),
        lightViewProjections[cascadeIndex]);
    output.texCoord = input.texCoord;
    return output;
}

VSOutput VSInstancedMain(VSInstancedInput input)
{
    VSOutput output;
    float4 localPosition = float4(input.position, 1.0f);
    float4 worldPosition = float4(
        dot(localPosition, input.instanceWorld0),
        dot(localPosition, input.instanceWorld1),
        dot(localPosition, input.instanceWorld2),
        dot(localPosition, input.instanceWorld3));
    output.position = mul(worldPosition, lightViewProjections[cascadeIndex]);
    output.texCoord = float2(0.0f, 0.0f);
    return output;
}

void PSMain(VSOutput input)
{
    float alpha = albedoColor.a;
    if (useAlbedoTexture > 0.5f)
    {
        alpha *= albedoTexture.Sample(
            linearWrapSampler,
            input.texCoord).a;
    }
    if (alphaMode > 0.5f
        && alphaMode < 1.5f
        && alpha < alphaCutoff)
    {
        discard;
    }
}
