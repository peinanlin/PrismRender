#include "ShaderBindings.hlsli"

PRISM_VK_BINDING(16) Texture2D<float> sourceDepth : register(t0);
PRISM_VK_BINDING(32) RWTexture2D<float> outputHiZ : register(u0);

bool IsOutputPixelInBounds(uint2 pixel)
{
    uint width = 0;
    uint height = 0;
    outputHiZ.GetDimensions(width, height);
    return pixel.x < width && pixel.y < height;
}

[numthreads(8, 8, 1)]
void CopyDepthCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (!IsOutputPixelInBounds(dispatchThreadId.xy))
    {
        return;
    }
    outputHiZ[dispatchThreadId.xy] =
        sourceDepth.Load(int3(dispatchThreadId.xy, 0));
}

[numthreads(8, 8, 1)]
void DownsampleDepthCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (!IsOutputPixelInBounds(dispatchThreadId.xy))
    {
        return;
    }

    uint sourceWidth = 0;
    uint sourceHeight = 0;
    sourceDepth.GetDimensions(sourceWidth, sourceHeight);
    uint2 sourceBase = dispatchThreadId.xy * 2u;
    float farthestDepth = 0.0f;
    [unroll]
    for (uint y = 0; y < 2; ++y)
    {
        [unroll]
        for (uint x = 0; x < 2; ++x)
        {
            uint2 sourcePixel = min(
                sourceBase + uint2(x, y),
                uint2(sourceWidth - 1u, sourceHeight - 1u));
            farthestDepth = max(
                farthestDepth,
                sourceDepth.Load(int3(sourcePixel, 0)));
        }
    }
    outputHiZ[dispatchThreadId.xy] = farthestDepth;
}
