#pragma once

#include "Renderer/Features/Ocean/WaterOpticsSettings.h"

#include <cstdint>

namespace Prism::Renderer
{
struct WaterVolumetricHistoryKey
{
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    WaterOpticsQuality quality = WaterOpticsQuality::High;
    std::uint64_t opticalHistoryVersion = 0u;
    std::uint64_t surfaceHistoryVersion = 0u;
    std::uint64_t mediumVersion = 0u;
    std::uint64_t sceneVersion = 0u;

    bool operator==(const WaterVolumetricHistoryKey&) const noexcept = default;
};

class WaterVolumetricHistoryState
{
public:
    [[nodiscard]] bool Update(
        const WaterVolumetricHistoryKey& key) noexcept;
    void Commit() noexcept { m_valid = true; }
    void Reset() noexcept;

    [[nodiscard]] bool IsValid() const noexcept { return m_valid; }
    [[nodiscard]] std::uint64_t GetVersion() const noexcept
    {
        return m_version;
    }

private:
    WaterVolumetricHistoryKey m_key{};
    std::uint64_t m_version = 0u;
    bool m_hasKey = false;
    bool m_valid = false;
};

[[nodiscard]] bool RejectWaterVolumetricHistory(
    float currentDepthMeters,
    float previousDepthMeters,
    float rejectionThresholdMeters,
    bool motionValid,
    bool historyValid) noexcept;
} // namespace Prism::Renderer
