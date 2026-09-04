#pragma once

#include "Renderer/Features/Ocean/OceanSettings.h"

#include <span>
#include <cstdint>

namespace Prism::Renderer
{
class OceanFoamSimulation
{
public:
    // WaveWorks exposes per-simulation-step generation/falloff controls. The
    // CPU oracle and GPU shader normalize them to this reference cadence.
    static constexpr float ReferenceSimulationRate = 60.0f;
    static constexpr float SpatialDissipationPerStep = 0.25f;

    void Reset() noexcept { m_historyVersion = 0u; }

    // CPU reference for the persistent spectral foam recurrence. GPU history
    // uses the same equation and thresholds, so this path is also a portable
    // validation oracle for D3D12/Vulkan shader results.
    void Update(std::span<float> history,
        std::span<const float> folding,
        const OceanFoamSettings& settings,
        float deltaSeconds) noexcept;

    [[nodiscard]] std::uint64_t HistoryVersion() const noexcept
    {
        return m_historyVersion;
    }

private:
    std::uint64_t m_historyVersion = 0u;
};
} // namespace Prism::Renderer
