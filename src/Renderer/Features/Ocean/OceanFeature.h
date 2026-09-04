#pragma once

#include "Renderer/Features/Ocean/OceanSettings.h"
#include "Renderer/Features/Ocean/OceanStatistics.h"

namespace Prism::Renderer
{
class OceanFeature
{
public:
    void Reset() noexcept;
    void SetSettings(const OceanSettings& settings) noexcept;
    [[nodiscard]] const OceanSettings& GetSettings() const noexcept;
    [[nodiscard]] const OceanStatistics& GetStatistics() const noexcept;

private:
    OceanSettings m_settings{};
    OceanStatistics m_statistics{};
};
} // namespace Prism::Renderer
