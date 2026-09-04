#include "ShaderBindings.hlsli"

PRISM_VK_BINDING(16) Texture2DArray shadowTexture : register(t0);
PRISM_VK_BINDING(48) SamplerState linearClampSampler : register(s0);

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(
        output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float3 LinearToSrgb(float3 color)
{
    color = saturate(color);
    float3 low = color * 12.92f;
    float3 high = 1.055f * pow(color, 1.0f / 2.4f) - 0.055f;
    return lerp(high, low, step(color, 0.0031308f));
}

float4 ShadowDebugPS(VSOutput input) : SV_TARGET
{
    float depth = shadowTexture.SampleLevel(linearClampSampler, float3(input.uv, 0.0f), 0).r;
    float visibleDepth = pow(saturate(depth), 16.0f);
    return float4(LinearToSrgb(visibleDepth.xxx), 1.0f);
}
