#include "../../assets/shaders/ShaderBindings.hlsli"

PRISM_VK_BINDING(16) StructuredBuffer<float4> positions : register(t0);
PRISM_VK_BINDING(17) StructuredBuffer<float> densities : register(t1);
PRISM_VK_BINDING(32) RWStructuredBuffer<uint> snapshot : register(u0);

[numthreads(64, 1, 1)]
void SnapshotCS(uint3 thread : SV_DispatchThreadID)
{
    uint count, stride;
    positions.GetDimensions(count, stride);
    if (thread.x >= count) return;
    const uint4 position = asuint(positions[thread.x]);
    const uint base = thread.x * 5u;
    snapshot[base] = position.x;
    snapshot[base + 1u] = position.y;
    snapshot[base + 2u] = position.z;
    snapshot[base + 3u] = position.w;
    snapshot[base + 4u] = asuint(densities[thread.x]);
}
