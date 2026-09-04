#pragma once

#include <cstdint>

namespace Prism::Renderer
{
struct FluidStatistics
{
    std::uint32_t particleCount = 0;
    std::uint32_t gridCellCount = 0;
    std::uint32_t maxParticlesPerCell = 0;
    std::uint32_t maxNeighborsPerParticle = 0;
    std::uint32_t solverIterations = 0;
    std::uint32_t substepCount = 0;
    std::uint32_t particleThreadGroups = 0;
    std::uint32_t cellThreadGroups = 0;
    std::uint32_t dispatchCount = 0;

    std::uint32_t gridOverflowCount = 0;
    std::uint32_t neighborOverflowCount = 0;
    std::uint32_t maximumCellOccupancy = 0;
    std::uint32_t invalidParticleCount = 0;
    std::uint64_t allocatedBufferBytes = 0;

    bool diagnosticsValid = false;
    bool resourceRebuiltThisFrame = false;
    bool simulationScheduled = false;
};
} // namespace Prism::Renderer
