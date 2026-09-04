#include "Renderer/Features/Ocean/OceanFeature.h"

namespace Prism::Renderer
{
void OceanFeature::Reset() noexcept
{
    m_settings = OceanSettings{};
    m_statistics = OceanStatistics{};
}

void OceanFeature::SetSettings(const OceanSettings& settings) noexcept
{
    m_settings = settings;
}

const OceanSettings& OceanFeature::GetSettings() const noexcept
{
    return m_settings;
}

const OceanStatistics& OceanFeature::GetStatistics() const noexcept
{
    return m_statistics;
}
} // namespace Prism::Renderer
