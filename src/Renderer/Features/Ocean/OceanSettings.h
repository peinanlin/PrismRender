#pragma once

#include "Renderer/Features/Ocean/WaterOpticsSettings.h"

#include <DirectXMath.h>

#include <cstdint>
#include <array>
#include <string>

namespace Prism::RHI { struct GraphicsDeviceCapabilities; }

namespace Prism::Renderer
{
enum class OceanImplementation : std::uint32_t
{
    LegacyFft = 0,
    SpectralOcean
};

enum class OceanSimulationQuality : std::uint32_t
{
    Normal = 0,
    High,
    Extreme
};

enum class OceanSimulationApi : std::uint32_t
{
    Cpu = 0,
    Compute
};

struct OceanWindSpectrumSettings
{
    DirectX::XMFLOAT2 direction{1.0f, 0.0f};
    float speed = 4.7f;
    float fetchKilometers = 0.1f;
    float dependency = 1.0f;
    float spectrumPeaking = 3.3f;
    float smallWavesCutoffLength = 0.0f;
    float smallWavesCutoffPower = 0.0f;
    float amplitudeMultiplier = 1.0f;
};

struct OceanSwellSettings
{
    DirectX::XMFLOAT2 direction{0.0f, 1.0f};
    float speed = 1.5f;
    float fetchKilometers = 520.0f;
    float dependency = 1.0f;
    float spectrumPeaking = 10.0f;
    float smallWavesCutoffLength = 60.0f;
    float smallWavesCutoffPower = 1.0f;
    float amplitudeMultiplier = 1.0f;
};

struct OceanFoamSettings
{
    float whitecapsThreshold = 0.5f;
    float generationThreshold = 0.37f;
    float generationAmount = 0.12f;
    float dissipationSpeed = 0.6f;
    float falloffSpeed = 0.985f;
};

struct OceanLocalWaveSettings
{
    bool enabled = true;
    bool demoEmittersEnabled = true;
    bool rainEmitterEnabled = false;
    bool wakeEmitterEnabled = true;
    bool shareSpectralTime = true;
    std::uint32_t emitterSeed = 0x13579bdfu;
    float rainRatePerSecond = 0.35f;
    float wakeSpeedMetersPerSecond = 12.0f;
    float domainSizeMeters = 200.0f;
    DirectX::XMFLOAT2 domainCenter{0.0f, 0.0f};
    std::uint32_t gridSize = 512u;
    float amplitudeMultiplier = 1.0f;
    float lateralMultiplier = 1.0f;
    DirectX::XMFLOAT2 manualPosition{0.0f, 0.0f};
    DirectX::XMFLOAT2 manualVelocityDirection{1.0f, 0.0f};
    float manualRadiusMeters = 5.0f;
    float manualStrength = 0.35f;
    std::uint32_t manualDisturbanceSerial = 0u;
    OceanFoamSettings foam{
        0.3f,
        0.6f,
        0.2f,
        0.3f,
        0.6f};
};

struct OceanGeometrySettings
{
    std::uint32_t cellsPerPatch = 64u;
    float minimumPatchLength = 5.0f;
    float maximumEdgeLengthPixels = 10.0f;
    float meanSeaLevel = 0.0f;
    std::uint32_t maximumLod = 15u;
    float geomorphingDegree = 1.0f;
    bool generateDiamondPattern = false;
    bool preferTessellation = true;
};

struct OceanShadingSettings
{
    bool useAtmosphere = true;
    bool useMicrofacetFresnel = true;
    bool useMicrofacetSpecular = true;
    bool useMicrofacetReflection = true;
    float sunAngleDegrees = 20.0f;
    float sunIntensity = 1.0f;
    float beckmannRoughness = 0.00001f;
    float uvWarpingAmplitude = 0.03f;
    float uvWarpingFrequency = 2.0f;
    DirectX::XMFLOAT3 deepWaterColor{0.0f, 0.2f, 0.4f};
    DirectX::XMFLOAT3 scatteringColor{0.0f, 0.7f, 0.6f};
    // xyz scales the deep-water color channels; w scales in-water scattering.
    // The reference sample's old 0.2 scattering multiplier left most troughs
    // near black after tone mapping, so the clean-room preset uses a brighter
    // daylight balance while retaining a dark deep-water base.
    DirectX::XMFLOAT4 waterColorIntensity{0.04f, 0.04f, 0.025f, 0.38f};
    DirectX::XMFLOAT3 foamColor{0.9f, 0.9f, 0.9f};
    DirectX::XMFLOAT3 underwaterFoamColor{0.6f, 0.6f, 0.6f};
};

struct OceanQuerySettings
{
    bool enableCpuReadback = true;
    bool enableGpuQueries = true;
    std::uint32_t readbackFifoEntries = 30u;
    bool enableCpuTimers = true;
    bool enableGpuTimers = true;
};

struct OceanDebugSettings
{
    static constexpr std::uint32_t CascadeCount = 4u;
    static constexpr std::uint32_t AllCascadeMask =
        (1u << CascadeCount) - 1u;

    bool simulateWater = true;
    bool renderWater = true;
    // Monotonic UI commands keep reset requests one-shot without mutating
    // physical ocean parameters. SceneRenderer consumes the counters.
    std::uint32_t fullResetSerial = 0u;
    std::uint32_t historyResetSerial = 0u;
    std::uint32_t localResetSerial = 0u;
    bool wireframe = false;
    bool showCascades = false;
    bool showFoamEnergy = false;
    bool showSlopeMoments = false;
    bool showGeometryLod = false;
    bool showNormals = false;

    // WaveWorks stage isolation is evaluated where the spectral maps are
    // consumed by the surface shader. Simulation/history resources continue
    // to advance so disabling a stage is stable and re-enabling it does not
    // introduce a cold history frame or change RenderGraph dependencies.
    bool spectralDisplacementEnabled = true;
    bool spectralGradientEnabled = true;
    bool spectralFoldingEnabled = true;
    bool spectralFoamHistoryEnabled = true;
    bool spectralMomentsEnabled = true;
    std::uint32_t cascadeMask = AllCascadeMask;
    std::uint32_t displacementCascadeMask = AllCascadeMask;
    std::uint32_t gradientCascadeMask = AllCascadeMask;
    std::uint32_t foldingCascadeMask = AllCascadeMask;
    std::uint32_t foamHistoryCascadeMask = AllCascadeMask;
    std::uint32_t momentsCascadeMask = AllCascadeMask;
};

// Exact-in-float bit layout carried by SharedObjectConstants. Both packed
// values stay below bit 24, so the numeric float round trip used by the
// existing object payload is lossless on DXIL and SPIR-V.
struct OceanRuntimeFlagLayout
{
    static constexpr std::uint32_t ShadingFresnelBit = 0u;
    static constexpr std::uint32_t ShadingSpecularBit = 1u;
    static constexpr std::uint32_t ShadingReflectionBit = 2u;
    static constexpr std::uint32_t ShadingAtmosphereBit = 3u;
    static constexpr std::uint32_t DisplacementCascadeFirstBit = 4u;
    static constexpr std::uint32_t GradientCascadeFirstBit = 8u;

    static constexpr std::uint32_t DebugCascadesBit = 0u;
    static constexpr std::uint32_t DebugFoamEnergyBit = 1u;
    static constexpr std::uint32_t DebugSlopeMomentsBit = 2u;
    static constexpr std::uint32_t DebugGeometryLodBit = 3u;
    static constexpr std::uint32_t DebugNormalsBit = 4u;
    static constexpr std::uint32_t FoldingCascadeFirstBit = 5u;
    static constexpr std::uint32_t FoamHistoryCascadeFirstBit = 9u;
    static constexpr std::uint32_t MomentsCascadeFirstBit = 13u;
};

static_assert(
    OceanRuntimeFlagLayout::MomentsCascadeFirstBit
        + OceanDebugSettings::CascadeCount <= 24u,
    "Ocean runtime flags must remain exactly representable as a float integer.");

struct OceanRuntimeFlags
{
    std::uint32_t primary = 0u;
    std::uint32_t secondary = 0u;
};

struct OceanSettings
{
    OceanImplementation implementation = OceanImplementation::LegacyFft;
    OceanOpticsModel opticsModel = OceanOpticsModel::Current;
    OceanSimulationQuality quality = OceanSimulationQuality::Extreme;
    OceanSimulationApi simulationApi = OceanSimulationApi::Compute;
    float simulationPeriodMeters = 1000.0f;
    float timeScale = 1.0f;
    float lateralMultiplier = 1.0f;
    bool useBeaufortScale = true;
    bool cameraFollowEnabled = true;
    bool asyncComputeEnabled = false;
    OceanWindSpectrumSettings baseWind{};
    OceanSwellSettings swell{};
    OceanFoamSettings foam{};
    OceanLocalWaveSettings local{};
    OceanGeometrySettings geometry{};
    OceanShadingSettings shading{};
    WaterOpticsSettings optics{};
    OceanQuerySettings query{};
    OceanDebugSettings debug{};

    // Returns the project-owned equivalent of the WaveWorks reference sample
    // controls.  The implementation remains self-contained and does not
    // depend on the WaveWorks SDK or its binary runtime.
    [[nodiscard]] static OceanSettings WaveWorksReference();

    // Reuses the proven large-area spectral/local-wave configuration and
    // changes only the visible-water optical model.
    [[nodiscard]] static OceanSettings HpWaterReference();

    // Clamps non-finite or physically invalid values before they reach a
    // shader.  The optional message is intended for the Ocean Lab warning
    // line and is empty when no adjustment was necessary.
    bool ValidateAndNormalize(std::string* message = nullptr);

    [[nodiscard]] static std::uint32_t ResolutionForQuality(
        OceanSimulationQuality quality);

    [[nodiscard]] static std::array<float, 4> ReferenceCascadePatchLengths();
};

[[nodiscard]] bool ShouldUseOceanTessellation(
    const OceanSettings& settings,
    const Prism::RHI::GraphicsDeviceCapabilities& capabilities) noexcept;

[[nodiscard]] OceanRuntimeFlags BuildOceanRuntimeFlags(
    const OceanSettings& settings) noexcept;

enum class OceanDirtyScope : std::uint32_t
{
    None = 0u,
    Constants = 1u << 0u,
    InitialSpectrum = 1u << 1u,
    Resources = 1u << 2u,
    Geometry = 1u << 3u,
    LocalReset = 1u << 4u,
    HistoryReset = 1u << 5u
};

constexpr OceanDirtyScope operator|(OceanDirtyScope lhs,
    OceanDirtyScope rhs) noexcept
{
    return static_cast<OceanDirtyScope>(
        static_cast<std::uint32_t>(lhs)
        | static_cast<std::uint32_t>(rhs));
}

constexpr bool HasDirtyScope(OceanDirtyScope value,
    OceanDirtyScope scope) noexcept
{
    return (static_cast<std::uint32_t>(value)
        & static_cast<std::uint32_t>(scope)) != 0u;
}

[[nodiscard]] OceanDirtyScope ClassifyOceanDirtyScopes(
    const OceanSettings& previous,
    const OceanSettings& current) noexcept;

struct OceanClipmapPlacement
{
    double snapInterval = 0.25;
    double snappedX = 0.0;
    double snappedZ = 0.0;
};

[[nodiscard]] OceanClipmapPlacement ComputeOceanClipmapPlacement(
    double cameraX,
    double cameraZ,
    float patchLength,
    std::uint32_t resolution,
    bool cameraFollowEnabled) noexcept;
} // namespace Prism::Renderer
