#include "../ShaderBindings.hlsli"

PRISM_VK_BINDING(16)
Texture2D<float> opaqueDepth : register(t0);

struct FullscreenOutput
{
    float4 position : SV_Position;
};

FullscreenOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenOutput output;
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(
        uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f),
        0.0f,
        1.0f);
    return output;
}

float WaterDepthCopyPS(float4 position : SV_Position) : SV_Depth
{
    return opaqueDepth.Load(int3(int2(position.xy), 0));
}
