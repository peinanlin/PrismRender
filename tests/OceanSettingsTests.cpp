#include "Renderer/Features/Ocean/OceanFeature.h"
#include "Renderer/Features/Ocean/OceanSpectrumMath.h"
#include "Renderer/Features/Ocean/OceanFft.h"
#include "Renderer/Features/Ocean/OceanSpectrumGenerator.h"
#include "Renderer/Features/Ocean/OceanFoamSimulation.h"
#include "Renderer/Features/Ocean/OceanMapBuilder.h"
#include "Renderer/Features/Ocean/LocalWaveSimulation.h"
#include "Renderer/Features/Ocean/LocalWaveGpuResources.h"
#include "Renderer/Features/Ocean/OceanQueryService.h"
#include "Renderer/Features/Ocean/OceanQuadtree.h"
#include "Renderer/Pipeline/ScenePipelinePlan.h"
#include "RHI/DeviceCapabilities.h"

#include <cmath>
#include <array>
#include <algorithm>
#include <complex>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void Expect(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void ValidateCpuRoundTrip(Prism::Renderer::OceanFft& fft,
    const std::vector<std::complex<float>>& input, const char* label)
{
    std::vector<std::complex<float>> values = input;
    Expect(fft.Transform(values, false) && fft.Transform(values, true),
        "CPU FFT fixture transform failed.");
    float maximumError = 0.0f;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        Expect(std::isfinite(values[index].real())
                && std::isfinite(values[index].imag()),
            "CPU FFT fixture produced a non-finite value.");
        maximumError = std::max(maximumError,
            std::abs(values[index] - input[index]));
    }
    if (maximumError
        >= Prism::Renderer::OceanFft::Fp32RegressionTolerance)
    {
        throw std::runtime_error(std::string("CPU FFT ") + label
            + " round trip exceeded FP32 tolerance.");
    }
}
} // namespace

int main()
{
    try
    {
        Prism::Renderer::OceanFeature feature;
        const Prism::Renderer::OceanSettings& settings =
            feature.GetSettings();
        Expect(
            settings.quality
                == Prism::Renderer::OceanSimulationQuality::Extreme,
            "WaveWorks Reference quality must default to Extreme.");
        Expect(
            settings.simulationApi
                == Prism::Renderer::OceanSimulationApi::Compute,
            "WaveWorks Reference simulation API must default to Compute.");
        Expect(
            std::abs(settings.simulationPeriodMeters - 1000.0f)
                < 0.0001f,
            "WaveWorks Reference simulation period is incorrect.");
        Expect(
            std::abs(settings.baseWind.speed - 4.7f) < 0.0001f
                && std::abs(settings.baseWind.fetchKilometers - 0.1f)
                    < 0.0001f,
            "Base wind defaults are incorrect.");
        Expect(std::abs(
                   Prism::Renderer::BeaufortToMetersPerSecond(4.7f)
                   - 8.5187f) < 0.01f,
            "WaveWorks Beaufort wind must be converted to JONSWAP m/s units.");
        const float effectiveReferenceWind =
            Prism::Renderer::BeaufortToMetersPerSecond(4.7f);
        const float basePeakWavelength =
            Prism::Renderer::JonswapPeakWavelength(
                effectiveReferenceWind, 100.0f);
        const float swellPeakWavelength =
            Prism::Renderer::JonswapPeakWavelength(
                1.5f, 520000.0f);
        Expect(std::abs(basePeakWavelength - 0.537f) < 0.01f
                && std::abs(swellPeakWavelength - 48.39f) < 0.1f,
            "Reference JONSWAP peak wavelengths changed unexpectedly.");
        Expect(basePeakWavelength < 15.625f / 2.5f,
            "The regression fixture no longer demonstrates the old fine-band omission.");
        Expect(
            std::abs(settings.swell.speed - 1.5f) < 0.0001f
                && std::abs(settings.swell.fetchKilometers - 520.0f)
                    < 0.0001f,
            "Swell defaults are incorrect.");
        Expect(
            settings.local.gridSize == 512u
                && std::abs(settings.local.domainSizeMeters - 200.0f)
                    < 0.0001f
                && settings.local.demoEmittersEnabled
                && !settings.local.rainEmitterEnabled
                && settings.local.wakeEmitterEnabled,
            "Local wave defaults are incorrect.");
        Expect(
            settings.geometry.cellsPerPatch == 64u
                && settings.geometry.maximumLod == 15u,
            "Geometry defaults are incorrect.");
        Expect(
            settings.query.readbackFifoEntries == 30u,
            "WaveWorks Reference readback FIFO must contain 30 entries.");

        const Prism::Renderer::OceanSettings reference =
            Prism::Renderer::OceanSettings::WaveWorksReference();
        Expect(
            reference.implementation
                == Prism::Renderer::OceanImplementation::SpectralOcean,
            "Reference preset must select the spectral implementation.");
        Expect(
            std::abs(reference.baseWind.direction.x - 0.8489717f)
                    < 0.0001f
                && std::abs(reference.baseWind.direction.y - 0.5284383f)
                    < 0.0001f
                && std::abs(reference.baseWind.speed - 9.9f) < 0.0001f
                && std::abs(reference.baseWind.fetchKilometers - 0.001f)
                    < 0.0001f
                && std::abs(reference.baseWind.spectrumPeaking - 13.664f)
                    < 0.0001f
                && std::abs(reference.baseWind.smallWavesCutoffLength - 5.10f)
                    < 0.0001f
                && std::abs(reference.baseWind.smallWavesCutoffPower - 0.248f)
                    < 0.0001f
                && std::abs(reference.baseWind.amplitudeMultiplier - 2.058f)
                    < 0.0001f,
            "WaveWorks Reference base-wind preset is incorrect.");
        Expect(
            std::abs(reference.swell.direction.x) < 0.0001f
                && std::abs(reference.swell.direction.y - 1.0f) < 0.0001f
                && std::abs(reference.swell.speed - 29.62f) < 0.0001f
                && std::abs(reference.swell.fetchKilometers - 9.502f)
                    < 0.0001f
                && std::abs(reference.swell.spectrumPeaking - 12.275f)
                    < 0.0001f
                && std::abs(reference.swell.smallWavesCutoffLength - 7.23f)
                    < 0.0001f
                && std::abs(reference.swell.smallWavesCutoffPower - 0.007f)
                    < 0.0001f
                && std::abs(reference.swell.amplitudeMultiplier - 0.477f)
                    < 0.0001f,
            "WaveWorks Reference swell preset is incorrect.");
        Expect(
            std::abs(reference.foam.whitecapsThreshold - 0.5f) < 0.0001f
                && std::abs(reference.foam.generationThreshold - 0.37f)
                    < 0.0001f
                && std::abs(reference.foam.generationAmount - 0.12f)
                    < 0.0001f
                && std::abs(reference.foam.dissipationSpeed - 0.6f)
                    < 0.0001f
                && std::abs(reference.foam.falloffSpeed - 0.985f)
                    < 0.0001f,
            "WaveWorks Reference spectral-foam preset is incorrect.");
        Expect(std::abs(
                   Prism::Renderer::BeaufortToMetersPerSecond(
                       reference.baseWind.speed)
                   - 26.0411f) < 0.01f,
            "The 9.9 Beaufort reference wind must report 26.04 m/s.");
        const float tunedReferenceWind =
            Prism::Renderer::BeaufortToMetersPerSecond(
                reference.baseWind.speed);
        const float tunedBasePeakWavelength =
            Prism::Renderer::JonswapPeakWavelength(
                tunedReferenceWind,
                reference.baseWind.fetchKilometers * 1000.0f);
        const float tunedSwellPeakWavelength =
            Prism::Renderer::JonswapPeakWavelength(
                reference.swell.speed,
                reference.swell.fetchKilometers * 1000.0f);
        Expect(std::abs(tunedBasePeakWavelength - 0.0537f) < 0.001f
                && std::abs(tunedSwellPeakWavelength - 24.69f) < 0.1f,
            "Tuned reference JONSWAP peak wavelengths are incorrect.");
        Expect(
            std::abs(reference.shading.waterColorIntensity.x - 0.04f)
                    < 0.0001f
                && std::abs(reference.shading.waterColorIntensity.w - 0.38f)
                    < 0.0001f
                && std::abs(
                    reference.shading.underwaterFoamColor.x - 0.6f)
                    < 0.0001f
                && std::abs(reference.shading.scatteringColor.x
                        - 128.0f / 255.0f) < 0.0001f
                && std::abs(reference.shading.scatteringColor.y
                        - 160.0f / 255.0f) < 0.0001f
                && std::abs(reference.shading.scatteringColor.z
                        - 180.0f / 255.0f) < 0.0001f
                && std::abs(reference.shading.foamColor.x
                        - 184.0f / 255.0f) < 0.0001f
                && std::abs(reference.shading.foamColor.y
                        - 199.0f / 255.0f) < 0.0001f
                && std::abs(reference.shading.foamColor.z
                        - 194.0f / 255.0f) < 0.0001f,
            "WaveWorks Reference optical color constants are incorrect.");
        Prism::RHI::GraphicsDeviceCapabilities tessellationCapabilities{};
        tessellationCapabilities.features.tessellationShader = true;
        tessellationCapabilities.features.patchListTopology = true;
        tessellationCapabilities.limits.maxTessellationControlPoints = 32u;
        Expect(Prism::Renderer::ShouldUseOceanTessellation(
                    reference, tessellationCapabilities),
            "Supported tessellation capabilities were not selected.");
        tessellationCapabilities.features.patchListTopology = false;
        Expect(!Prism::Renderer::ShouldUseOceanTessellation(
                    reference, tessellationCapabilities),
            "Ocean selected tessellation without patch-list support.");
        Expect(
            Prism::Renderer::OceanSettings::ResolutionForQuality(
                Prism::Renderer::OceanSimulationQuality::Normal) == 128u
                && Prism::Renderer::OceanSettings::ResolutionForQuality(
                    Prism::Renderer::OceanSimulationQuality::High) == 256u
                && Prism::Renderer::OceanSettings::ResolutionForQuality(
                    Prism::Renderer::OceanSimulationQuality::Extreme) == 512u,
            "Quality resolution mapping is incorrect.");
        Expect(
            Prism::Renderer::OceanSettings::ReferenceCascadePatchLengths()
                == std::array<float, 4>{15.625f, 62.5f, 250.0f, 1000.0f},
            "Reference cascade patch lengths are incorrect.");
        const Prism::Renderer::OceanRuntimeFlags defaultRuntimeFlags =
            Prism::Renderer::BuildOceanRuntimeFlags(reference);
        constexpr std::uint32_t allCascades =
            Prism::Renderer::OceanDebugSettings::AllCascadeMask;
        const auto packedMask = [](const std::uint32_t packed,
                                    const std::uint32_t firstBit)
        {
            return (packed >> firstBit)
                & Prism::Renderer::OceanDebugSettings::AllCascadeMask;
        };
        Expect(
            packedMask(defaultRuntimeFlags.primary,
                Prism::Renderer::OceanRuntimeFlagLayout::
                    DisplacementCascadeFirstBit) == allCascades
                && packedMask(defaultRuntimeFlags.primary,
                    Prism::Renderer::OceanRuntimeFlagLayout::
                        GradientCascadeFirstBit) == allCascades
                && packedMask(defaultRuntimeFlags.secondary,
                    Prism::Renderer::OceanRuntimeFlagLayout::
                        FoldingCascadeFirstBit) == allCascades
                && packedMask(defaultRuntimeFlags.secondary,
                    Prism::Renderer::OceanRuntimeFlagLayout::
                        FoamHistoryCascadeFirstBit) == allCascades
                && packedMask(defaultRuntimeFlags.secondary,
                    Prism::Renderer::OceanRuntimeFlagLayout::
                        MomentsCascadeFirstBit) == allCascades,
            "WaveWorks runtime stages must default to all cascades enabled.");
        Prism::Renderer::OceanSettings isolatedRuntime = reference;
        isolatedRuntime.debug.cascadeMask = 0b0101u;
        isolatedRuntime.debug.displacementCascadeMask = 0b0011u;
        isolatedRuntime.debug.gradientCascadeMask = 0b1110u;
        isolatedRuntime.debug.foldingCascadeMask = 0b1001u;
        isolatedRuntime.debug.foamHistoryCascadeMask = 0b0110u;
        isolatedRuntime.debug.momentsCascadeMask = 0b1100u;
        isolatedRuntime.debug.spectralFoamHistoryEnabled = false;
        const Prism::Renderer::OceanRuntimeFlags isolatedFlags =
            Prism::Renderer::BuildOceanRuntimeFlags(isolatedRuntime);
        Expect(
            packedMask(isolatedFlags.primary,
                Prism::Renderer::OceanRuntimeFlagLayout::
                    DisplacementCascadeFirstBit) == 0b0001u
                && packedMask(isolatedFlags.primary,
                    Prism::Renderer::OceanRuntimeFlagLayout::
                        GradientCascadeFirstBit) == 0b0100u
                && packedMask(isolatedFlags.secondary,
                    Prism::Renderer::OceanRuntimeFlagLayout::
                        FoldingCascadeFirstBit) == 0b0001u
                && packedMask(isolatedFlags.secondary,
                    Prism::Renderer::OceanRuntimeFlagLayout::
                        FoamHistoryCascadeFirstBit) == 0u
                && packedMask(isolatedFlags.secondary,
                    Prism::Renderer::OceanRuntimeFlagLayout::
                        MomentsCascadeFirstBit) == 0b0100u,
            "WaveWorks stage masks did not combine with the cascade master mask.");
        Expect(
            Prism::Renderer::ClassifyOceanDirtyScopes(
                reference, isolatedRuntime)
                == Prism::Renderer::OceanDirtyScope::None,
            "Surface-only stage isolation must not rebuild FFT resources or reset foam history.");
        const auto clipmapPlacement =
            Prism::Renderer::ComputeOceanClipmapPlacement(
                1.24, -2.51, 1000.0f, 128u, true);
        Expect(std::abs(clipmapPlacement.snapInterval - 7.8125) < 1.0e-6
                && std::abs(clipmapPlacement.snappedX - 0.0) < 1.0e-6
                && std::abs(clipmapPlacement.snappedZ + 0.0) < 1.0e-6,
            "Camera-follow clipmap snapping is not deterministic.");
        const auto staticPlacement =
            Prism::Renderer::ComputeOceanClipmapPlacement(
                100.0, -50.0, 1000.0f, 128u, false);
        Expect(staticPlacement.snappedX == 0.0
                && staticPlacement.snappedZ == 0.0
                && staticPlacement.snapInterval >= 0.25,
            "Disabled camera-follow must keep a stable ocean origin.");

        Prism::Renderer::OceanSettings invalid = reference;
        invalid.simulationPeriodMeters = -1.0f;
        invalid.baseWind.direction = {0.0f, 0.0f};
        std::string warning;
        Expect(!invalid.ValidateAndNormalize(&warning) && !warning.empty(),
            "Invalid settings must be normalized and reported.");
        Expect(invalid.simulationPeriodMeters > 0.0f,
            "Invalid period was not clamped.");

        using Prism::Renderer::DeepWaterAngularFrequency;
        using Prism::Renderer::EvaluateSpectrum;
        using Prism::Renderer::JonswapEnergy;
        Expect(DeepWaterAngularFrequency(0.0f) == 0.0f,
            "Zero wave number must have zero angular frequency.");
        Expect(JonswapEnergy(0.0f, 4.7f, 100.0f, 3.3f, 0.0f, 0.0f, 1.0f)
                == 0.0f,
            "Zero wave number must have zero JONSWAP energy.");
        const float aligned = EvaluateSpectrum(0.15f, 1.0f,
            reference.baseWind).energy;
        const float opposing = EvaluateSpectrum(0.15f, -1.0f,
            reference.baseWind).energy;
        Expect(std::isfinite(aligned) && std::isfinite(opposing)
                && aligned >= opposing,
            "Directional JONSWAP values must be finite and directional.");
        const float peakWaveNumber = std::pow(
            22.0f * std::pow(9.81f * 9.81f / (4.7f * 100.0f), 0.33f),
            2.0f) / 9.81f;
        Expect(
            JonswapEnergy(peakWaveNumber, 4.7f, 100.0f, 10.0f,
                0.0f, 0.0f, 1.0f)
                > JonswapEnergy(peakWaveNumber, 4.7f, 100.0f, 1.0f,
                    0.0f, 0.0f, 1.0f),
            "JONSWAP peaking must increase energy near the peak.");
        Prism::Renderer::OceanWindSpectrumSettings noAmplitude =
            reference.baseWind;
        noAmplitude.amplitudeMultiplier = 0.0f;
        Expect(EvaluateSpectrum(0.15f, 1.0f, noAmplitude).energy == 0.0f,
            "Zero amplitude must disable the wind contribution.");

        Prism::Renderer::RenderSettings pipelineSettings{};
        pipelineSettings.fftOceanEnabled = true;
        pipelineSettings.ocean.debug.simulateWater = true;
        pipelineSettings.ocean.debug.renderWater = true;
        pipelineSettings.ocean.implementation =
            Prism::Renderer::OceanImplementation::LegacyFft;
        const auto legacyPlan = Prism::Renderer::BuildScenePipelinePlan(
            pipelineSettings, Prism::RHI::ResourceState::ShaderResource);
        Expect(legacyPlan.execution.fftOcean
                && !legacyPlan.execution.spectralOcean,
            "Legacy migration mode must schedule only the legacy ocean path.");
        pipelineSettings.ocean.implementation =
            Prism::Renderer::OceanImplementation::SpectralOcean;
        const auto spectralPlan = Prism::Renderer::BuildScenePipelinePlan(
            pipelineSettings, Prism::RHI::ResourceState::ShaderResource);
        Expect(!spectralPlan.execution.fftOcean
                && spectralPlan.execution.spectralOcean,
            "Spectral mode must schedule only the spectral ocean path.");
        pipelineSettings.ocean.opticsModel =
            Prism::Renderer::OceanOpticsModel::HpWater;
        const auto hpWaterForwardPlan =
            Prism::Renderer::BuildScenePipelinePlan(
                pipelineSettings,
                Prism::RHI::ResourceState::Present);
        Expect(hpWaterForwardPlan.execution.spectralOcean
                && hpWaterForwardPlan.execution.hpWaterOptics
                && hpWaterForwardPlan.execution.waterVisibility
                && !hpWaterForwardPlan.execution.opaqueGeometryIncludesOcean,
            "HPWater forward planning must split water from ordinary geometry without duplicating spectral simulation.");
        pipelineSettings.deferredRenderingEnabled = true;
        const auto hpWaterDeferredPlan =
            Prism::Renderer::BuildScenePipelinePlan(
                pipelineSettings,
                Prism::RHI::ResourceState::Present);
        Expect(hpWaterDeferredPlan.execution.deferredGeometry
                && hpWaterDeferredPlan.execution.waterVisibility
                && !hpWaterDeferredPlan.execution.opaqueGeometryIncludesOcean,
            "HPWater deferred planning must split water from the opaque GBuffer.");
        pipelineSettings.ocean.debug.renderWater = false;
        const auto disabledWaterPlan =
            Prism::Renderer::BuildScenePipelinePlan(
                pipelineSettings,
                Prism::RHI::ResourceState::Present);
        Expect(!disabledWaterPlan.execution.spectralOcean
                && !disabledWaterPlan.execution.hpWaterOptics
                && !disabledWaterPlan.execution.waterVisibility
                && disabledWaterPlan.execution.opaqueGeometryIncludesOcean,
            "Disabled water must not schedule HPWater visibility.");
        pipelineSettings.ocean.debug.renderWater = true;
        pipelineSettings.deferredRenderingEnabled = false;
        pipelineSettings.ocean.opticsModel =
            Prism::Renderer::OceanOpticsModel::Current;
        Prism::Renderer::OceanSpectrumGenerator settingsPathGenerator;
        pipelineSettings.ocean = reference;
        pipelineSettings.ocean.implementation =
            Prism::Renderer::OceanImplementation::SpectralOcean;
        Expect(settingsPathGenerator.Configure(pipelineSettings.ocean),
            "Spectral settings-path fixture failed to configure.");
        settingsPathGenerator.MarkSpectrumCommitted();
        pipelineSettings.oceanWindSpeed += 9.0f;
        Expect(!settingsPathGenerator.Configure(pipelineSettings.ocean),
            "A flat compatibility-only edit leaked into the spectral path.");
        pipelineSettings.ocean.baseWind.speed += 0.5f;
        Expect(settingsPathGenerator.Configure(pipelineSettings.ocean),
            "A visible nested spectral edit did not reach the spectrum path.");
        pipelineSettings.ocean = reference;
        pipelineSettings.ocean.debug.simulateWater = false;
        pipelineSettings.ocean.debug.renderWater = true;
        const auto pausedSpectralPlan =
            Prism::Renderer::BuildScenePipelinePlan(
                pipelineSettings, Prism::RHI::ResourceState::ShaderResource);
        Expect(pausedSpectralPlan.execution.spectralOcean
                && !pausedSpectralPlan.graphOptions.oceanSimulationEnabled,
            "Pausing simulation must keep the last spectral maps renderable.");
        pipelineSettings.ocean.debug.simulateWater = true;

        pipelineSettings.ocean = reference;
        pipelineSettings.fftOceanEnabled = true;
        pipelineSettings.oceanCameraFollowEnabled = false;
        pipelineSettings.oceanPatchLength = 320.0f;
        pipelineSettings.oceanWindDirection = {0.6f, 0.8f};
        pipelineSettings.oceanWindSpeed = 15.0f;
        pipelineSettings.oceanSpectrumAmplitude = 10.5f;
        pipelineSettings.oceanChoppiness = 1.18f;
        pipelineSettings.SyncLegacyOceanFields();
        Expect(
            pipelineSettings.ocean.implementation
                == Prism::Renderer::OceanImplementation::LegacyFft
                && !pipelineSettings.ocean.cameraFollowEnabled
                && std::abs(pipelineSettings.ocean.local.domainSizeMeters
                    - 320.0f) < 0.0001f
                && std::abs(pipelineSettings.ocean.baseWind.speed - 15.0f)
                    < 0.0001f,
            "Legacy ocean fields did not migrate into structured settings.");

        for (const std::uint32_t size : {128u, 256u, 512u})
        {
            Prism::Renderer::OceanFft fft;
            Expect(fft.Configure(size) && fft.StageCount() == 7u
                    + (size == 256u ? 1u : size == 512u ? 2u : 0u),
                "FFT did not accept a supported power-of-two size.");
            Expect(fft.BitReverse(0u) == 0u
                    && fft.BitReverse(1u) == size / 2u
                    && fft.BitReverse(size - 1u) == size - 1u,
                "FFT bit-reversal indexing is incorrect.");
            const auto stageZeroTwiddle = fft.Twiddle(0u, 0u, true);
            const auto quarterTurnForward = fft.Twiddle(1u, 1u, false);
            const auto quarterTurnInverse = fft.Twiddle(1u, 1u, true);
            Expect(std::abs(stageZeroTwiddle.real() - 1.0f) < 1.0e-6f
                    && std::abs(stageZeroTwiddle.imag()) < 1.0e-6f
                    && std::abs(quarterTurnForward.real()) < 1.0e-6f
                    && std::abs(quarterTurnForward.imag() + 1.0f)
                        < 1.0e-6f
                    && std::abs(quarterTurnInverse.imag() - 1.0f)
                        < 1.0e-6f,
                "FFT twiddle generation is incorrect.");
            const auto twoDimensionalPlan = fft.BuildPasses(
                2u, true, 0u, false, false);
            Expect(twoDimensionalPlan.size()
                        == static_cast<std::size_t>(fft.StageCount()) * 2u
                    && twoDimensionalPlan.front().sourceIndex == 0u
                    && twoDimensionalPlan.back().destinationIndex == 0u,
                "FFT runtime stage plan has invalid ping-pong metadata.");
            std::vector<std::complex<float>> impulse(size);
            impulse[3] = {1.0f, 0.0f};
            ValidateCpuRoundTrip(fft, impulse, "impulse");
            std::vector<std::complex<float>> constant(
                size, {0.75f, -0.25f});
            ValidateCpuRoundTrip(fft, constant, "constant");
            std::vector<std::complex<float>> sinusoid(size);
            for (std::uint32_t index = 0u; index < size; ++index)
            {
                const float phase = 2.0f * 3.14159265358979323846f
                    * 5.0f * static_cast<float>(index)
                    / static_cast<float>(size);
                sinusoid[index] = {std::cos(phase), std::sin(phase)};
            }
            ValidateCpuRoundTrip(fft, sinusoid, "sinusoid");

            std::vector<std::complex<float>> hermitian(size);
            hermitian[0] = {0.5f, 0.0f};
            hermitian[3] = {0.25f, 0.4f};
            hermitian[size - 3u] = std::conj(hermitian[3]);
            Expect(fft.Transform(hermitian, true),
                "CPU Hermitian inverse FFT failed.");
            for (const std::complex<float>& value : hermitian)
            {
                Expect(std::isfinite(value.real())
                        && std::abs(value.imag()) < 2.0e-5f,
                    "Hermitian spectrum did not produce finite real output.");
            }

            std::vector<std::complex<float>> normalization(size);
            normalization[0] = {
                static_cast<float>(size), 0.0f};
            Expect(fft.Transform(normalization, true),
                "CPU normalization fixture inverse FFT failed.");
            for (const std::complex<float>& value : normalization)
            {
                Expect(std::abs(value.real() - 1.0f) < 2.0e-5f
                        && std::abs(value.imag()) < 2.0e-5f,
                    "CPU inverse FFT normalization is incorrect.");
            }
        }
        Prism::Renderer::OceanFft unsupportedFft;
        Expect(!unsupportedFft.Configure(64u)
                && !unsupportedFft.Configure(1024u),
            "FFT accepted a size outside the project quality mapping.");

        Prism::Renderer::OceanSpectrumGenerator generator;
        Expect(generator.Configure(reference),
            "First spectrum configuration must request a cache rebuild.");
        const std::uint64_t referenceSpectrumKey = generator.SpectrumKey();
        generator.MarkSpectrumCommitted();
        Expect(!generator.Configure(reference)
                && !generator.IsSpectrumDirty(),
            "Unchanged settings must reuse the cached initial spectrum.");
        Prism::Renderer::OceanSettings changedWind = reference;
        changedWind.baseWind.speed += 0.25f;
        Expect(generator.Configure(changedWind)
                && generator.IsSpectrumDirty()
                && generator.SpectrumKey() != referenceSpectrumKey,
            "A wind setting change must schedule exactly one compatible rebuild.");
        generator.MarkSpectrumCommitted();
        Expect(!generator.Configure(changedWind)
                && !generator.IsSpectrumDirty(),
            "A committed wind spectrum must not rebuild again without changes.");
        Expect(generator.Configure(reference),
            "Restoring reference settings must rebuild the cached spectrum.");
        Expect(generator.GetCascades()[0].patchLengthMeters == 15.625f
                && generator.GetCascades()[3].patchLengthMeters == 1000.0f
                && generator.GetCascades()[2].resolution == 512u,
            "Cascade reference configuration is incorrect.");
        for (const auto quality : {
                 Prism::Renderer::OceanSimulationQuality::Normal,
                 Prism::Renderer::OceanSimulationQuality::High,
                 Prism::Renderer::OceanSimulationQuality::Extreme})
        {
            Prism::Renderer::OceanSettings qualitySettings = reference;
            qualitySettings.quality = quality;
            Prism::Renderer::OceanSpectrumGenerator qualityGenerator;
            Expect(qualityGenerator.Configure(qualitySettings),
                "Quality-specific cascade metadata failed to configure.");
            const auto baseWeights = qualityGenerator.BandWeights(
                basePeakWavelength);
            const auto swellWeights = qualityGenerator.BandWeights(
                swellPeakWavelength);
            float baseOwnership = 0.0f;
            float swellOwnership = 0.0f;
            for (std::size_t cascade = 0u; cascade < 4u; ++cascade)
            {
                baseOwnership += baseWeights[cascade];
                swellOwnership += swellWeights[cascade];
                const auto& metadata =
                    qualityGenerator.GetCascades()[cascade];
                Expect(std::abs(metadata.wavelengthMinMeters
                        - Prism::Renderer::OceanSpectrumGenerator::NyquistTexelsPerWave
                            * metadata.patchLengthMeters
                            / static_cast<float>(metadata.resolution))
                            < 1.0e-6f
                        && metadata.wavelengthMaxMeters
                            == metadata.patchLengthMeters
                        && metadata.uvScale > 0.0f,
                    "Resolution-aware cascade metadata is inconsistent.");
            }
            Expect(std::abs(baseOwnership - 1.0f) < 1.0e-4f
                    && std::abs(swellOwnership - 1.0f) < 1.0e-4f,
                "Reference wind or swell peak has no cascade ownership.");
        }
        const auto& referenceCascades = generator.GetCascades();
        const DirectX::XMFLOAT2 uvFixturePosition{37.0f, -19.0f};
        std::array<DirectX::XMFLOAT2, 4> warpDirections{};
        for (std::size_t cascade = 0u;
             cascade < referenceCascades.size(); ++cascade)
        {
            const auto unwarped =
                Prism::Renderer::OceanSpectrumGenerator::ComputeCascadeUv(
                    uvFixturePosition, referenceCascades[cascade], 0.0f, 2.0f);
            const auto warped =
                Prism::Renderer::OceanSpectrumGenerator::ComputeCascadeUv(
                    uvFixturePosition, referenceCascades[cascade], 0.03f, 2.0f);
            Expect(std::abs(unwarped.x
                        - uvFixturePosition.x
                            * referenceCascades[cascade].uvScale) < 1.0e-6f
                    && std::abs(unwarped.y
                        - uvFixturePosition.y
                            * referenceCascades[cascade].uvScale) < 1.0e-6f
                    && std::abs(warped.x - unwarped.x) <= 0.03001f
                    && std::abs(warped.y - unwarped.y) <= 0.03001f
                    && std::hypot(
                        warped.x - unwarped.x,
                        warped.y - unwarped.y) > 1.0e-5f,
                "Cascade UV warping is not expressed in per-cascade UV units.");
            const float warpLength = std::hypot(
                warped.x - unwarped.x,
                warped.y - unwarped.y);
            warpDirections[cascade] = {
                (warped.x - unwarped.x) / warpLength,
                (warped.y - unwarped.y) / warpLength};
        }
        float strongestAdjacentAlignment = -1.0f;
        for (std::size_t cascade = 1u;
             cascade < warpDirections.size(); ++cascade)
        {
            strongestAdjacentAlignment = std::max(
                strongestAdjacentAlignment,
                warpDirections[cascade - 1u].x * warpDirections[cascade].x
                    + warpDirections[cascade - 1u].y
                        * warpDirections[cascade].y);
        }
        Expect(strongestAdjacentAlignment < 0.98f,
            "Adjacent cascade UV warps became mechanically coordinated.");
        Expect(Prism::Renderer::OceanSpectrumGenerator::DistanceFadeWeight(
                   referenceCascades[0].distanceFadeStartMeters,
                   referenceCascades[0]) == 1.0f
                && std::abs(
                    referenceCascades[0].distanceFadeEndMeters
                        - referenceCascades[0].patchLengthMeters
                            * Prism::Renderer::OceanSpectrumGenerator::
                                CascadeVisibilityPeriods) < 1.0e-5f
                && std::abs(
                    Prism::Renderer::OceanSpectrumGenerator::
                        DistanceFadeWeight(
                            referenceCascades[0].distanceFadeEndMeters * 0.5f,
                            referenceCascades[0]) - 0.5f) < 1.0e-5f
                && Prism::Renderer::OceanSpectrumGenerator::DistanceFadeWeight(
                   referenceCascades[0].distanceFadeEndMeters,
                   referenceCascades[0]) == 0.0f
                && Prism::Renderer::OceanSpectrumGenerator::DistanceFadeWeight(
                   1000000.0f, referenceCascades[3]) == 1.0f,
            "Fine-to-coarse distance fading or horizon preservation is invalid.");
        const float minimumResolvable =
            referenceCascades.front().wavelengthMinMeters;
        const float maximumResolvable =
            referenceCascades.back().wavelengthMaxMeters;
        for (std::uint32_t sample = 0u; sample <= 256u; ++sample)
        {
            const float t = static_cast<float>(sample) / 256.0f;
            const float wavelength = std::exp(
                std::log(minimumResolvable) * (1.0f - t)
                + std::log(maximumResolvable) * t);
            const auto weights = generator.BandWeights(wavelength);
            float sum = 0.0f;
            for (const float weight : weights)
                sum += weight;
            Expect(std::abs(sum - 1.0f) < 1.0e-4f,
                "A resolvable wavelength fell through all cascade bands.");
        }
        Prism::Renderer::OceanWindSpectrumSettings effectiveWind =
            reference.baseWind;
        effectiveWind.speed = tunedReferenceWind;
        Expect(Prism::Renderer::EvaluateSpectrum(
                   2.0f * 3.14159265358979323846f
                       / tunedBasePeakWavelength,
                   1.0f, effectiveWind).energy > 0.0f
                && Prism::Renderer::EvaluateSpectrum(
                   2.0f * 3.14159265358979323846f
                       / tunedSwellPeakWavelength,
                   1.0f, reference.swell).energy > 0.0f,
            "Reference peak energy must remain finite and non-zero.");
        for (const float wavelength : {8.0f, 20.0f, 31.0f, 35.0f,
            80.0f, 125.0f, 300.0f,
            900.0f, 2200.0f})
        {
            const auto weights = generator.BandWeights(wavelength);
            float sum = 0.0f;
            for (const float weight : weights)
            {
                Expect(std::isfinite(weight) && weight >= 0.0f,
                    "Cascade band weight must be finite and non-negative.");
                sum += weight;
            }
            Expect(std::abs(sum - 1.0f) < 1.0e-4f,
                "Cascade band weights must preserve total energy.");
        }
        const std::uint32_t sampleX = 69u;
        const std::uint32_t sampleY = 67u;
        const float samplePatch = generator.GetCascades()[2].patchLengthMeters;
        const float sampleWaveX = 2.0f * 3.14159265358979323846f
            * static_cast<float>(static_cast<int>(sampleX) - 64)
            / samplePatch;
        const float sampleWaveY = 2.0f * 3.14159265358979323846f
            * static_cast<float>(static_cast<int>(sampleY) - 64)
            / samplePatch;
        const float sampleWavelength = 2.0f * 3.14159265358979323846f
            / std::sqrt(sampleWaveX * sampleWaveX
                + sampleWaveY * sampleWaveY);
        const auto sampleWeights = generator.BandWeights(sampleWavelength);
        const auto initialSample =
            Prism::Renderer::EvaluateInitialSpectrumTexel(reference,
                samplePatch, sampleWeights[2], 128u, sampleX, sampleY,
                2u, 0x4f1bbcdcu);
        const auto replayedSample =
            Prism::Renderer::EvaluateInitialSpectrumTexel(reference,
                samplePatch, sampleWeights[2], 128u, sampleX, sampleY,
                2u, 0x4f1bbcdcu);
        Expect(std::isfinite(initialSample.real)
                && std::isfinite(initialSample.imaginary)
                && initialSample.energy >= 0.0f
                && initialSample.angularFrequency > 0.0f
                && initialSample.real == replayedSample.real
                && initialSample.imaginary == replayedSample.imaginary,
            "Deterministic CPU initial-spectrum sampling is invalid.");
        const auto separatedSample =
            Prism::Renderer::EvaluateInitialSpectrumComponents(reference,
                samplePatch, sampleWeights[2], 128u, sampleX, sampleY,
                2u, 0x4f1bbcdcu);
        Prism::Renderer::OceanSettings noSwell = reference;
        noSwell.swell.amplitudeMultiplier = 0.0f;
        const auto noSwellSample =
            Prism::Renderer::EvaluateInitialSpectrumComponents(noSwell,
                samplePatch, sampleWeights[2], 128u, sampleX, sampleY,
                2u, 0x4f1bbcdcu);
        Expect(separatedSample.baseH0.x == noSwellSample.baseH0.x
                && separatedSample.baseH0.y == noSwellSample.baseH0.y
                && noSwellSample.swellH0.x == 0.0f
                && noSwellSample.swellH0.y == 0.0f
                && noSwellSample.swellEnergy == 0.0f,
            "Disabling swell must not perturb the cached base-wind realization.");

        Prism::Renderer::OceanSettings unitAmplitude = noSwell;
        // Keep this scaling oracle at the historical 0.54 m peak so the
        // selected FFT bin carries measurable energy independently of the
        // user-authored WaveWorks comparison preset.
        unitAmplitude.baseWind = {};
        unitAmplitude.baseWind.amplitudeMultiplier = 1.0f;
        Prism::Renderer::OceanSettings doubledAmplitude = unitAmplitude;
        doubledAmplitude.baseWind.amplitudeMultiplier = 2.0f;
        const auto unitAmplitudeSample =
            Prism::Renderer::EvaluateInitialSpectrumComponents(
                unitAmplitude, 15.625f, 1.0f, 128u,
                93u, 64u, 0u, 0x4f1bbcdcu);
        const auto doubledAmplitudeSample =
            Prism::Renderer::EvaluateInitialSpectrumComponents(
                doubledAmplitude, 15.625f, 1.0f, 128u,
                93u, 64u, 0u, 0x4f1bbcdcu);
        const float unitAmplitudeMagnitude = std::hypot(
            unitAmplitudeSample.baseH0.x, unitAmplitudeSample.baseH0.y);
        const float doubledAmplitudeMagnitude = std::hypot(
            doubledAmplitudeSample.baseH0.x,
            doubledAmplitudeSample.baseH0.y);
        Expect(unitAmplitudeMagnitude > 1.0e-8f
                && std::abs(doubledAmplitudeMagnitude
                        / unitAmplitudeMagnitude - 2.0f) < 1.0e-4f,
            "Spectrum amplitude multiplier must scale h0 linearly, not by its square root.");

        const Prism::Renderer::OceanAnalyticWave analyticWave{
            2.0f, {0.5f, 0.25f}, 0.3f, 1.25f};
        const auto analytic =
            Prism::Renderer::OceanMapBuilder::EvaluateAnalyticWave(
                analyticWave, 4.0f, -3.0f);
        const float normalLength = std::sqrt(
            analytic.normal.x * analytic.normal.x
            + analytic.normal.y * analytic.normal.y
            + analytic.normal.z * analytic.normal.z);
        Expect(std::isfinite(analytic.displacement.x)
                && std::isfinite(analytic.displacement.y)
                && std::isfinite(analytic.displacement.z)
                && std::abs(normalLength - 1.0f) < 1.0e-5f
                && std::abs(analytic.slopeMoments.z
                    - analytic.gradient.x * analytic.gradient.x) < 1.0e-5f
                && std::abs(analytic.slopeMoments.w
                    - analytic.gradient.y * analytic.gradient.y) < 1.0e-5f,
            "Analytic Y-up/XZ ocean map oracle is invalid.");
        const auto flat =
            Prism::Renderer::OceanMapBuilder::EvaluateAnalyticWave(
                {0.0f, {1.0f, 0.0f}, 0.0f, 1.0f}, 0.0f, 0.0f);
        Expect(flat.displacement.x == 0.0f
                && flat.displacement.y == 0.0f
                && flat.displacement.z == 0.0f
                && flat.normal.x == 0.0f && flat.normal.y == 1.0f
                && flat.normal.z == 0.0f && flat.jacobian == 1.0f,
            "Flat analytic ocean must publish identity tangents and up normal.");
        const auto breakingCrest =
            Prism::Renderer::OceanMapBuilder::EvaluateAnalyticWave(
                {2.0f, {0.5f, 0.0f}, DirectX::XM_PI * 0.5f, 1.25f},
                0.0f, 0.0f);
        Expect(std::isfinite(breakingCrest.jacobian)
                && breakingCrest.jacobian < 0.0f
                && breakingCrest.folding > reference.foam.whitecapsThreshold
                && flat.folding == 0.0f,
            "Jacobian folding must identify bounded breaking crests while calm water remains zero.");

        const Prism::Renderer::OceanAnalyticWave linearProfile{
            0.5f, {1.0f, 0.0f}, 0.0f, 0.0f};
        const Prism::Renderer::OceanAnalyticWave choppyProfile{
            0.5f, {1.0f, 0.0f}, 0.0f, 1.0f};
        const auto linearShoulder =
            Prism::Renderer::OceanMapBuilder::EvaluateAnalyticWave(
                linearProfile, 0.0f, 0.0f);
        const auto linearCrest =
            Prism::Renderer::OceanMapBuilder::EvaluateAnalyticWave(
                linearProfile, DirectX::XM_PIDIV2, 0.0f);
        const auto choppyShoulder =
            Prism::Renderer::OceanMapBuilder::EvaluateAnalyticWave(
                choppyProfile, 0.0f, 0.0f);
        const auto choppyCrest =
            Prism::Renderer::OceanMapBuilder::EvaluateAnalyticWave(
                choppyProfile, DirectX::XM_PIDIV2, 0.0f);
        const float linearHalfWidth =
            (DirectX::XM_PIDIV2 + linearCrest.displacement.x)
            - linearShoulder.displacement.x;
        const float choppyHalfWidth =
            (DirectX::XM_PIDIV2 + choppyCrest.displacement.x)
            - choppyShoulder.displacement.x;
        Expect(choppyHalfWidth > 0.0f
                && choppyHalfWidth < linearHalfWidth * 0.75f,
            "Positive lateral multiplier must compress the wave crest instead of widening it.");

        Prism::Renderer::OceanFoamSimulation foam;
        std::vector<float> foamHistory{0.0f};
        const std::vector<float> folding{1.0f};
        foam.Update(foamHistory, folding, reference.foam, 1.0f / 60.0f);
        Expect(foamHistory[0] > 0.0f,
            "Folding must generate persistent foam.");
        Prism::Renderer::OceanFoamSettings independentThresholds =
            reference.foam;
        independentThresholds.whitecapsThreshold = 0.3f;
        independentThresholds.generationThreshold = 0.6f;
        independentThresholds.generationAmount = 0.2f;
        independentThresholds.dissipationSpeed = 0.0f;
        independentThresholds.falloffSpeed = 1.0f;
        std::vector<float> lowerWhitecapHistory{0.0f};
        std::vector<float> higherWhitecapHistory{0.0f};
        const std::vector<float> independentFolding{0.8f};
        foam.Update(lowerWhitecapHistory, independentFolding,
            independentThresholds, 1.0f / 60.0f);
        independentThresholds.whitecapsThreshold = 0.9f;
        foam.Update(higherWhitecapHistory, independentFolding,
            independentThresholds, 1.0f / 60.0f);
        Expect(lowerWhitecapHistory[0] > 0.0f
                && std::abs(lowerWhitecapHistory[0]
                    - higherWhitecapHistory[0]) < 1.0e-6f,
            "Whitecaps threshold must not become the upper endpoint of the persistent-energy generation ramp.");
        Prism::Renderer::OceanFoamSettings spatialSettings =
            independentThresholds;
        spatialSettings.generationAmount = 0.0f;
        spatialSettings.dissipationSpeed = 1.0f;
        spatialSettings.falloffSpeed = 1.0f;
        std::vector<float> spatialHistory{1.0f, 0.0f, 0.0f};
        const std::vector<float> calmSpatialFolding(3u, 0.0f);
        foam.Update(spatialHistory, calmSpatialFolding,
            spatialSettings, 1.0f / 60.0f);
        Expect(spatialHistory[0] < 1.0f
                && spatialHistory[1] > 0.0f
                && spatialHistory[2] > 0.0f,
            "Dissipation must spread turbulent energy spatially instead of acting as a second temporal decay rate.");
        const float generatedFoam = foamHistory[0];
        const std::vector<float> noFolding{0.0f};
        foam.Update(foamHistory, noFolding, reference.foam, 1.0f / 60.0f);
        Expect(foamHistory[0] < generatedFoam && foamHistory[0] > 0.0f,
            "Foam must decay without disappearing in one frame.");
        float previousFoam = foamHistory[0];
        for (std::uint32_t frameIndex = 0u; frameIndex < 1200u; ++frameIndex)
        {
            foam.Update(foamHistory, noFolding, reference.foam,
                1.0f / 60.0f);
            Expect(std::isfinite(foamHistory[0])
                    && foamHistory[0] >= 0.0f
                    && foamHistory[0] <= previousFoam,
                "Foam decay must remain finite, bounded, and monotonic.");
            previousFoam = foamHistory[0];
        }
        Expect(foamHistory[0] < 1.0e-4f,
            "Persistent foam did not asymptotically decay to zero.");
        foamHistory[0] = std::numeric_limits<float>::quiet_NaN();
        const std::vector<float> invalidFolding{
            std::numeric_limits<float>::quiet_NaN()};
        foam.Update(foamHistory, invalidFolding, reference.foam,
            std::numeric_limits<float>::quiet_NaN());
        Expect(foamHistory[0] == 0.0f,
            "Invalid foam history was not sanitized to a finite neutral state.");
        const std::uint64_t foamVersion = foam.HistoryVersion();
        foam.Reset();
        Expect(foamVersion > 0u && foam.HistoryVersion() == 0u,
            "Foam reset did not invalidate the CPU history version.");

        Prism::Renderer::LocalWaveSimulation local;
        Prism::Renderer::OceanLocalWaveSettings localSettings = reference.local;
        localSettings.gridSize = 128u;
        localSettings.demoEmittersEnabled = false;
        Expect(Prism::Renderer::LocalWaveGpuResources::ClampGridSize(1u)
                == 128u
                && Prism::Renderer::LocalWaveGpuResources::ClampGridSize(512u)
                == 512u
                && Prism::Renderer::LocalWaveGpuResources::ClampGridSize(4096u)
                == 2048u,
            "Local wave GPU capability limits must clamp to supported grids.");
        Expect(std::abs(
                    Prism::Renderer::LocalWaveGpuResources::EstimateAllocatedMegabytes(
                        512u)
                    - 7.0f)
                < 1.0e-5f,
            "The default 512 local-wave resource memory estimate is invalid.");
        const auto baseBounds =
            Prism::Renderer::OceanQueryService::EstimateBounds(reference);
        Prism::Renderer::OceanSettings higherAmplitude = reference;
        higherAmplitude.baseWind.amplitudeMultiplier = 3.0f;
        const auto highBounds =
            Prism::Renderer::OceanQueryService::EstimateBounds(
                higherAmplitude);
        Expect(std::isfinite(baseBounds.radius)
                && baseBounds.radius > 0.0f
                && highBounds.radius > baseBounds.radius,
            "Ocean displacement bounds must be finite and respond to amplitude.");
        Expect(local.Configure(localSettings)
                && local.GridSize() == 128u
                && local.DomainSizeMeters() == 200.0f,
            "Local wave resource configuration is invalid.");
        const float untouchedHeight = local.Sample(80.0f, 80.0f).height;
        const Prism::Renderer::LocalWaveDisturbance outside{
            {500.0f, 500.0f}, 4.0f, 2.0f, {0.0f, 0.0f}};
        local.AddDisturbances(std::span<const Prism::Renderer::LocalWaveDisturbance>(
            &outside, 1u));
        local.Advance(0.5f, localSettings);
        Expect(std::abs(local.Sample(80.0f, 80.0f).height - untouchedHeight)
                < 1.0e-6f,
            "An outside local disturbance changed the active domain.");
        const Prism::Renderer::LocalWaveDisturbance inside{
            {0.0f, 0.0f}, 5.0f, 1.5f, {1.0f, 0.0f}};
        local.AddDisturbances(std::span<const Prism::Renderer::LocalWaveDisturbance>(
            &inside, 1u));
        local.Advance(0.5f, localSettings);
        const auto localSample = local.Sample(0.0f, 0.0f);
        Expect(std::isfinite(localSample.height)
                && std::isfinite(localSample.velocity)
                && std::isfinite(localSample.gradient.x)
                && std::isfinite(localSample.slopeMoments.z)
                && std::isfinite(localSample.foam)
                && std::abs(localSample.height) > 0.0f,
            "An inside local disturbance did not produce a finite wave.");

        Prism::Renderer::OceanLocalWaveSettings localFoamSettings =
            localSettings;
        localFoamSettings.foam.generationThreshold = 0.0f;
        localFoamSettings.foam.whitecapsThreshold = 0.25f;
        localFoamSettings.foam.generationAmount = 0.8f;
        localFoamSettings.foam.falloffSpeed = 0.985f;
        Prism::Renderer::LocalWaveSimulation localFoam;
        Expect(localFoam.Configure(localFoamSettings),
            "Local foam fixture failed to configure.");
        const Prism::Renderer::LocalWaveDisturbance foamImpulse{
            {0.0f, 0.0f}, 7.5f, 3.0f, {1.0f, 0.0f}};
        localFoam.AddDisturbances(
            std::span<const Prism::Renderer::LocalWaveDisturbance>(
                &foamImpulse, 1u));
        localFoam.Advance(1.0f / 60.0f, localFoamSettings);
        const auto maxLocalFoam = [&localFoam]()
        {
            float maximum = 0.0f;
            for (int z = -12; z <= 12; ++z)
            {
                for (int x = -12; x <= 12; ++x)
                {
                    maximum = std::max(maximum,
                        localFoam.Sample(static_cast<float>(x),
                            static_cast<float>(z)).foam);
                }
            }
            return maximum;
        };
        const float generatedLocalFoam = maxLocalFoam();
        Expect(generatedLocalFoam > 0.0f,
            "A breaking local impulse did not generate persistent foam.");
        const float generatedHeight = std::abs(
            localFoam.Sample(0.0f, 0.0f).height);
        localFoamSettings.foam.generationAmount = 0.0f;
        localFoam.Advance(1.0f / 60.0f, localFoamSettings);
        const float decayedLocalFoam = maxLocalFoam();
        Expect(decayedLocalFoam >= 0.0f
                && decayedLocalFoam < generatedLocalFoam
                && std::abs(localFoam.Sample(0.0f, 0.0f).height) > 0.0f
                && generatedHeight > 0.0f,
            "Disabling local foam generation must preserve wake displacement while existing foam decays.");
        local.Advance(10.0f, localSettings);
        const auto longFrameSample = local.Sample(0.0f, 0.0f);
        Expect(std::isfinite(longFrameSample.height)
                && std::isfinite(longFrameSample.velocity)
                && local.AllocatedMegabytes() > 0.0f,
            "Local wave long-frame integration became unstable.");
        local.Reset();
        const auto resetSample = local.Sample(0.0f, 0.0f);
        Expect(resetSample.height == 0.0f && resetSample.velocity == 0.0f
                && resetSample.foam == 0.0f,
            "Local wave reset did not clear displacement, velocity and foam.");
        Prism::Renderer::OceanLocalWaveSettings centeredLocalSettings =
            localSettings;
        centeredLocalSettings.domainCenter = {40.0f, -20.0f};
        Prism::Renderer::LocalWaveSimulation centeredLocal;
        Expect(centeredLocal.Configure(centeredLocalSettings),
            "Centered local wave domain failed to configure.");
        // The disturbance record is world-space; use a translated copy so
        // the same impulse lands at the configured domain center.
        Prism::Renderer::LocalWaveDisturbance centeredInside = inside;
        centeredInside.position = centeredLocalSettings.domainCenter;
        centeredLocal.Reset();
        centeredLocal.AddDisturbances(
            std::span<const Prism::Renderer::LocalWaveDisturbance>(
                &centeredInside, 1u));
        centeredLocal.Advance(0.5f, centeredLocalSettings);
        Expect(std::abs(centeredLocal.Sample(40.0f, -20.0f).height) > 0.0f,
            "A translated local domain did not preserve world-space sampling.");

        Prism::Renderer::OceanLocalWaveSettings emitterSettings = localSettings;
        emitterSettings.demoEmittersEnabled = true;
        emitterSettings.rainEmitterEnabled = true;
        emitterSettings.wakeEmitterEnabled = true;
        emitterSettings.emitterSeed = 0x12345678u;
        Prism::Renderer::LocalWaveSimulation emitterA;
        Prism::Renderer::LocalWaveSimulation emitterB;
        Expect(emitterA.Configure(emitterSettings)
                && emitterB.Configure(emitterSettings),
            "Emitter replay fixtures failed to configure.");
        emitterSettings.rainEmitterEnabled = false;
        emitterA.Advance(1.0f / 60.0f, emitterSettings);
        const auto wakeBatch = emitterA.LastFrameDisturbances();
        Expect(wakeBatch.size() == 100u
                && wakeBatch.front().position.x
                    != wakeBatch.back().position.x
                && wakeBatch.front().position.y
                    != wakeBatch.back().position.y
                && std::abs(wakeBatch[0].position.x) < 100.0f
                && std::abs(wakeBatch[0].position.y) < 100.0f
                && std::abs(wakeBatch.back().position.x) < 100.0f
                && std::abs(wakeBatch.back().position.y) < 100.0f
                && wakeBatch.front().velocityDirection.x > 0.0f,
            "The reference wake must publish the deterministic 10x10 moving-hull disturbance batch and let the solver form the wake.");

        auto wakeCentroid = [](const auto disturbances)
        {
            DirectX::XMFLOAT2 centroid{};
            for (const auto& disturbance : disturbances)
            {
                centroid.x += disturbance.position.x;
                centroid.y += disturbance.position.y;
            }
            const float inverseCount = disturbances.empty()
                ? 0.0f
                : 1.0f / static_cast<float>(disturbances.size());
            centroid.x *= inverseCount;
            centroid.y *= inverseCount;
            return centroid;
        };
        float minimumWakeX = wakeCentroid(wakeBatch).x;
        float maximumWakeX = minimumWakeX;
        float minimumWakeZ = wakeCentroid(wakeBatch).y;
        float maximumWakeZ = minimumWakeZ;
        for (std::uint32_t frame = 0u; frame < 900u; ++frame)
        {
            emitterA.AdvanceEmitters(1.0f / 60.0f, emitterSettings);
            const DirectX::XMFLOAT2 centroid = wakeCentroid(
                emitterA.LastFrameDisturbances());
            minimumWakeX = std::min(minimumWakeX, centroid.x);
            maximumWakeX = std::max(maximumWakeX, centroid.x);
            minimumWakeZ = std::min(minimumWakeZ, centroid.y);
            maximumWakeZ = std::max(maximumWakeZ, centroid.y);
        }
        Expect(minimumWakeX
                    < emitterSettings.domainCenter.x - 20.0f
                && maximumWakeX
                    > emitterSettings.domainCenter.x + 20.0f
                && minimumWakeZ
                    < emitterSettings.domainCenter.y - 12.0f
                && maximumWakeZ
                    > emitterSettings.domainCenter.y + 12.0f,
            "The invisible hull wake did not complete an observable foreground/background orbit inside the local domain.");
        Prism::Renderer::LocalWaveSimulation gpuEmitter;
        gpuEmitter.AdvanceEmitters(1.0f / 60.0f, emitterSettings);
        Expect(gpuEmitter.LastFrameDisturbances().size() == 100u
                && gpuEmitter.GridSize() == 0u
                && gpuEmitter.AllocatedMegabytes() == 0.0f,
            "The GPU emitter path must publish the same moving-hull batch without allocating or integrating the CPU oracle grids.");
        emitterA.Reset();
        emitterB.Reset();
        emitterSettings.rainEmitterEnabled = true;
        for (std::uint32_t frame = 0u; frame < 60u; ++frame)
        {
            emitterA.Advance(1.0f / 60.0f, emitterSettings);
            emitterB.Advance(1.0f / 60.0f, emitterSettings);
        }
        const auto emitterSampleA = emitterA.Sample(0.0f, 0.0f);
        const auto emitterSampleB = emitterB.Sample(0.0f, 0.0f);
        Expect(emitterA.EmitterSequence() > 0u
                && emitterA.EmitterSequence() == emitterB.EmitterSequence()
                && !emitterA.LastFrameDisturbances().empty()
                && std::abs(emitterSampleA.height - emitterSampleB.height)
                    < 1.0e-6f,
            "Deterministic rain/wake emitters failed replay equivalence.");
        emitterA.ResetEmitters();
        Expect(emitterA.EmitterSequence() == 0u,
            "Local emitter reset did not clear its sequence.");

        const auto noDirtyScopes =
            Prism::Renderer::ClassifyOceanDirtyScopes(reference, reference);
        Expect(noDirtyScopes == Prism::Renderer::OceanDirtyScope::None,
            "Identical ocean settings must not schedule any rebuild.");
        Prism::Renderer::OceanSettings sunOnly = reference;
        sunOnly.shading.sunIntensity += 0.5f;
        const auto sunScopes =
            Prism::Renderer::ClassifyOceanDirtyScopes(reference, sunOnly);
        Expect(Prism::Renderer::HasDirtyScope(
                   sunScopes, Prism::Renderer::OceanDirtyScope::Constants)
                && !Prism::Renderer::HasDirtyScope(
                   sunScopes, Prism::Renderer::OceanDirtyScope::InitialSpectrum)
                && !Prism::Renderer::HasDirtyScope(
                   sunScopes, Prism::Renderer::OceanDirtyScope::Resources),
            "Shading-only edits must not rebuild ocean simulation resources.");
        Prism::Renderer::OceanSettings timeOnly = reference;
        timeOnly.timeScale = 0.0f;
        const auto timeScopes =
            Prism::Renderer::ClassifyOceanDirtyScopes(reference, timeOnly);
        Expect(Prism::Renderer::HasDirtyScope(
                   timeScopes, Prism::Renderer::OceanDirtyScope::Constants)
                && !Prism::Renderer::HasDirtyScope(
                   timeScopes, Prism::Renderer::OceanDirtyScope::InitialSpectrum)
                && !Prism::Renderer::HasDirtyScope(
                   timeScopes, Prism::Renderer::OceanDirtyScope::HistoryReset),
            "Time scale must not rebuild H0 or reset foam history.");
        Prism::Renderer::OceanSettings lateralOnly = reference;
        lateralOnly.lateralMultiplier += 0.25f;
        const auto lateralScopes =
            Prism::Renderer::ClassifyOceanDirtyScopes(reference, lateralOnly);
        Expect(!Prism::Renderer::HasDirtyScope(
                   lateralScopes, Prism::Renderer::OceanDirtyScope::InitialSpectrum),
            "Lateral multiplier must not rebuild H0.");
        Prism::Renderer::OceanSettings windOnly = reference;
        windOnly.baseWind.speed += 1.0f;
        const auto windScopes =
            Prism::Renderer::ClassifyOceanDirtyScopes(reference, windOnly);
        Expect(Prism::Renderer::HasDirtyScope(
                   windScopes, Prism::Renderer::OceanDirtyScope::InitialSpectrum),
            "Wind edits must invalidate the initial spectrum.");
        Prism::Renderer::OceanSettings qualityOnly = reference;
        qualityOnly.quality = Prism::Renderer::OceanSimulationQuality::High;
        const auto qualityScopes =
            Prism::Renderer::ClassifyOceanDirtyScopes(reference, qualityOnly);
        Expect(Prism::Renderer::HasDirtyScope(
                   qualityScopes, Prism::Renderer::OceanDirtyScope::Resources)
                && Prism::Renderer::HasDirtyScope(
                   qualityScopes, Prism::Renderer::OceanDirtyScope::InitialSpectrum),
            "Quality edits must rebuild resolution-dependent simulation resources.");
        Prism::Renderer::OceanSettings localDomain = reference;
        localDomain.local.domainSizeMeters *= 0.5f;
        const auto localScopes =
            Prism::Renderer::ClassifyOceanDirtyScopes(reference, localDomain);
        Expect(Prism::Renderer::HasDirtyScope(
                   localScopes, Prism::Renderer::OceanDirtyScope::Resources)
                && Prism::Renderer::HasDirtyScope(
                   localScopes, Prism::Renderer::OceanDirtyScope::LocalReset),
            "Local-domain edits must rebuild and reset the local solver.");
        Prism::Renderer::OceanSettings foamOnly = reference;
        foamOnly.foam.whitecapsThreshold += 0.1f;
        const auto foamScopes =
            Prism::Renderer::ClassifyOceanDirtyScopes(reference, foamOnly);
        Expect(Prism::Renderer::HasDirtyScope(
                   foamScopes, Prism::Renderer::OceanDirtyScope::HistoryReset),
            "Foam threshold edits must reset persistent foam history.");

        // GPU query lifecycle: submission never blocks, completion is tagged,
        // CPU payload is demand-driven, and the bounded history expires the
        // oldest simulation time rather than returning unrelated current data.
        Prism::Renderer::OceanQueryService queries;
        Prism::Renderer::OceanSettings querySettings = reference;
        querySettings.query.readbackFifoEntries = 2u;
        queries.Configure(querySettings);
        const std::array points{
            Prism::Renderer::OceanQueryPoint{{0.0f, 0.0f}},
            Prism::Renderer::OceanQueryPoint{{12.0f, -4.0f}}};
        const std::array samplesA{
            Prism::Renderer::OceanDisplacementSample{{1.0f, 2.0f, 3.0f}},
            Prism::Renderer::OceanDisplacementSample{{4.0f, 5.0f, 6.0f}}};
        Expect(queries.SubmitGpuBatch(101u, 10.0, 20u, points),
            "GPU query batch submission failed.");
        Prism::Renderer::OceanQueryResult pending{};
        Expect(queries.Poll(101u, &pending)
                == Prism::Renderer::OceanQueryState::Pending
                && pending.samples.empty(),
            "GPU query submission must remain pending without blocking.");
        Expect(queries.HasAllocatedQueryResource()
                && queries.ReadbackCopyRequestCount() == 0u,
            "Query readback was scheduled before a CPU consumer requested it.");
        Expect(queries.CompleteGpuBatch(101u, 21u, samplesA),
            "GPU query completion failed.");
        Expect(queries.Poll(101u) == Prism::Renderer::OceanQueryState::Completed
                && queries.RequestCpuReadback(101u),
            "Completed query did not accept demand-driven CPU readback.");
        Prism::Renderer::OceanQueryResult completed{};
        Expect(queries.Poll(101u, &completed)
                == Prism::Renderer::OceanQueryState::Completed
                && completed.samples.size() == 2u
                && completed.samples[1].displacement.z == 6.0f,
            "Completed query payload or sequence tag is incorrect.");
        Expect(queries.ReadbackCopyRequestCount() == 1u,
            "A completed query did not schedule exactly one demand-driven readback.");
        Expect(queries.FindHistorical(10.0, &completed)
                == Prism::Renderer::OceanQueryState::Completed,
            "Completed query was not available through historical lookup.");
        Expect(queries.FindHistorical(9.0)
                == Prism::Renderer::OceanQueryState::Expired,
            "Unknown historical time must not alias the current result.");
        for (std::uint64_t sequence = 102u; sequence <= 103u; ++sequence)
        {
            Expect(queries.SubmitGpuBatch(sequence,
                    static_cast<double>(sequence), 30u, points),
                "Historical FIFO submission failed.");
            Expect(queries.CompleteGpuBatch(sequence, 31u, samplesA),
                "Historical FIFO completion failed.");
        }
        Expect(queries.Poll(101u) == Prism::Renderer::OceanQueryState::Expired
                && queries.ExpiredCount() > 0u,
            "Historical FIFO did not expire the oldest tagged query.");
        Prism::Renderer::OceanSettings disabledQueries = querySettings;
        disabledQueries.query.enableGpuQueries = false;
        disabledQueries.query.enableCpuReadback = false;
        queries.Configure(disabledQueries);
        Expect(!queries.SubmitGpuBatch(200u, 20.0, 40u, points)
                && !queries.RequestCpuReadback(103u),
            "Disabled query modes must not allocate or schedule work.");
        Expect(queries.GpuBatchCount() == 3u,
            "Disabled query modes unexpectedly allocated a GPU query batch.");
        Prism::Renderer::OceanQueryService disabledService;
        disabledService.Configure(disabledQueries);
        Expect(!disabledService.HasAllocatedQueryResource()
                && disabledService.GpuBatchCount() == 0u
                && !disabledService.SubmitGpuBatch(201u, 21.0, 41u, points),
            "Disabling both query modes must not allocate query resources.");

        const auto patch = Prism::Renderer::OceanQuadtree::BuildPatchGeometry(
            8u,
            Prism::Renderer::OceanPatchEdge::North
                | Prism::Renderer::OceanPatchEdge::West);
        Expect(patch.vertices.size() == 81u && patch.indices.size() == 384u,
            "Ocean patch geometry has an invalid watertight topology.");
        for (std::uint32_t mask = 0u; mask < 16u; ++mask)
        {
            const auto variant = Prism::Renderer::OceanQuadtree::BuildPatchGeometry(
                8u, static_cast<Prism::Renderer::OceanPatchEdge>(mask));
            for (const std::uint32_t index : variant.indices)
                Expect(index < variant.vertices.size(),
                    "Ocean patch edge variant emitted an out-of-range index.");
            for (std::size_t index = 0u; index + 2u < variant.indices.size(); index += 3u)
                Expect(!(variant.indices[index] == variant.indices[index + 1u]
                        || variant.indices[index] == variant.indices[index + 2u]
                        || variant.indices[index + 1u] == variant.indices[index + 2u]),
                    "Ocean patch edge variant emitted a degenerate triangle.");
        }
        const Prism::Renderer::OceanQuadtreeSettings quadtreeSettings{};
        const auto selection = Prism::Renderer::OceanQuadtree::Select(
            quadtreeSettings, {0.0f, 20.0f, 0.0f}, 1200.0f);
        Expect(!selection.nodes.empty() && selection.refinementIterations > 0u,
            "Ocean quadtree did not refine a deterministic camera fixture.");
        Prism::Renderer::OceanQuadtreeSettings pathologicalSettings{};
        pathologicalSettings.minimumPatchSizeMeters = 0.25f;
        pathologicalSettings.maximumScreenEdgePixels = 1.0f;
        pathologicalSettings.maximumViewDistanceMeters = 100000.0f;
        pathologicalSettings.maximumLod = 10u;
        pathologicalSettings.maximumNodeCount = 256u;
        const auto boundedSelection = Prism::Renderer::OceanQuadtree::Select(
            pathologicalSettings, {0.0f, 10.0f, 0.0f}, 100000.0f);
        if (boundedSelection.nodes.empty()
            || boundedSelection.nodes.size()
                > pathologicalSettings.maximumNodeCount)
        {
            std::cerr << "Bounded ocean selection node count: "
                      << boundedSelection.nodes.size() << '\n';
        }
        Expect(!boundedSelection.nodes.empty()
                && boundedSelection.nodes.size()
                    <= pathologicalSettings.maximumNodeCount,
            "Ocean quadtree exceeded its hard node budget for an editor far plane.");
        for (std::size_t i = 0u; i < selection.nodes.size(); ++i)
            for (std::size_t j = i + 1u; j < selection.nodes.size(); ++j)
            {
                const float dx = std::abs(selection.nodes[i].center.x
                    - selection.nodes[j].center.x);
                const float dz = std::abs(selection.nodes[i].center.y
                    - selection.nodes[j].center.y);
                if (dx <= selection.nodes[i].halfExtent
                        + selection.nodes[j].halfExtent
                    && dz <= selection.nodes[i].halfExtent
                        + selection.nodes[j].halfExtent)
                {
                    Expect(std::abs(static_cast<int>(selection.nodes[i].lod)
                            - static_cast<int>(selection.nodes[j].lod)) <= 1,
                        "Ocean quadtree selection left an unbalanced neighbor.");
                }
            }
        Expect(Prism::Renderer::OceanQuadtree::GroupForInstances(selection).size()
                == selection.nodes.size(),
            "Ocean quadtree instance grouping dropped selected nodes.");
        const auto instanceData =
            Prism::Renderer::OceanQuadtree::BuildInstanceData(selection);
        Expect(instanceData.size() == selection.nodes.size(),
            "Ocean quadtree instance payload count changed during upload preparation.");
        for (std::size_t index = 1u; index < instanceData.size(); ++index)
        {
            const auto& previous = instanceData[index - 1u].morphEdgeLod;
            const auto& current = instanceData[index].morphEdgeLod;
            Expect(previous.y < current.y
                    || (previous.y == current.y && previous.z <= current.z),
                "Ocean quadtree instance payloads are not grouped by patch topology and LOD.");
        }
        Prism::Renderer::OceanQuadtreeFrustum rejectAll{};
        rejectAll.enabled = true;
        for (auto& plane : rejectAll.planes)
            plane = {0.0f, 1.0f, 0.0f, -10000.0f};
        Expect(Prism::Renderer::OceanQuadtree::Select(
                    quadtreeSettings, {0.0f, 20.0f, 0.0f}, 1200.0f,
                    rejectAll).nodes.empty(),
            "Ocean quadtree frustum culling retained a rejected root.");

        // Deterministic camera-path coverage used by the clipmap/quadtree
        // visual gate.  Every reference pose must retain finite geometry,
        // conservative displacement margin, and a stable snap interval.
        const std::array<DirectX::XMFLOAT3, 5> referenceCameras{
            DirectX::XMFLOAT3{0.0f, 2.0f, 0.0f},
            DirectX::XMFLOAT3{40.0f, 8.0f, -35.0f},
            DirectX::XMFLOAT3{0.0f, 400.0f, 0.0f},
            DirectX::XMFLOAT3{0.0f, 40.0f, 2800.0f},
            DirectX::XMFLOAT3{620.0f, 18.0f, 620.0f}};
        for (const auto& camera : referenceCameras)
        {
            const auto pathSelection = Prism::Renderer::OceanQuadtree::Select(
                quadtreeSettings, camera, 4000.0f);
            Expect(!pathSelection.nodes.empty(),
                "Reference ocean camera path exposed a geometry coverage hole.");
            for (const auto& node : pathSelection.nodes)
            {
                Expect(std::isfinite(node.center.x)
                        && std::isfinite(node.center.y)
                        && std::isfinite(node.halfExtent)
                        && node.halfExtent > 0.0f,
                    "Reference ocean camera path produced non-finite patch bounds.");
                Expect(node.halfExtent + quadtreeSettings.conservativeDisplacementMeters
                        > 0.0f,
                    "Ocean displacement margin did not cover the selected patch.");
            }
        }

        feature.Reset();
        Expect(
            feature.GetSettings().implementation
                == Prism::Renderer::OceanImplementation::LegacyFft,
            "Reset must restore the legacy migration implementation.");
        std::cout << "Ocean settings tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
