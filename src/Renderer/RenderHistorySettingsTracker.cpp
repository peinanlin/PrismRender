#include "Renderer/RenderHistorySettingsTracker.h"

#include "Renderer/Features/Ocean/OceanSettings.h"
#include "Renderer/Features/Ocean/WaterOpticsSettings.h"

#include <limits>
#include <stdexcept>

namespace Prism::Renderer
{
namespace
{
bool InvalidatesHistory(
    const RenderSettings& previous,
    const RenderSettings& current) noexcept
{
    const bool viewPipelineChanged =
        previous.viewportShadingMode != current.viewportShadingMode
        || previous.deferredRenderingEnabled
            != current.deferredRenderingEnabled
        || previous.hdrEnabled != current.hdrEnabled
        || previous.temporalAntiAliasingEnabled
            != current.temporalAntiAliasingEnabled
        || previous.gtaoEnabled != current.gtaoEnabled
        || previous.screenSpaceReflectionsEnabled
            != current.screenSpaceReflectionsEnabled
        || previous.planarReflectionsEnabled
            != current.planarReflectionsEnabled;
    const bool oceanPipelineChanged =
        previous.fftOceanEnabled != current.fftOceanEnabled
        || previous.ocean.implementation
            != current.ocean.implementation
        || previous.ocean.opticsModel != current.ocean.opticsModel
        || previous.ocean.quality != current.ocean.quality
        || previous.ocean.debug.renderWater
            != current.ocean.debug.renderWater
        || previous.ocean.debug.historyResetSerial
            != current.ocean.debug.historyResetSerial
        || previous.ocean.debug.fullResetSerial
            != current.ocean.debug.fullResetSerial;
    const WaterOpticsDirtyScope waterScopes =
        ClassifyWaterOpticsDirtyScopes(
            previous.ocean.optics,
            current.ocean.optics);
    return viewPipelineChanged || oceanPipelineChanged
        || HasDirtyScope(
            waterScopes,
            WaterOpticsDirtyScope::History);
}
} // namespace

RenderHistorySettingsUpdate RenderHistorySettingsTracker::Observe(
    const RenderSettings& settings)
{
    if (!m_previous.has_value())
    {
        m_previous = settings;
        m_revision = 1;
        return {m_revision, false};
    }

    const bool invalidated = InvalidatesHistory(*m_previous, settings);
    m_previous = settings;
    if (invalidated)
    {
        if (m_revision == std::numeric_limits<std::uint64_t>::max())
        {
            throw std::overflow_error(
                "Render history settings revision capacity exhausted.");
        }
        ++m_revision;
    }
    return {m_revision, invalidated};
}

void RenderHistorySettingsTracker::Reset() noexcept
{
    m_previous.reset();
    m_revision = 0;
}
} // namespace Prism::Renderer
