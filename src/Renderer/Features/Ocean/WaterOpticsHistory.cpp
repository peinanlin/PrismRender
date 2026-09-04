#include "Renderer/Features/Ocean/WaterOpticsHistory.h"

namespace Prism::Renderer
{
WaterOpticsHistoryInvalidation ClassifyWaterOpticsHistoryInvalidation(
    const WaterOpticsHistoryKey& previous,
    const WaterOpticsHistoryKey& current) noexcept
{
    WaterOpticsHistoryInvalidation result =
        WaterOpticsHistoryInvalidation::None;
    if (previous.width != current.width || previous.height != current.height)
        result = result | WaterOpticsHistoryInvalidation::Extent;
    if (previous.quality != current.quality)
        result = result | WaterOpticsHistoryInvalidation::Quality;
    if (previous.cameraCutVersion != current.cameraCutVersion)
        result = result | WaterOpticsHistoryInvalidation::CameraCut;
    if (previous.sceneVersion != current.sceneVersion)
        result = result | WaterOpticsHistoryInvalidation::Scene;
    if (previous.surfaceHistoryVersion != current.surfaceHistoryVersion)
        result = result | WaterOpticsHistoryInvalidation::SurfaceHistory;
    if (previous.explicitResetSerial != current.explicitResetSerial)
        result = result | WaterOpticsHistoryInvalidation::ExplicitReset;
    return result;
}

bool WaterOpticsHistoryState::Update(
    const WaterOpticsHistoryKey& key) noexcept
{
    if (!m_valid)
    {
        m_key = key;
        m_valid = true;
        if (m_version == 0u)
            m_version = 1u;
        m_lastInvalidation =
            WaterOpticsHistoryInvalidation::Initialization;
        return true;
    }

    m_lastInvalidation =
        ClassifyWaterOpticsHistoryInvalidation(m_key, key);
    if (m_lastInvalidation == WaterOpticsHistoryInvalidation::None)
        return false;

    m_key = key;
    ++m_version;
    return true;
}

void WaterOpticsHistoryState::Reset() noexcept
{
    m_valid = false;
    m_lastInvalidation = WaterOpticsHistoryInvalidation::ExplicitReset;
    ++m_version;
}
} // namespace Prism::Renderer
