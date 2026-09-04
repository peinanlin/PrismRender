#include "ShaderBindings.hlsli"

PRISM_VK_BINDING(0) cbuffer PlanarFrameConstants : register(b0)
{
    float4x4 reflectedViewProjection;
    float3 reflectedCameraPosition;
    float reflectionPlaneHeight;
    float3 directionalLightDirection;
    float directionalLightIntensity;
    float3 directionalLightColor;
    float planarPassEnabled;
};

PRISM_VK_BINDING(1) cbuffer PlanarObjectConstants : register(b1)
{
    float4x4 world;
};

PRISM_VK_BINDING(2) cbuffer PlanarMaterialConstants : register(b2)
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
    float materialPadding;
};

PRISM_VK_BINDING(16) Texture2D albedoTexture : register(t0);
PRISM_VK_BINDING(48) SamplerState linearWrapSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float4 color : COLOR;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
    float4 vertexColor : COLOR;
    float clipDistance : SV_ClipDistance0;
};

VSOutput PlanarReflectionVS(VSInput input)
{
    VSOutput output;
    output.worldPosition = mul(float4(input.position, 1.0f), world).xyz;
    output.worldNormal = normalize(mul(float4(input.normal, 0.0f), world).xyz);
    output.position = mul(
        float4(output.worldPosition, 1.0f),
        reflectedViewProjection);
    output.texCoord = input.texCoord;
    output.vertexColor = input.color;
    output.clipDistance = output.worldPosition.y - reflectionPlaneHeight + 0.01f;
    return output;
}

float3 SrgbToLinear(float3 value)
{
    return pow(max(value, 0.0f.xxx), 2.2f.xxx);
}

float4 PlanarReflectionPS(VSOutput input) : SV_TARGET
{
    float4 albedoSample = albedoTexture.Sample(
        linearWrapSampler,
        input.texCoord);
    float3 albedo = albedoColor.rgb * input.vertexColor.rgb;
    float alpha = albedoColor.a * input.vertexColor.a;
    if (useAlbedoTexture > 0.5f)
    {
        albedo *= SrgbToLinear(albedoSample.rgb);
        alpha *= albedoSample.a;
    }
    if (alphaMode > 0.5f
        && alphaMode < 1.5f
        && alpha < alphaCutoff)
    {
        discard;
    }

    const float3 normal = normalize(input.worldNormal);
    const float3 lightDirection = normalize(-directionalLightDirection);
    const float3 viewDirection = normalize(
        reflectedCameraPosition - input.worldPosition);
    const float diffuse = saturate(dot(normal, lightDirection));
    const float3 halfVector = normalize(lightDirection + viewDirection);
    const float specularPower = lerp(96.0f, 8.0f, saturate(roughness));
    const float specular = pow(
        saturate(dot(normal, halfVector)),
        specularPower);
    const float3 litColor = albedo * 0.07f
        + albedo * directionalLightColor
            * directionalLightIntensity * diffuse
        + directionalLightColor * directionalLightIntensity
            * specular * lerp(0.04f, 1.0f, saturate(metallic));
    return float4(
        max(litColor + emissiveColor * emissiveStrength, 0.0f.xxx),
        alpha);
}
