#include "ShaderBindings.hlsli"

PRISM_VK_BINDING(0) cbuffer VarianceShadowConstants : register(b0)
{
    uint directionX;
    uint directionY;
    uint exponential;
    uint padding;
};

PRISM_VK_BINDING(16)
Texture2DArray<float> shadowDepth : register(t0);
PRISM_VK_BINDING(17)
Texture2DArray<float4> inputMoments : register(t1);
PRISM_VK_BINDING(32)
RWTexture2DArray<float4> outputMoments : register(u0);

float4 EncodeMoments(float depth)
{
    depth = saturate(depth);
    if (exponential == 0u)
    {
        return float4(
            depth,
            depth * depth,
            0.0f,
            0.0f);
    }
    const float exponent = 5.0f;
    const float positive = exp(exponent * depth);
    const float negative = -exp(-exponent * depth);
    return float4(
        positive,
        positive * positive,
        negative,
        negative * negative);
}

[numthreads(8, 8, 1)]
void ConvertShadowMomentsCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint outputWidth = 0;
    uint outputHeight = 0;
    uint outputLayers = 0;
    outputMoments.GetDimensions(
        outputWidth,
        outputHeight,
        outputLayers);
    if (dispatchThreadId.x >= outputWidth
        || dispatchThreadId.y >= outputHeight
        || dispatchThreadId.z >= outputLayers)
    {
        return;
    }
    uint depthWidth = 0;
    uint depthHeight = 0;
    uint depthLayers = 0;
    uint depthLevels = 0;
    shadowDepth.GetDimensions(
        0,
        depthWidth,
        depthHeight,
        depthLayers,
        depthLevels);
    // Preserve both the first and second moments while reducing the shadow
    // depth map. Sampling only the center depth aliases a receiver/blocker
    // edge whenever the cascade moves by a fraction of a moments texel.
    const uint2 outputPixel = dispatchThreadId.xy;
    const uint2 outputExtent = uint2(outputWidth, outputHeight);
    const uint2 depthExtent = uint2(depthWidth, depthHeight);
    const uint2 sourceBegin = outputPixel * depthExtent / outputExtent;
    const uint2 sourceEnd = min(
        max(
            ((outputPixel + uint2(1u, 1u)) * depthExtent
                + outputExtent - uint2(1u, 1u)) / outputExtent,
            sourceBegin + uint2(1u, 1u)),
        depthExtent);

    float4 accumulatedMoments = 0.0f.xxxx;
    float sampleCount = 0.0f;
    [loop]
    for (uint y = sourceBegin.y; y < sourceEnd.y; ++y)
    {
        [loop]
        for (uint x = sourceBegin.x; x < sourceEnd.x; ++x)
        {
            const float depth = shadowDepth.Load(
                int4(uint2(x, y), dispatchThreadId.z, 0));
            accumulatedMoments += EncodeMoments(depth);
            sampleCount += 1.0f;
        }
    }
    outputMoments[dispatchThreadId] =
        accumulatedMoments / max(sampleCount, 1.0f);
}

[numthreads(8, 8, 1)]
void BlurShadowMomentsCS(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width = 0;
    uint height = 0;
    uint layers = 0;
    outputMoments.GetDimensions(width, height, layers);
    if (dispatchThreadId.x >= width
        || dispatchThreadId.y >= height
        || dispatchThreadId.z >= layers)
    {
        return;
    }
    static const float weights[5] = {
        0.06136f,
        0.24477f,
        0.38774f,
        0.24477f,
        0.06136f};
    float4 result = 0.0f.xxxx;
    [unroll]
    for (int offset = -2; offset <= 2; ++offset)
    {
        const int2 sourcePixel = clamp(
            int2(dispatchThreadId.xy)
                + int2(directionX, directionY) * offset,
            int2(0, 0),
            int2(width, height) - 1);
        result += inputMoments.Load(
            int4(sourcePixel, dispatchThreadId.z, 0))
            * weights[offset + 2];
    }
    outputMoments[dispatchThreadId] = result;
}
