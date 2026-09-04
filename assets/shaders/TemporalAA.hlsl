#include "ShaderBindings.hlsli"

PRISM_VK_BINDING(0) cbuffer TemporalConstants : register(b0)
{
    float2 resolution;
    float2 inverseResolution;
    float feedback;
    uint historyValid;
    uint temporalEnabled;
    uint padding;
};

PRISM_VK_BINDING(16)
Texture2D<float4> currentColor : register(t0);
PRISM_VK_BINDING(17)
Texture2D<float2> motionVectors : register(t1);
PRISM_VK_BINDING(18)
Texture2D<float4> historyColor : register(t2);
PRISM_VK_BINDING(32)
RWTexture2D<float4> resolvedColor : register(u0);
PRISM_VK_BINDING(33)
RWTexture2D<float4> outputHistory : register(u1);

[numthreads(8, 8, 1)]
void TemporalResolveCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= (uint)resolution.x
        || pixel.y >= (uint)resolution.y)
    {
        return;
    }

    const float4 current =
        currentColor.Load(int3(pixel, 0));
    float4 result = current;
    if (temporalEnabled != 0
        && historyValid != 0)
    {
        const float2 motion =
            motionVectors.Load(int3(pixel, 0));
        const float2 currentUv =
            (float2(pixel) + 0.5)
            * inverseResolution;
        const float2 previousUv =
            currentUv - motion;
        if (all(previousUv >= 0.0)
            && all(previousUv <= 1.0))
        {
            const uint2 previousPixel = min(
                uint2(
                    previousUv
                    * resolution),
                uint2(resolution) - 1);
            float3 neighborhoodMin =
                current.rgb;
            float3 neighborhoodMax =
                current.rgb;
            [unroll]
            for (int y = -1; y <= 1; ++y)
            {
                [unroll]
                for (int x = -1; x <= 1; ++x)
                {
                    const int2 samplePixel =
                        clamp(
                            int2(pixel)
                                + int2(x, y),
                            int2(0, 0),
                            int2(resolution) - 1);
                    const float3 sampleColor =
                        currentColor.Load(
                            int3(samplePixel, 0)).rgb;
                    neighborhoodMin = min(
                        neighborhoodMin,
                        sampleColor);
                    neighborhoodMax = max(
                        neighborhoodMax,
                        sampleColor);
                }
            }
            float4 history =
                historyColor.Load(
                    int3(previousPixel, 0));
            history.rgb = clamp(
                history.rgb,
                neighborhoodMin,
                neighborhoodMax);
            const float velocity =
                length(motion * resolution);
            const float historyWeight =
                saturate(
                    feedback
                    - velocity * 0.02);
            result = lerp(
                current,
                history,
                historyWeight);
        }
    }
    resolvedColor[pixel] = result;
    outputHistory[pixel] = result;
}
