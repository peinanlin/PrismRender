#include "Renderer/Features/Ocean/WaterMediumState.h"

#include <algorithm>
#include <cmath>

namespace Prism::Renderer
{
WaterMediumResult WaterMediumState::Update(
    const WaterMediumSample& source) noexcept
{
    const float meanSeaLevel = std::isfinite(source.meanSeaLevelMeters)
        ? source.meanSeaLevelMeters : 0.0f;
    const bool coherent = source.coherentQueryAvailable
        && std::isfinite(source.queriedDisplacementMeters);
    const float surfaceHeight = meanSeaLevel
        + (coherent ? source.queriedDisplacementMeters : 0.0f);
    const float cameraHeight = std::isfinite(source.cameraHeightMeters)
        ? source.cameraHeightMeters : surfaceHeight;
    const float hysteresis = std::clamp(
        std::isfinite(source.hysteresisMeters)
            ? source.hysteresisMeters : 0.12f,
        0.0f, 10.0f);

    WaterMediumResult result{};
    result.surfaceHeightMeters = surfaceHeight;
    result.queryVersion = coherent ? source.queryVersion : 0u;
    result.usedMeanSeaLevelFallback = !coherent;
    if (!m_initialized)
    {
        m_medium = cameraHeight < surfaceHeight
            ? WaterCameraMedium::Underwater
            : WaterCameraMedium::AboveWater;
        m_initialized = true;
        ++m_version;
        result.transitioned = true;
        result.reason = m_pendingResetReason;
        m_pendingResetReason = WaterMediumTransitionReason::None;
    }
    else
    {
        const WaterCameraMedium previous = m_medium;
        if (m_medium == WaterCameraMedium::AboveWater
            && cameraHeight < surfaceHeight - hysteresis)
        {
            m_medium = WaterCameraMedium::Underwater;
        }
        else if (m_medium == WaterCameraMedium::Underwater
            && cameraHeight > surfaceHeight + hysteresis)
        {
            m_medium = WaterCameraMedium::AboveWater;
        }
        if (m_medium != previous)
        {
            ++m_version;
            result.transitioned = true;
            result.reason = WaterMediumTransitionReason::CrossedWaterline;
        }
    }
    result.medium = m_medium;
    result.version = m_version;
    return result;
}

void WaterMediumState::Reset(const bool sceneChanged) noexcept
{
    m_initialized = false;
    m_medium = WaterCameraMedium::AboveWater;
    m_pendingResetReason = sceneChanged
        ? WaterMediumTransitionReason::SceneReset
        : WaterMediumTransitionReason::ExplicitReset;
    ++m_version;
}
} // namespace Prism::Renderer
