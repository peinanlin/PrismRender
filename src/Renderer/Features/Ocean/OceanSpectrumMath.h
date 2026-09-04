#pragma once

#include "Renderer/Features/Ocean/OceanSettings.h"

#include <cstdint>

namespace Prism::Renderer
{
inline constexpr float OceanInitialSpectrumAbsoluteTolerance = 2.0e-5f;
inline constexpr float OceanInitialSpectrumRelativeTolerance = 8.0e-4f;

struct OceanSpectrumSample
{
    float energy = 0.0f;
    float directionalWeight = 0.0f;
    float angularFrequency = 0.0f;
};

struct OceanInitialSpectrumSample
{
    float real = 0.0f;
    float imaginary = 0.0f;
    float energy = 0.0f;
    float angularFrequency = 0.0f;
};

struct OceanInitialSpectrumComponents
{
    DirectX::XMFLOAT2 baseH0{};
    DirectX::XMFLOAT2 swellH0{};
    float baseEnergy = 0.0f;
    float swellEnergy = 0.0f;
    float angularFrequency = 0.0f;
};

// WaveWorks interprets base_wind_speed as a Beaufort number when the matching
// simulation setting is enabled. Convert that UI value to the metres/second
// unit consumed by the project-owned JONSWAP evaluator.
[[nodiscard]] float BeaufortToMetersPerSecond(float beaufort) noexcept;

// Peak wavelength implied by the project JONSWAP parameterization. This is
// exposed for metadata validation without requiring a GPU spectrum build.
[[nodiscard]] float JonswapPeakWavelength(float windSpeed,
    float fetchMeters, float gravity = 9.81f) noexcept;

// Deep-water dispersion in radians/second for a wave-number magnitude in
// radians/metre.  The zero limit is explicitly defined to keep shader and
// CPU reference behavior finite and deterministic.
[[nodiscard]] float DeepWaterAngularFrequency(float waveNumber,
    float gravity = 9.81f);

// Directional spreading used by both the base-wind and swell spectra.  The
// dot product is clamped so opposing directions remain finite and non-negative.
[[nodiscard]] float DirectionalSpreading(float directionDot,
    float dependency);

// Self-contained JONSWAP energy evaluation.  Fetch is expressed in metres,
// while the public settings retain the WaveWorks-style kilometre UI unit.
[[nodiscard]] float JonswapEnergy(float waveNumber,
    float windSpeed,
    float fetchMeters,
    float spectrumPeaking,
    float smallWavesCutoffLength,
    float smallWavesCutoffPower,
    float amplitudeMultiplier,
    float gravity = 9.81f);

[[nodiscard]] OceanSpectrumSample EvaluateSpectrum(
    float waveNumber,
    float directionDot,
    const OceanWindSpectrumSettings& wind,
    float gravity = 9.81f);

[[nodiscard]] OceanSpectrumSample EvaluateSpectrum(
    float waveNumber,
    float directionDot,
    const OceanSwellSettings& swell,
    float gravity = 9.81f);

// Deterministic Box-Muller sample shared conceptually with the Slang kernel.
// The integer hash deliberately uses only uint32 arithmetic so D3D12, Vulkan,
// and the CPU reference receive the same random pair for a texel and seed.
[[nodiscard]] DirectX::XMFLOAT2 DeterministicGaussian(
    std::uint32_t coordinateX,
    std::uint32_t coordinateY,
    std::uint32_t cascadeIndex,
    std::uint32_t seed) noexcept;

// Evaluates one cached h0 texel. The patch length determines wave number,
// bandWeight partitions total energy across cascades, and the returned complex
// coefficient is scaled for the normalized inverse transform used by OceanFft.
[[nodiscard]] OceanInitialSpectrumSample EvaluateInitialSpectrumTexel(
    const OceanSettings& settings,
    float patchLengthMeters,
    float bandWeight,
    std::uint32_t resolution,
    std::uint32_t coordinateX,
    std::uint32_t coordinateY,
    std::uint32_t cascadeIndex,
    std::uint32_t seed,
    float gravity = 9.81f);

// Returns independently seeded base-wind and swell coefficients. This is the
// canonical layout of the cached RGBA32F h0 array: RG=base and BA=swell.
[[nodiscard]] OceanInitialSpectrumComponents
EvaluateInitialSpectrumComponents(
    const OceanSettings& settings,
    float patchLengthMeters,
    float bandWeight,
    std::uint32_t resolution,
    std::uint32_t coordinateX,
    std::uint32_t coordinateY,
    std::uint32_t cascadeIndex,
    std::uint32_t seed,
    float gravity = 9.81f);
} // namespace Prism::Renderer
