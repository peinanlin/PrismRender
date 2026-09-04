#include "FluidCommon.hlsli"

PRISM_VK_BINDING(32)
RWStructuredBuffer<float4> particlePositions : register(u0);
PRISM_VK_BINDING(33)
RWStructuredBuffer<float4> predictedPositions : register(u1);
PRISM_VK_BINDING(34)
RWStructuredBuffer<float4> particleVelocities : register(u2);
PRISM_VK_BINDING(35)
RWStructuredBuffer<float4> velocityScratch : register(u3);
PRISM_VK_BINDING(36)
RWStructuredBuffer<float> particleLambdas : register(u4);
PRISM_VK_BINDING(37)
RWStructuredBuffer<float4> deltaOrCurl : register(u5);
PRISM_VK_BINDING(38)
RWStructuredBuffer<float> particleDensities : register(u6);

[numthreads(PBF_THREAD_GROUP_SIZE, 1, 1)]
void InitializeParticlesCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint particleIndex = dispatchThreadId.x;
    if (particleIndex >= PbfParticleCount())
    {
        return;
    }

    const bool doubleDam = spawnDimensions.w != 0u;
    const uint particlesPerDam =
        (PbfParticleCount() + 1u) / 2u;
    const bool rightDam = doubleDam
        && particleIndex >= particlesPerDam;
    const uint localIndex = rightDam
        ? particleIndex - particlesPerDam
        : particleIndex;
    const uint planeSize = spawnDimensions.x * spawnDimensions.y;
    const uint z = localIndex / planeSize;
    const uint planeIndex = localIndex - z * planeSize;
    const uint y = planeIndex / spawnDimensions.x;
    const uint x = planeIndex - y * spawnDimensions.x;
    const float3 dimensions = max(
        float3(spawnDimensions.xyz),
        float3(1.0f, 1.0f, 1.0f));
    const float3 uvw =
        (float3(x, y, z) + 0.5f) / dimensions;
    float3 spawnMinimum = spawnMinParticleMass.xyz;
    float3 spawnMaximum = spawnMaxRestDensity.xyz;
    if (doubleDam)
    {
        const float3 spawnExtent = spawnMaximum - spawnMinimum;
        const float damWidth = min(
            spawnExtent.z,
            spawnExtent.x * 0.45f);
        if (rightDam)
        {
            spawnMinimum.x = spawnMaximum.x - damWidth;
        }
        else
        {
            spawnMaximum.x = spawnMinimum.x + damWidth;
        }
    }
    const float3 position = PbfClampToDomain(
        lerp(spawnMinimum, spawnMaximum, uvw));

    particlePositions[particleIndex] = float4(position, 1.0f);
    predictedPositions[particleIndex] = float4(position, 1.0f);
    particleVelocities[particleIndex] = 0.0f;
    velocityScratch[particleIndex] = 0.0f;
    particleLambdas[particleIndex] = 0.0f;
    deltaOrCurl[particleIndex] = 0.0f;
    particleDensities[particleIndex] = 1.0f;
}

[numthreads(PBF_THREAD_GROUP_SIZE, 1, 1)]
void PredictPositionsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint particleIndex = dispatchThreadId.x;
    if (particleIndex >= PbfParticleCount())
    {
        return;
    }

    const float deltaTime = gravityDeltaTime.w;
    float3 velocity = particleVelocities[particleIndex].xyz;
    velocity += gravityDeltaTime.xyz * deltaTime;
    const float speed = length(velocity);
    if (speed > velocityParameters.w)
    {
        velocity *= velocityParameters.w / max(speed, 1.0e-6f);
    }
    const float3 predicted = PbfClampToDomain(
        particlePositions[particleIndex].xyz
            + velocity * deltaTime);
    particleVelocities[particleIndex] = float4(velocity, 0.0f);
    predictedPositions[particleIndex] = float4(predicted, 1.0f);
}
