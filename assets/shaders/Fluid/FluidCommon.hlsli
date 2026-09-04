#ifndef PRISM_FLUID_COMMON_HLSLI
#define PRISM_FLUID_COMMON_HLSLI

#include "../ShaderBindings.hlsli"

#define PBF_THREAD_GROUP_SIZE 64

PRISM_VK_BINDING(0) cbuffer PbfSimulationConstants : register(b0)
{
    float4 domainMinCellSize;
    float4 domainMaxParticleRadius;
    float4 spawnMinParticleMass;
    float4 spawnMaxRestDensity;
    float4 gravityDeltaTime;
    float4 solverParameters;
    float4 velocityParameters;
    float4 kernelParameters;
    float4 correctionParameters;
    float4 surfaceClassificationParameters;
    uint4 gridDimensions;
    uint4 simulationCounts;
    uint4 spawnDimensions;
    uint4 neighborParameters;
};

uint PbfParticleCount()
{
    return simulationCounts.x;
}

uint PbfGridCellCount()
{
    return simulationCounts.y;
}

float PbfSmoothingRadius()
{
    return domainMinCellSize.w;
}

float PbfParticleMass()
{
    return spawnMinParticleMass.w;
}

float PbfInverseRestDensity()
{
    return kernelParameters.w;
}

uint PbfMaxNeighborsPerParticle()
{
    return neighborParameters.x;
}

bool PbfDiagnosticsEnabled()
{
    return neighborParameters.y != 0u;
}

int3 PbfPositionToCell(float3 position)
{
    const float inverseCellSize = 1.0f / PbfSmoothingRadius();
    const int3 cell = (int3)floor(
        (position - domainMinCellSize.xyz) * inverseCellSize);
    return clamp(
        cell,
        int3(0, 0, 0),
        int3(gridDimensions.xyz) - int3(1, 1, 1));
}

bool PbfCellInBounds(int3 cell)
{
    return all(cell >= int3(0, 0, 0))
        && all(cell < int3(gridDimensions.xyz));
}

uint PbfFlattenCell(int3 cell)
{
    return (uint)cell.x
        + gridDimensions.x
            * ((uint)cell.y
                + gridDimensions.y * (uint)cell.z);
}

float PbfPoly6(float squaredDistance)
{
    const float radius = PbfSmoothingRadius();
    const float radiusSquared = radius * radius;
    if (squaredDistance >= radiusSquared)
    {
        return 0.0f;
    }
    const float difference = radiusSquared - squaredDistance;
    return kernelParameters.x
        * difference * difference * difference;
}

float3 PbfSpikyGradient(
    float3 difference,
    float squaredDistance)
{
    const float radius = PbfSmoothingRadius();
    const float radiusSquared = radius * radius;
    if (squaredDistance <= 1.0e-12f
        || squaredDistance >= radiusSquared)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float inverseDistance = rsqrt(squaredDistance);
    const float distance = squaredDistance * inverseDistance;
    const float kernelDistance = radius - distance;
    return kernelParameters.y
        * kernelDistance * kernelDistance
        * difference * inverseDistance;
}

float3 PbfSpikyGradient(float3 difference)
{
    return PbfSpikyGradient(
        difference,
        dot(difference, difference));
}

float3 PbfClampToDomain(float3 position)
{
    const float radius = domainMaxParticleRadius.w;
    const float3 lower = domainMinCellSize.xyz + radius;
    const float3 upper = domainMaxParticleRadius.xyz - radius;
    return clamp(position, lower, upper);
}

float3 PbfSafeNormalize(float3 value)
{
    const float squaredLength = dot(value, value);
    return squaredLength > 1.0e-12f
        ? value * rsqrt(squaredLength)
        : float3(0.0f, 0.0f, 0.0f);
}

#endif
