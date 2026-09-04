#include "FluidCommon.hlsli"

PRISM_VK_BINDING(32)
RWStructuredBuffer<float4> predictedPositions : register(u0);
PRISM_VK_BINDING(33)
RWStructuredBuffer<float> particleLambdas : register(u1);
PRISM_VK_BINDING(34)
RWStructuredBuffer<float4> deltaPositions : register(u2);
PRISM_VK_BINDING(35)
RWStructuredBuffer<uint> neighborCounts : register(u3);
PRISM_VK_BINDING(36)
RWStructuredBuffer<uint> neighborParticleIndices : register(u4);
PRISM_VK_BINDING(37)
RWStructuredBuffer<float> particleDensities : register(u5);

[numthreads(PBF_THREAD_GROUP_SIZE, 1, 1)]
void ComputeLambdaCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint particleIndex = dispatchThreadId.x;
    if (particleIndex >= PbfParticleCount())
    {
        return;
    }

    const float3 position = predictedPositions[particleIndex].xyz;
    const float smoothingRadiusSquared =
        PbfSmoothingRadius() * PbfSmoothingRadius();
    const uint capacity = PbfMaxNeighborsPerParticle();
    const uint count = neighborCounts[particleIndex];
    const uint baseIndex = particleIndex * capacity;
    float density = PbfParticleMass() * PbfPoly6(0.0f);
    float3 selfGradient = 0.0f;
    float squaredGradientSum = 0.0f;

    [loop]
    for (uint slot = 0u; slot < count; ++slot)
    {
        const uint neighborIndex =
            neighborParticleIndices[baseIndex + slot];
        const float3 difference =
            position - predictedPositions[neighborIndex].xyz;
        const float squaredDistance = dot(difference, difference);
        if (squaredDistance >= smoothingRadiusSquared)
        {
            continue;
        }
        const float kernel = PbfPoly6(squaredDistance);
        density += PbfParticleMass() * kernel;
        const float3 gradient =
            PbfParticleMass()
            * PbfInverseRestDensity()
            * PbfSpikyGradient(difference, squaredDistance);
        selfGradient += gradient;
        squaredGradientSum += dot(gradient, gradient);
    }

    squaredGradientSum +=
        dot(selfGradient, selfGradient);
    const float normalizedDensity =
        density * PbfInverseRestDensity();
    particleDensities[particleIndex] = normalizedDensity;
    const float densityConstraint = max(normalizedDensity - 1.0f, 0.0f);
    particleLambdas[particleIndex] =
        -densityConstraint
        / (squaredGradientSum + solverParameters.x);
}

[numthreads(PBF_THREAD_GROUP_SIZE, 1, 1)]
void ComputeDeltaCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint particleIndex = dispatchThreadId.x;
    if (particleIndex >= PbfParticleCount())
    {
        return;
    }

    const float3 position = predictedPositions[particleIndex].xyz;
    const float smoothingRadiusSquared =
        PbfSmoothingRadius() * PbfSmoothingRadius();
    const uint capacity = PbfMaxNeighborsPerParticle();
    const uint count = neighborCounts[particleIndex];
    const uint baseIndex = particleIndex * capacity;
    const float lambda = particleLambdas[particleIndex];
    float3 delta = 0.0f;

    [loop]
    for (uint slot = 0u; slot < count; ++slot)
    {
        const uint neighborIndex =
            neighborParticleIndices[baseIndex + slot];
        const float3 difference =
            position - predictedPositions[neighborIndex].xyz;
        const float squaredDistance = dot(difference, difference);
        if (squaredDistance >= smoothingRadiusSquared)
        {
            continue;
        }
        const float kernel = PbfPoly6(squaredDistance);
        const float ratio = max(
            kernel / max(kernelParameters.z, 1.0e-8f),
            0.0f);
        const float squaredRatio = ratio * ratio;
        const float pressurePower =
            abs(solverParameters.w - 4.0f) < 1.0e-4f
            ? squaredRatio * squaredRatio
            : pow(ratio, solverParameters.w);
        const float artificialPressure =
            -solverParameters.y * pressurePower;
        delta +=
            (lambda
                + particleLambdas[neighborIndex]
                + artificialPressure)
            * PbfParticleMass()
            * PbfInverseRestDensity()
            * PbfSpikyGradient(difference, squaredDistance);
    }

    const float deltaLength = length(delta);
    if (deltaLength > correctionParameters.x)
    {
        delta *= correctionParameters.x
            / max(deltaLength, 1.0e-6f);
    }
    deltaPositions[particleIndex] = float4(delta, 0.0f);
}

[numthreads(PBF_THREAD_GROUP_SIZE, 1, 1)]
void ApplyDeltaCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint particleIndex = dispatchThreadId.x;
    if (particleIndex >= PbfParticleCount())
    {
        return;
    }
    const float3 corrected = PbfClampToDomain(
        predictedPositions[particleIndex].xyz
            + deltaPositions[particleIndex].xyz);
    predictedPositions[particleIndex] =
        float4(corrected, 1.0f);
}
