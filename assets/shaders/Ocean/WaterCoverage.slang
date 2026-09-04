#include "../ShaderBindings.hlsli"

PRISM_VK_BINDING(16) Texture2D<float4> coverageMask : register(t0);
PRISM_VK_BINDING(32) RWStructuredBuffer<uint> coverageTiles : register(u0);
groupshared uint coverageCount[256];

[numthreads(16, 16, 1)]
void WaterCoverageCS(uint3 id : SV_DispatchThreadID,
    uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint width, height;
    coverageMask.GetDimensions(width, height);
    coverageCount[lane] = id.x < width && id.y < height
        && coverageMask.Load(int3(id.xy, 0)).a > 0.0f ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint stride = 128u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride) coverageCount[lane] += coverageCount[lane + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0u)
        coverageTiles[group.y * ((width + 15u) / 16u) + group.x] = coverageCount[0];
}
