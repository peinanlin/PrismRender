#include "ShaderBindings.hlsli"

PRISM_VK_BINDING(0) cbuffer LocalShadowRenderConstants : register(b0)
{
    float4x4 worldViewProjection;
};

struct VSInput
{
    float3 position : POSITION;
};

struct VSOutput
{
    float4 position : SV_POSITION;
};

VSOutput LocalShadowVS(VSInput input)
{
    VSOutput output;
    output.position =
        mul(
            float4(input.position, 1.0f),
            worldViewProjection);
    return output;
}
