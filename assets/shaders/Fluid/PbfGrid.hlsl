#include "FluidCommon.hlsli"

PRISM_VK_BINDING(32)
RWStructuredBuffer<float4> predictedPositions : register(u0);
PRISM_VK_BINDING(33)
RWStructuredBuffer<uint> cellParticleCounts : register(u1);
PRISM_VK_BINDING(34)
RWStructuredBuffer<uint> cellParticleIndices : register(u2);
PRISM_VK_BINDING(35)
RWStructuredBuffer<uint> neighborCounts : register(u3);
PRISM_VK_BINDING(36)
RWStructuredBuffer<uint> neighborParticleIndices : register(u4);
PRISM_VK_BINDING(37)
RWStructuredBuffer<uint> simulationDiagnostics : register(u5);

groupshared uint gridMaximumOccupancy[PBF_THREAD_GROUP_SIZE];
groupshared uint gridOverflowCounts[PBF_THREAD_GROUP_SIZE];

[numthreads(PBF_THREAD_GROUP_SIZE, 1, 1)]
void ClearGridCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if (index < PbfGridCellCount())
    {
        cellParticleCounts[index] = 0u;
    }
}

[numthreads(PBF_THREAD_GROUP_SIZE, 1, 1)]
void ClearDiagnosticsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x < 4u)
    {
        simulationDiagnostics[dispatchThreadId.x] = 0u;
    }
}

[numthreads(PBF_THREAD_GROUP_SIZE, 1, 1)]
void BuildGridCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint particleIndex = dispatchThreadId.x;
    if (particleIndex >= PbfParticleCount())
    {
        return;
    }

    const uint cellIndex = PbfFlattenCell(
        PbfPositionToCell(
            predictedPositions[particleIndex].xyz));
    uint bucketIndex = 0u;
    InterlockedAdd(
        cellParticleCounts[cellIndex],
        1u,
        bucketIndex);
    if (bucketIndex < gridDimensions.w)
    {
        cellParticleIndices[
            cellIndex * gridDimensions.w
                + bucketIndex] = particleIndex;
    }
}

[numthreads(PBF_THREAD_GROUP_SIZE, 1, 1)]
void SortGridCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint cellIndex = dispatchThreadId.x;
    if (cellIndex >= PbfGridCellCount()) return;
    const uint count = min(cellParticleCounts[cellIndex], gridDimensions.w);
    const uint base = cellIndex * gridDimensions.w;
    // Atomic insertion has no stable order. One thread owns each bucket and
    // orders the retained IDs before any floating-point neighbor reduction.
    // Capacity overflow is still reported; sorting does not recover lost IDs.
    [loop]
    for (uint slot = 1u; slot < count; ++slot)
    {
        const uint particle = cellParticleIndices[base + slot];
        uint destination = slot;
        [loop]
        while (destination > 0u)
        {
            const uint previous = cellParticleIndices[base + destination - 1u];
            if (previous < particle) break;
            cellParticleIndices[base + destination] = previous;
            --destination;
        }
        cellParticleIndices[base + destination] = particle;
    }
}

[numthreads(PBF_THREAD_GROUP_SIZE, 1, 1)]
void BuildNeighborsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint particleIndex = dispatchThreadId.x;
    if (particleIndex >= PbfParticleCount())
    {
        return;
    }

    const float3 position = predictedPositions[particleIndex].xyz;
    const int3 centerCell = PbfPositionToCell(position);
    const float smoothingRadiusSquared =
        PbfSmoothingRadius() * PbfSmoothingRadius();
    const uint capacity = PbfMaxNeighborsPerParticle();
    uint neighborCount = 0u;

    [unroll]
    for (int z = -1; z <= 1; ++z)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int x = -1; x <= 1; ++x)
            {
                const int3 neighborCell =
                    centerCell + int3(x, y, z);
                if (!PbfCellInBounds(neighborCell))
                {
                    continue;
                }
                const uint cellIndex =
                    PbfFlattenCell(neighborCell);
                const uint count = min(
                    cellParticleCounts[cellIndex],
                    gridDimensions.w);
                [loop]
                for (uint slot = 0u; slot < count; ++slot)
                {
                    const uint neighborIndex =
                        cellParticleIndices[
                            cellIndex * gridDimensions.w + slot];
                    if (neighborIndex == particleIndex)
                    {
                        continue;
                    }
                    const float3 difference = position
                        - predictedPositions[neighborIndex].xyz;
                    const float squaredDistance =
                        dot(difference, difference);
                    if (squaredDistance >= smoothingRadiusSquared)
                    {
                        continue;
                    }
                    if (neighborCount < capacity)
                    {
                        neighborParticleIndices[
                            particleIndex * capacity
                                + neighborCount] = neighborIndex;
                    }
                    ++neighborCount;
                }
            }
        }
    }

    neighborCounts[particleIndex] = min(neighborCount, capacity);
    if (neighborCount > capacity && PbfDiagnosticsEnabled())
    {
        InterlockedAdd(simulationDiagnostics[3], 1u);
    }
}

[numthreads(PBF_THREAD_GROUP_SIZE, 1, 1)]
void ReduceGridDiagnosticsCS(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupThreadId : SV_GroupThreadID)
{
    const uint lane = groupThreadId.x;
    const uint count = dispatchThreadId.x < PbfGridCellCount()
        ? cellParticleCounts[dispatchThreadId.x]
        : 0u;
    gridMaximumOccupancy[lane] = count;
    gridOverflowCounts[lane] = count > gridDimensions.w
        ? count - gridDimensions.w
        : 0u;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = PBF_THREAD_GROUP_SIZE / 2u;
         stride > 0u;
         stride >>= 1u)
    {
        if (lane < stride)
        {
            gridMaximumOccupancy[lane] = max(
                gridMaximumOccupancy[lane],
                gridMaximumOccupancy[lane + stride]);
            gridOverflowCounts[lane] +=
                gridOverflowCounts[lane + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u)
    {
        InterlockedMax(
            simulationDiagnostics[1],
            gridMaximumOccupancy[0]);
        if (gridOverflowCounts[0] > 0u)
        {
            InterlockedAdd(
                simulationDiagnostics[0],
                gridOverflowCounts[0]);
        }
    }
}
