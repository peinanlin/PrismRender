#pragma once

#include "Renderer/Features/Ocean/OceanSettings.h"

#include <array>
#include <cstdint>

namespace Prism::Renderer
{
inline constexpr float OceanCascadeVisibilityPeriods = 30.0f;

struct OceanCascadeDescription
{
    float patchLengthMeters = 0.0f;
    float wavelengthMinMeters = 0.0f;
    float wavelengthMaxMeters = 0.0f;
    float uvScale = 0.0f;
    DirectX::XMFLOAT2 uvOffset{};
    float distanceFadeStartMeters = 0.0f;
    float distanceFadeEndMeters = 0.0f;
    std::uint32_t resolution = 0u;
};

class OceanSpectrumGenerator
{
public:
    // A radix-2 grid can represent a two-texel Nyquist wave. Keeping three
    // texels per shortest wave leaves a stable margin for derivatives and
    // mip filtering on both DXIL and SPIR-V backends.
    static constexpr float NyquistTexelsPerWave = 3.0f;
    static constexpr float FundamentalWavelengthMargin = 1.0f;
    // Matches the surface shader's long projected-footprint fade. Keeping
    // fine bands for roughly thirty periods avoids a smooth middle-distance
    // annulus while the derivative-selected mip suppresses aliasing.
    static constexpr float CascadeVisibilityPeriods = 30.0f;

    void Reset() noexcept;
    // Returns true only when the cached initial spectrum key changes.
    [[nodiscard]] bool Configure(const OceanSettings& settings,
        std::uint32_t randomSeed = 0x4f1bbcdcu) noexcept;
    [[nodiscard]] const std::array<OceanCascadeDescription, 4>&
        GetCascades() const noexcept { return m_cascades; }
    [[nodiscard]] std::array<float, 4> BandWeights(
        float wavelengthMeters) const noexcept;
    [[nodiscard]] static DirectX::XMFLOAT2 ComputeCascadeUv(
        const DirectX::XMFLOAT2& undisplacedWorldPosition,
        const OceanCascadeDescription& cascade,
        float warpAmplitude,
        float warpFrequency) noexcept;
    [[nodiscard]] static float DistanceFadeWeight(
        float cameraDistance,
        const OceanCascadeDescription& cascade) noexcept;
    [[nodiscard]] bool IsSpectrumDirty() const noexcept
    {
        return m_spectrumDirty;
    }
    void MarkSpectrumCommitted() noexcept { m_spectrumDirty = false; }
    [[nodiscard]] std::uint64_t SpectrumKey() const noexcept
    {
        return m_spectrumKey;
    }
    [[nodiscard]] std::uint32_t RandomSeed() const noexcept
    {
        return m_randomSeed;
    }

private:
    [[nodiscard]] static std::uint64_t BuildSpectrumKey(
        const OceanSettings& settings,
        std::uint32_t randomSeed) noexcept;

    std::array<OceanCascadeDescription, 4> m_cascades{};
    std::uint64_t m_spectrumKey = 0u;
    std::uint32_t m_randomSeed = 0u;
    bool m_configured = false;
    bool m_spectrumDirty = true;
};
} // namespace Prism::Renderer
