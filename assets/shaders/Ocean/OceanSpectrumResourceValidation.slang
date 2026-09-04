#include "../ShaderBindings.hlsli"

PRISM_VK_BINDING(16) Texture2DArray<float4> spectrumRead : register(t0);
PRISM_VK_BINDING(32) RWTexture2DArray<float4> spectrumWrite : register(u0);
PRISM_VK_BINDING(36) RWStructuredBuffer<float4> validationOutput
    : register(u4);

[numthreads(8, 8, 1)]
void WriteSpectrumSlicesCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    uint layers;
    spectrumWrite.GetDimensions(width, height, layers);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height
        || dispatchThreadId.z >= layers)
    {
        return;
    }
    spectrumWrite[dispatchThreadId] = float4(
        float(dispatchThreadId.z + 1u),
        float(dispatchThreadId.x) / float(width),
        float(dispatchThreadId.y) / float(height),
        float(100u + dispatchThreadId.z));
}

[numthreads(1, 1, 1)]
void ReadSpectrumSlicesCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    uint layers;
    uint mipLevels;
    spectrumRead.GetDimensions(0u, width, height, layers, mipLevels);
    if (dispatchThreadId.z >= layers)
    {
        return;
    }
    const uint2 coordinate = uint2(min(5u, width - 1u), min(7u, height - 1u));
    validationOutput[dispatchThreadId.z] = spectrumRead.Load(
        int4(coordinate, dispatchThreadId.z, 0));
}
