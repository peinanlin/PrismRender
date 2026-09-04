cbuffer PerFrame : register(b0)
{
    float4x4 uViewProj;
};

cbuffer PerObject : register(b1)
{
    float4x4 uWorld;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 worldPos = mul(float4(input.position, 1.0f), uWorld);
    output.position = mul(worldPos, uViewProj);
    output.normal = input.normal;
    output.uv = input.uv;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return float4(input.uv, 0.5f, 1.0f);
}