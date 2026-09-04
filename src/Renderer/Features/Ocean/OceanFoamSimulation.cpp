#include "Renderer/Features/Ocean/OceanFoamSimulation.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Prism::Renderer
{
void OceanFoamSimulation::Update(std::span<float> history,
    const std::span<const float> folding,
    const OceanFoamSettings& settings,
    const float deltaSeconds) noexcept
{
    if (history.size() != folding.size())
    {
        return;
    }
    const float dt = std::isfinite(deltaSeconds)
        ? std::clamp(deltaSeconds, 0.0f, 1.0f) : 0.0f;
    const float frameScale = dt * ReferenceSimulationRate;
    const float temporalRetention = std::pow(
        std::clamp(settings.falloffSpeed, 0.0001f, 1.0f),
        frameScale);
    const float spatialBlend = 1.0f - std::pow(
        std::max(1.0f
                - std::clamp(settings.dissipationSpeed, 0.0f, 1.0f)
                    * SpatialDissipationPerStep,
            0.001f),
        frameScale);
    std::vector<float> previousHistory(history.begin(), history.end());
    for (std::size_t index = 0; index < history.size(); ++index)
    {
        const float fold = std::isfinite(folding[index])
            ? std::max(folding[index], 0.0f) : 0.0f;
        const float generationSpan = std::max(
            1.0f - settings.generationThreshold, 1.0e-4f);
        const float generationCandidate = std::clamp(
            (fold - settings.generationThreshold) / generationSpan,
            0.0f, 1.0f);
        const float generated = generationCandidate
            * std::max(settings.generationAmount, 0.0f)
            * frameScale;
        const auto sanitized = [&previousHistory](const std::size_t sample)
        {
            const float value = previousHistory[sample];
            return std::isfinite(value)
                ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
        };
        const float previous = sanitized(index);
        const float neighborAverage = history.size() > 1u
            ? 0.5f * (sanitized(
                           (index + history.size() - 1u) % history.size())
                    + sanitized((index + 1u) % history.size()))
            : previous;
        const float dissipated = std::lerp(
            previous, neighborAverage, std::clamp(spatialBlend, 0.0f, 1.0f));
        const float candidate = dissipated * temporalRetention + generated;
        history[index] = std::isfinite(candidate)
            ? std::clamp(candidate, 0.0f, 1.0f) : 0.0f;
    }
    ++m_historyVersion;
}
} // namespace Prism::Renderer
