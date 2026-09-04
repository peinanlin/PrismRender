#include "../ShaderBindings.hlsli"

struct ValidationSample
{
    float4 spectrum;
    float4 displacement;
    float4 gradient;
    float4 moments;
    float4 foam;
};

PRISM_VK_BINDING(0) cbuffer ValidationConstants : register(b0)
{
    uint sampleCount;
    uint3 validationPadding;
};
PRISM_VK_BINDING(16) Texture2DArray<float4> spectrumTexture
    : register(t0);
PRISM_VK_BINDING(17) Texture2DArray<float4> displacementTexture
    : register(t1);
PRISM_VK_BINDING(18) Texture2DArray<float4> gradientTexture
    : register(t2);
PRISM_VK_BINDING(19) Texture2DArray<float4> momentTexture
    : register(t3);
PRISM_VK_BINDING(20) Texture2DArray<float4> foamTexture
    : register(t4);
PRISM_VK_BINDING(21) StructuredBuffer<uint4> sampleCoordinates
    : register(t5);
PRISM_VK_BINDING(36) RWStructuredBuffer<ValidationSample> outputSamples
    : register(u4);

[numthreads(64, 1, 1)]
void SampleOceanSimulationCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if (index >= sampleCount)
    {
        return;
    }
    const uint4 coordinate = sampleCoordinates[index];
    ValidationSample result;
    result.spectrum = spectrumTexture.Load(
        int4(coordinate.xyz, 0));
    result.displacement = displacementTexture.Load(
        int4(coordinate.xyz, coordinate.w));
    result.gradient = gradientTexture.Load(
        int4(coordinate.xyz, coordinate.w));
    result.moments = momentTexture.Load(
        int4(coordinate.xyz, coordinate.w));
    result.foam = foamTexture.Load(
        int4(coordinate.xyz, coordinate.w));
    outputSamples[index] = result;
}
