#include "Renderer/Features/Ocean/WaterVolumetricHistory.h"

#include <algorithm>
#include <cmath>

namespace Prism::Renderer
{
bool WaterVolumetricHistoryState::Update(
    const WaterVolumetricHistoryKey& key) noexcept
{
    if (!m_hasKey || key != m_key)
    {
        m_key = key;
        m_hasKey = true;
        m_valid = false;
        ++m_version;
        return true;
    }
    return false;
}

void WaterVolumetricHistoryState::Reset() noexcept
{
    m_hasKey = false;
    m_valid = false;
    ++m_version;
}

bool RejectWaterVolumetricHistory(
    const float currentDepthMeters,
    const float previousDepthMeters,
    const float rejectionThresholdMeters,
    const bool motionValid,
    const bool historyValid) noexcept
{
    if (!historyValid || !motionValid
        || !std::isfinite(currentDepthMeters)
        || !std::isfinite(previousDepthMeters))
    {
        return true;
    }
    const float threshold = std::max(
        std::isfinite(rejectionThresholdMeters)
            ? rejectionThresholdMeters : 0.01f,
        0.001f);
    return std::abs(currentDepthMeters - previousDepthMeters) > threshold;
}
} // namespace Prism::Renderer
