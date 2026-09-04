#pragma once

#include <cstdint>

namespace Prism::Renderer
{
enum class WaterCameraMedium : std::uint32_t
{
    AboveWater = 0u,
    Underwater = 1u
};

enum class WaterMediumTransitionReason : std::uint32_t
{
    None = 0u,
    Initialized,
    CrossedWaterline,
    SceneReset,
    ExplicitReset
};

struct WaterMediumSample
{
    float cameraHeightMeters = 0.0f;
    float meanSeaLevelMeters = 0.0f;
    float queriedDisplacementMeters = 0.0f;
    float hysteresisMeters = 0.12f;
    std::uint64_t queryVersion = 0u;
    bool coherentQueryAvailable = false;
};

struct WaterMediumResult
{
    WaterCameraMedium medium = WaterCameraMedium::AboveWater;
    WaterMediumTransitionReason reason = WaterMediumTransitionReason::None;
    float surfaceHeightMeters = 0.0f;
    std::uint64_t version = 0u;
    std::uint64_t queryVersion = 0u;
    bool usedMeanSeaLevelFallback = true;
    bool transitioned = false;
};

class WaterMediumState
{
public:
    [[nodiscard]] WaterMediumResult Update(
        const WaterMediumSample& sample) noexcept;
    void Reset(bool sceneChanged = false) noexcept;

    [[nodiscard]] WaterCameraMedium GetMedium() const noexcept
    {
        return m_medium;
    }
    [[nodiscard]] std::uint64_t GetVersion() const noexcept
    {
        return m_version;
    }

private:
    WaterCameraMedium m_medium = WaterCameraMedium::AboveWater;
    WaterMediumTransitionReason m_pendingResetReason =
        WaterMediumTransitionReason::Initialized;
    std::uint64_t m_version = 0u;
    bool m_initialized = false;
};
} // namespace Prism::Renderer
