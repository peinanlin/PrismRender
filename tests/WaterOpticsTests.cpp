#include "Renderer/Features/Ocean/OceanSettings.h"
#include "Renderer/Features/Ocean/OceanStatistics.h"
#include "Renderer/Features/Ocean/WaterOpticsHistory.h"
#include "Renderer/Features/Ocean/WaterCaustics.h"
#include "Renderer/Features/Ocean/WaterMediumState.h"
#include "Renderer/Features/Ocean/WaterVolumetricHistory.h"
#include "Renderer/Features/Ocean/WaterOpticalModel.h"
#include "Renderer/Features/Ocean/WaterRefractionModel.h"
#include "Renderer/Features/Ocean/WaterOpticsSettings.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
void Expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace

int main()
{
    try
    {
        using namespace Prism::Renderer;

        OceanSettings ocean{};
        Expect(ocean.opticsModel == OceanOpticsModel::Current,
            "Existing oceans must default to the current optics model.");

        WaterOpticsSettings reference = WaterOpticsSettings::HpWaterReference();
        std::string warning;
        Expect(reference.ValidateAndNormalize(&warning) && warning.empty(),
            "HPWater reference optics must be valid without adjustment.");
        Expect(reference.distance.nearEndMeters
                    < reference.distance.middleEndMeters
                && reference.distance.middleEndMeters
                    < reference.distance.farEndMeters,
            "HPWater distance tiers must be strictly ordered.");
        const WaterOpticsQualityPolicy normalPolicy =
            GetWaterOpticsQualityPolicy(WaterOpticsQuality::Normal);
        const WaterOpticsQualityPolicy highPolicy =
            GetWaterOpticsQualityPolicy(WaterOpticsQuality::High);
        const WaterOpticsQualityPolicy extremePolicy =
            GetWaterOpticsQualityPolicy(WaterOpticsQuality::Extreme);
        Expect(normalPolicy.refractionResolutionScale
                    < highPolicy.refractionResolutionScale
                && highPolicy.refractionResolutionScale
                    < extremePolicy.refractionResolutionScale
                && normalPolicy.maximumRayMarchSamples == 0u
                && highPolicy.maximumRayMarchSamples > 0u
                && extremePolicy.maximumRayMarchSamples > 0u,
            "Optics quality policy does not increase only optical work monotonically.");

        WaterOpticsSettings invalid = reference;
        auto modeTest = reference;
        modeTest.caustics.mode = WaterCausticsMode::Chromatic;
        modeTest.quality = WaterOpticsQuality::Normal;
        Expect(GetEffectiveWaterCausticsMode(modeTest) == WaterCausticsMode::SingleChannel,
            "Normal quality must report the actual single-channel fallback.");
        modeTest.quality = WaterOpticsQuality::High;
        Expect(GetEffectiveWaterCausticsMode(modeTest) == WaterCausticsMode::Chromatic,
            "High quality must preserve RGB dispersion.");
        modeTest.caustics.enabled = false;
        Expect(GetEffectiveWaterCausticsMode(modeTest) == WaterCausticsMode::Disabled,
            "Disabled caustics must be reported as disabled.");
        for (std::uint32_t debug = 0u; debug <= 13u; ++debug)
        {
            auto debugSettings = reference;
            debugSettings.debugView = static_cast<WaterOpticsDebugView>(debug);
            const auto dirty = ClassifyWaterOpticsDirtyScopes(reference, debugSettings);
            Expect(!HasDirtyScope(dirty, WaterOpticsDirtyScope::History)
                    && !HasDirtyScope(dirty, WaterOpticsDirtyScope::Resources),
                "Debug selection must not change optical resources or histories.");
            auto debugOcean = OceanSettings::HpWaterReference();
            const auto previousOcean = debugOcean;
            debugOcean.optics = debugSettings;
            Expect(!HasDirtyScope(ClassifyOceanDirtyScopes(previousOcean, debugOcean), OceanDirtyScope::InitialSpectrum),
                "Optical debug changes must not rebuild spectrum.");
        }
        invalid.material.absorption.x =
            std::numeric_limits<float>::quiet_NaN();
        invalid.material.scattering.y = -4.0f;
        invalid.material.indexOfRefraction = 0.2f;
        invalid.refraction.rayMarchSampleCount = 0u;
        invalid.refraction.rayStepScale =
            std::numeric_limits<float>::infinity();
        invalid.caustics.resolution = 333u;
        invalid.caustics.middleCoverageMeters = 2.0f;
        invalid.volumetrics.resolutionScale = 0.0f;
        invalid.volumetrics.historyWeight = 4.0f;
        invalid.distance.middleEndMeters = 10.0f;
        invalid.distance.farEndMeters = 5.0f;
        Expect(!invalid.ValidateAndNormalize(&warning) && !warning.empty(),
            "Invalid optics values must be reported and normalized.");
        Expect(std::isfinite(invalid.material.absorption.x)
                && invalid.material.scattering.y >= 0.0f
                && invalid.material.indexOfRefraction > 1.0f,
            "Optical material normalization produced invalid coefficients.");
        Expect(invalid.refraction.rayMarchSampleCount >= 1u
                && invalid.refraction.rayMarchSampleCount <= 64u
                && invalid.caustics.resolution == 256u,
            "Optical discrete settings were not normalized.");
        Expect(invalid.distance.nearEndMeters
                    < invalid.distance.middleEndMeters
                && invalid.distance.middleEndMeters
                    < invalid.distance.farEndMeters,
            "Optical tier ordering was not repaired.");

        const auto SumTierWeights = [](const WaterDistanceTierWeights& value) {
            return value.nearWeight + value.middleWeight + value.farWeight;
        };
        const WaterDistanceTierWeights atCamera =
            EvaluateWaterDistanceTierWeights(0.0f, reference.distance);
        const WaterDistanceTierWeights atNearBoundary =
            EvaluateWaterDistanceTierWeights(
                reference.distance.nearEndMeters, reference.distance);
        const WaterDistanceTierWeights atMiddleBoundary =
            EvaluateWaterDistanceTierWeights(
                reference.distance.middleEndMeters, reference.distance);
        const WaterDistanceTierWeights beyondFar =
            EvaluateWaterDistanceTierWeights(
                reference.distance.farEndMeters * 4.0f,
                reference.distance);
        Expect(atCamera.nearWeight == 1.0f
                && atNearBoundary.nearWeight > 0.0f
                && atNearBoundary.middleWeight > 0.0f
                && atMiddleBoundary.middleWeight > 0.0f
                && atMiddleBoundary.farWeight > 0.0f
                && beyondFar.farWeight == 1.0f,
            "Distance tier boundaries or horizon-preserving far tier are invalid.");
        for (const WaterDistanceTierWeights weights : {
                 atCamera, atNearBoundary, atMiddleBoundary, beyondFar})
        {
            Expect(std::isfinite(weights.nearWeight)
                    && std::isfinite(weights.middleWeight)
                    && std::isfinite(weights.farWeight)
                    && std::abs(SumTierWeights(weights) - 1.0f) < 1.0e-6f,
                "Distance tier weights must be finite and normalized.");
        }
        const float boundaryEpsilon = 0.001f;
        const WaterDistanceTierWeights boundaryLeft =
            EvaluateWaterDistanceTierWeights(
                reference.distance.nearEndMeters - boundaryEpsilon,
                reference.distance);
        const WaterDistanceTierWeights boundaryRight =
            EvaluateWaterDistanceTierWeights(
                reference.distance.nearEndMeters + boundaryEpsilon,
                reference.distance);
        Expect(std::abs(boundaryLeft.nearWeight
                    - boundaryRight.nearWeight) < 0.001f
                && std::abs(boundaryLeft.middleWeight
                    - boundaryRight.middleWeight) < 0.001f,
            "Distance tier transition is not continuous at the near boundary.");
        const WaterDistanceTierWeights cameraRelativeA =
            EvaluateWaterDistanceTierWeights(
                DirectX::XMFLOAT3{1000.0f, 20.0f, -500.0f},
                DirectX::XMFLOAT3{1100.0f, 20.0f, -500.0f},
                reference.distance);
        const WaterDistanceTierWeights cameraRelativeB =
            EvaluateWaterDistanceTierWeights(
                DirectX::XMFLOAT3{501000.0f, 20.0f, 799500.0f},
                DirectX::XMFLOAT3{501100.0f, 20.0f, 799500.0f},
                reference.distance);
        Expect(std::abs(cameraRelativeA.nearWeight
                    - cameraRelativeB.nearWeight) < 1.0e-6f
                && std::abs(cameraRelativeA.middleWeight
                    - cameraRelativeB.middleWeight) < 1.0e-6f,
            "Distance tiers must depend on camera-relative separation, not world origin.");

        WaterCausticsSettings causticSettings = reference.caustics;
        causticSettings.resolution = 256u;
        causticSettings.nearCoverageMeters = 64.0f;
        causticSettings.middleCoverageMeters = 256.0f;
        const WaterCausticCascadeSet causticCascadesA =
            BuildWaterCausticCascadeSet({10.01f, -7.99f},
                causticSettings);
        const WaterCausticCascadeSet causticCascadesB =
            BuildWaterCausticCascadeSet({10.08f, -7.92f},
                causticSettings);
        Expect(causticCascadesA.cascades[0].centerMeters.x
                    == causticCascadesB.cascades[0].centerMeters.x
                && causticCascadesA.cascades[0].centerMeters.y
                    == causticCascadesB.cascades[0].centerMeters.y,
            "Sub-texel camera motion changed the snapped caustic transform.");
        const WaterCausticCascadeWeights causticCenter =
            EvaluateWaterCausticCascadeWeights(
                causticCascadesA.cascades[0].centerMeters,
                causticCascadesA);
        const DirectX::XMFLOAT2 nearEdge{
            causticCascadesA.cascades[0].centerMeters.x + 31.0f,
            causticCascadesA.cascades[0].centerMeters.y};
        const WaterCausticCascadeWeights causticEdge =
            EvaluateWaterCausticCascadeWeights(nearEdge,
                causticCascadesA);
        const DirectX::XMFLOAT2 outsideMiddle{
            causticCascadesA.cascades[1].centerMeters.x + 200.0f,
            causticCascadesA.cascades[1].centerMeters.y + 200.0f};
        const WaterCausticCascadeWeights causticOutside =
            EvaluateWaterCausticCascadeWeights(outsideMiddle,
                causticCascadesA);
        Expect(causticCenter.nearWeight == 1.0f
                && causticCenter.coverageWeight == 1.0f
                && causticEdge.nearWeight < 1.0f
                && causticEdge.middleWeight > 0.0f
                && causticEdge.coverageWeight <= 1.0f
                && causticOutside.coverageWeight == 0.0f,
            "Caustic cascade coverage did not normalize or fade at its edges.");

        WaterCausticHistoryState causticHistory;
        WaterCausticHistoryKey causticHistoryKey{};
        causticHistoryKey.cascades = causticCascadesA;
        causticHistoryKey.surfaceHistoryVersion = 4u;
        Expect(causticHistory.Update(causticHistoryKey)
                && causticHistory.IsValid(),
            "First caustic history key did not initialize history.");
        causticHistoryKey.cascades = causticCascadesB;
        Expect(!causticHistory.Update(causticHistoryKey),
            "Stable sub-texel caustic snapping invalidated history.");
        causticSettings.resolution = 512u;
        causticHistoryKey.cascades = BuildWaterCausticCascadeSet(
            {10.08f, -7.92f}, causticSettings);
        Expect(causticHistory.Update(causticHistoryKey)
                && (static_cast<std::uint32_t>(
                        causticHistory.GetLastInvalidation())
                    & static_cast<std::uint32_t>(
                        WaterCausticHistoryInvalidation::Resources)) != 0u,
            "Caustic resize did not invalidate its history resources.");
        causticHistoryKey.cascades = BuildWaterCausticCascadeSet(
            {1000.0f, 1000.0f}, causticSettings);
        Expect(causticHistory.Update(causticHistoryKey)
                && (static_cast<std::uint32_t>(
                        causticHistory.GetLastInvalidation())
                    & static_cast<std::uint32_t>(
                        WaterCausticHistoryInvalidation::CameraJump)) != 0u,
            "A large camera jump retained stale caustic history.");

        WaterMediumState mediumState;
        WaterMediumSample mediumSample{};
        mediumSample.cameraHeightMeters = 1.0f;
        mediumSample.meanSeaLevelMeters = 0.0f;
        mediumSample.hysteresisMeters = 0.2f;
        WaterMediumResult medium = mediumState.Update(mediumSample);
        Expect(medium.medium == WaterCameraMedium::AboveWater
                && medium.usedMeanSeaLevelFallback
                && medium.transitioned,
            "Unavailable camera query did not use the mean-sea-level fallback.");
        mediumSample.cameraHeightMeters = -0.3f;
        mediumSample.coherentQueryAvailable = true;
        mediumSample.queriedDisplacementMeters = 0.0f;
        mediumSample.queryVersion = 7u;
        medium = mediumState.Update(mediumSample);
        Expect(medium.medium == WaterCameraMedium::Underwater
                && medium.transitioned && !medium.usedMeanSeaLevelFallback
                && medium.queryVersion == 7u,
            "Coherent camera query did not enter the underwater medium.");
        for (const float oscillatingHeight : {-0.1f, 0.1f, -0.05f, 0.05f})
        {
            mediumSample.cameraHeightMeters = oscillatingHeight;
            medium = mediumState.Update(mediumSample);
            Expect(medium.medium == WaterCameraMedium::Underwater
                    && !medium.transitioned,
                "Waterline hysteresis oscillated the camera medium.");
        }
        mediumSample.cameraHeightMeters = 0.25f;
        medium = mediumState.Update(mediumSample);
        Expect(medium.medium == WaterCameraMedium::AboveWater
                && medium.transitioned,
            "Camera did not leave the medium above the hysteresis band.");
        mediumState.Reset(true);
        mediumSample.cameraHeightMeters = -1.0f;
        medium = mediumState.Update(mediumSample);
        Expect(medium.medium == WaterCameraMedium::Underwater
                && medium.reason == WaterMediumTransitionReason::SceneReset,
            "Scene switch did not reset and reclassify the camera medium.");
        mediumState.Reset(false);
        mediumSample.cameraHeightMeters = 1.0f;
        medium = mediumState.Update(mediumSample);
        Expect(medium.medium == WaterCameraMedium::AboveWater
                && medium.reason == WaterMediumTransitionReason::ExplicitReset,
            "Explicit reset did not clear the camera medium state.");

        WaterVolumetricHistoryState volumetricHistory;
        WaterVolumetricHistoryKey volumetricKey{};
        volumetricKey.width = 640u;
        volumetricKey.height = 400u;
        volumetricKey.quality = WaterOpticsQuality::High;
        volumetricKey.opticalHistoryVersion = 3u;
        volumetricKey.surfaceHistoryVersion = 4u;
        volumetricKey.mediumVersion = medium.version;
        volumetricKey.sceneVersion = 2u;
        Expect(volumetricHistory.Update(volumetricKey)
                && !volumetricHistory.IsValid(),
            "New volumetric history was sampled before its first commit.");
        volumetricHistory.Commit();
        Expect(volumetricHistory.IsValid()
                && !volumetricHistory.Update(volumetricKey),
            "Stable volumetric history key did not remain valid.");
        for (std::uint32_t change = 0u; change < 6u; ++change)
        {
            WaterVolumetricHistoryKey changed = volumetricKey;
            if (change == 0u) ++changed.width;
            if (change == 1u) changed.quality = WaterOpticsQuality::Extreme;
            if (change == 2u) ++changed.opticalHistoryVersion;
            if (change == 3u) ++changed.surfaceHistoryVersion;
            if (change == 4u) ++changed.mediumVersion;
            if (change == 5u) ++changed.sceneVersion;
            Expect(volumetricHistory.Update(changed)
                    && !volumetricHistory.IsValid(),
                "Resize/quality/cut/surface/medium/scene change retained stale volumetrics.");
            volumetricHistory.Commit();
            Expect(volumetricHistory.Update(volumetricKey),
                "Volumetric history did not restore the baseline key.");
            volumetricHistory.Commit();
        }
        Expect(!RejectWaterVolumetricHistory(
                8.0f, 8.1f, 0.2f, true, true)
                && RejectWaterVolumetricHistory(
                    8.0f, 9.0f, 0.2f, true, true)
                && RejectWaterVolumetricHistory(
                    8.0f, 8.0f, 0.2f, false, true)
                && RejectWaterVolumetricHistory(
                    8.0f, 8.0f, 0.2f, true, false),
            "Depth, motion, or invalid-history rejection policy is incorrect.");

        WaterOpticsSettings materialEdit = reference;
        materialEdit.material.absorption.x += 0.1f;
        const WaterOpticsDirtyScope materialScopes =
            ClassifyWaterOpticsDirtyScopes(reference, materialEdit);
        Expect(HasDirtyScope(materialScopes,
                   WaterOpticsDirtyScope::Constants)
                && !HasDirtyScope(materialScopes,
                    WaterOpticsDirtyScope::Resources),
            "Material edits must update constants without reallocating optics.");

        WaterOpticsSettings qualityEdit = reference;
        qualityEdit.quality = WaterOpticsQuality::Extreme;
        const WaterOpticsDirtyScope qualityScopes =
            ClassifyWaterOpticsDirtyScopes(reference, qualityEdit);
        Expect(HasDirtyScope(qualityScopes,
                   WaterOpticsDirtyScope::Resources)
                && HasDirtyScope(qualityScopes,
                    WaterOpticsDirtyScope::History),
            "Optics quality edits must rebuild optical resources and history.");

        OceanSettings before = OceanSettings::WaveWorksReference();
        const OceanSettings hpWater = OceanSettings::HpWaterReference();
        Expect(hpWater.opticsModel == OceanOpticsModel::HpWater
                && hpWater.implementation == before.implementation
                && hpWater.quality == before.quality
                && hpWater.simulationApi == before.simulationApi
                && hpWater.simulationPeriodMeters
                    == before.simulationPeriodMeters
                && hpWater.baseWind.speed == before.baseWind.speed
                && hpWater.swell.speed == before.swell.speed
                && hpWater.local.domainCenter.x
                    == before.local.domainCenter.x
                && hpWater.local.domainCenter.y
                    == before.local.domainCenter.y
                && hpWater.geometry.maximumLod
                    == before.geometry.maximumLod
                && hpWater.geometry.minimumPatchLength
                    == before.geometry.minimumPatchLength
                && hpWater.geometry.cellsPerPatch
                    == before.geometry.cellsPerPatch,
            "HPWater must reuse WaveWorks spectral, local-wave, and adaptive-geometry inputs.");
        OceanSettings after = before;
        after.opticsModel = OceanOpticsModel::HpWater;
        after.optics.material.scattering.z += 0.2f;
        const OceanDirtyScope oceanScopes =
            ClassifyOceanDirtyScopes(before, after);
        Expect(HasDirtyScope(oceanScopes, OceanDirtyScope::Constants),
            "Optics selection must publish new renderer constants.");
        Expect(!HasDirtyScope(oceanScopes, OceanDirtyScope::InitialSpectrum)
                && !HasDirtyScope(oceanScopes, OceanDirtyScope::Resources)
                && !HasDirtyScope(oceanScopes, OceanDirtyScope::LocalReset)
                && !HasDirtyScope(oceanScopes, OceanDirtyScope::HistoryReset),
            "Optics-only edits must not rebuild H0, simulation resources, local waves, or spectral foam history.");

        WaterOpticsHistoryKey historyKey{};
        historyKey.width = 1280u;
        historyKey.height = 800u;
        historyKey.cameraCutVersion = 3u;
        historyKey.sceneVersion = 7u;
        historyKey.surfaceHistoryVersion = 11u;
        WaterOpticsHistoryState history;
        Expect(history.Update(historyKey) && history.IsValid()
                && history.GetVersion() == 1u,
            "The first optical history key must initialize history.");
        Expect(!history.Update(historyKey) && history.GetVersion() == 1u,
            "An unchanged optical history key must retain history.");

        const auto ExpectInvalidation = [&history, &historyKey](
                                            auto mutate,
                                            WaterOpticsHistoryInvalidation flag,
                                            const char* message) {
            WaterOpticsHistoryKey changed = historyKey;
            mutate(changed);
            Expect(history.Update(changed)
                    && HasHistoryInvalidation(
                        history.GetLastInvalidation(), flag),
                message);
            historyKey = changed;
        };
        ExpectInvalidation([](WaterOpticsHistoryKey& key) { ++key.width; },
            WaterOpticsHistoryInvalidation::Extent,
            "Resize must invalidate optical history.");
        ExpectInvalidation([](WaterOpticsHistoryKey& key) {
            key.quality = WaterOpticsQuality::Extreme;
        }, WaterOpticsHistoryInvalidation::Quality,
            "Quality changes must invalidate optical history.");
        ExpectInvalidation([](WaterOpticsHistoryKey& key) {
            ++key.cameraCutVersion;
        }, WaterOpticsHistoryInvalidation::CameraCut,
            "Camera cuts must invalidate optical history.");
        ExpectInvalidation([](WaterOpticsHistoryKey& key) {
            ++key.sceneVersion;
        }, WaterOpticsHistoryInvalidation::Scene,
            "Scene changes must invalidate optical history.");
        ExpectInvalidation([](WaterOpticsHistoryKey& key) {
            ++key.surfaceHistoryVersion;
        }, WaterOpticsHistoryInvalidation::SurfaceHistory,
            "Surface resets must invalidate optical history.");
        ExpectInvalidation([](WaterOpticsHistoryKey& key) {
            ++key.explicitResetSerial;
        }, WaterOpticsHistoryInvalidation::ExplicitReset,
            "Explicit resets must invalidate optical history.");

        const std::uint64_t versionBeforeReset = history.GetVersion();
        history.Reset();
        Expect(!history.IsValid()
                && history.GetVersion() == versionBeforeReset + 1u,
            "Manual optical reset must invalidate and version history.");

        OceanStatistics statistics{};
        statistics.waterVisibilityMilliseconds = 0.1f;
        statistics.waterRefractionMilliseconds = 0.2f;
        statistics.waterCompositeMilliseconds = 0.3f;
        statistics.waterCausticsMilliseconds = 0.4f;
        statistics.waterVolumetricsMilliseconds = 0.5f;
        Expect(statistics.waterVisibilityMilliseconds
                    + statistics.waterRefractionMilliseconds
                    + statistics.waterCompositeMilliseconds
                    + statistics.waterCausticsMilliseconds
                    + statistics.waterVolumetricsMilliseconds
                > 1.0f,
            "Water optical stage statistics must remain independently writable.");

        WaterApproximateRefractionParameters refractionParameters{};
        refractionParameters.distortionStrength = 0.2f;
        refractionParameters.maximumUvOffset = 0.1f;
        refractionParameters.thicknessOffset = 0.01f;
        refractionParameters.maximumThickness = 10.0f;
        refractionParameters.edgeMargin = 0.01f;
        WaterApproximateRefractionInput refractionInput{};
        refractionInput.uv = {0.5f, 0.5f};
        refractionInput.screenNormal = {1.0f, 0.0f};
        refractionInput.waterDepth = 2.0f;
        refractionInput.opaqueDepth = 7.0f;
        refractionInput.candidateOpaqueDepth = 7.0f;
        const WaterApproximateRefractionResult validRefraction =
            EvaluateApproximateWaterRefraction(
                refractionInput, refractionParameters);
        Expect(validRefraction.IsValidHit()
                && std::isfinite(validRefraction.sampleUv.x)
                && validRefraction.sampleUv.x > refractionInput.uv.x
                && validRefraction.sampleUv.x
                    <= refractionInput.uv.x
                        + refractionParameters.maximumUvOffset + 1.0e-6f
                && std::abs(validRefraction.thickness - 5.0f) < 1.0e-6f,
            "Bounded approximate refraction rejected a valid hit.");

        WaterApproximateRefractionInput offScreen = refractionInput;
        offScreen.uv.x = 0.98f;
        Expect(EvaluateApproximateWaterRefraction(
                   offScreen, refractionParameters).fallback
                == WaterRefractionFallback::OffScreen,
            "Approximate refraction did not reject an off-screen hit.");
        WaterApproximateRefractionInput foreground = refractionInput;
        foreground.candidateOpaqueDepth = 2.005f;
        Expect(EvaluateApproximateWaterRefraction(
                   foreground, refractionParameters).fallback
                == WaterRefractionFallback::Foreground,
            "Approximate refraction did not reject foreground crossing.");
        WaterApproximateRefractionInput zeroThickness = refractionInput;
        zeroThickness.opaqueDepth = 2.005f;
        Expect(EvaluateApproximateWaterRefraction(
                   zeroThickness, refractionParameters).fallback
                == WaterRefractionFallback::ZeroThickness,
            "Approximate refraction did not reject zero thickness.");
        WaterApproximateRefractionInput nonFinite = refractionInput;
        nonFinite.screenNormal.x =
            std::numeric_limits<float>::quiet_NaN();
        const WaterApproximateRefractionResult nonFiniteResult =
            EvaluateApproximateWaterRefraction(
                nonFinite, refractionParameters);
        Expect(nonFiniteResult.fallback
                    == WaterRefractionFallback::NonFinite
                && std::isfinite(nonFiniteResult.sampleUv.x)
                && std::isfinite(nonFiniteResult.sampleUv.y),
            "Approximate refraction did not provide a finite fallback.");

        WaterRayMarchRefractionParameters marchParameters{};
        marchParameters.sampleCount = 0u;
        marchParameters.maximumUvOffset = 2.0f;
        marchParameters.maximumThickness = -4.0f;
        marchParameters.stepScale =
            std::numeric_limits<float>::infinity();
        marchParameters.jitter = -1.0f;
        const WaterRayMarchRefractionParameters normalizedMarch =
            NormalizeWaterRayMarchRefractionParameters(marchParameters);
        Expect(normalizedMarch.sampleCount == 1u
                && normalizedMarch.maximumUvOffset == 0.5f
                && normalizedMarch.maximumThickness >= 0.01f
                && std::isfinite(normalizedMarch.stepScale)
                && normalizedMarch.jitter == 0.0f,
            "Ray-march parameter bounds were not normalized.");

        const float noise = WaterRefractionFrameNoise(17u, 29u, 3u);
        Expect(noise == WaterRefractionFrameNoise(17u, 29u, 3u)
                && noise >= 0.0f && noise < 1.0f
                && noise != WaterRefractionFrameNoise(17u, 29u, 4u),
            "Ray-march frame jitter is not deterministic and bounded.");

        WaterRayMarchRefractionInput marchInput{};
        marchInput.uv = {0.5f, 0.5f};
        marchInput.screenDirection = {1.0f, 0.0f};
        marchInput.waterDepth = 2.0f;
        marchParameters = {};
        marchParameters.sampleCount = 8u;
        marchParameters.maximumUvOffset = 0.1f;
        marchParameters.thicknessOffset = 0.01f;
        marchParameters.maximumThickness = 10.0f;
        marchParameters.stepScale = 1.6f;
        marchParameters.jitter = 0.25f;
        marchParameters.edgeMargin = 0.01f;
        const std::array<float, 8> disjointDepths{
            20.0f, 4.0f, 20.0f, 20.0f,
            20.0f, 20.0f, 20.0f, 20.0f};
        const WaterApproximateRefractionResult marchedHit =
            EvaluateRayMarchedWaterRefraction(
                marchInput, marchParameters, disjointDepths);
        Expect(marchedHit.IsValidHit()
                && marchedHit.sampleUv.x > marchInput.uv.x
                && marchedHit.sampleUv.x < 0.55f
                && std::abs(marchedHit.thickness - 2.0f) < 1.0e-5f,
            "Ray marching missed the bounded disjoint-object hit.");

        const std::array<float, 8> selfIntersectionDepths{
            2.001f, 2.001f, 2.001f, 2.001f,
            2.001f, 2.001f, 2.001f, 2.001f};
        Expect(EvaluateRayMarchedWaterRefraction(
                   marchInput, marchParameters,
                   selfIntersectionDepths).fallback
                == WaterRefractionFallback::Foreground,
            "Ray marching accepted a water self-intersection.");
        const std::array<float, 8> missedDepths{
            20.0f, 20.0f, 20.0f, 20.0f,
            20.0f, 20.0f, 20.0f, 20.0f};
        Expect(EvaluateRayMarchedWaterRefraction(
                   marchInput, marchParameters, missedDepths).fallback
                == WaterRefractionFallback::NoIntersection,
            "Ray marching did not report a finite bounded miss.");
        std::array<float, 8> invalidDepths = missedDepths;
        invalidDepths[0] = std::numeric_limits<float>::quiet_NaN();
        Expect(EvaluateRayMarchedWaterRefraction(
                   marchInput, marchParameters, invalidDepths).fallback
                == WaterRefractionFallback::NonFinite,
            "Ray marching accepted a non-finite depth sample.");

        WaterOpticalParameters opticalParameters{};
        opticalParameters.absorption = {};
        opticalParameters.scattering = {};
        WaterOpticalInput opticalInput{};
        opticalInput.thickness = 20.0f;
        const WaterOpticalResult zeroCoefficients =
            EvaluateWaterOpticalResponse(opticalInput, opticalParameters);
        Expect(zeroCoefficients.transmittance.x == 1.0f
                && zeroCoefficients.transmittance.y == 1.0f
                && zeroCoefficients.transmittance.z == 1.0f
                && zeroCoefficients.singleScattering.x == 0.0f,
            "Zero optical coefficients must preserve transmission.");

        opticalParameters = {};
        opticalInput.thickness = 1.0f;
        const WaterOpticalResult thinResponse =
            EvaluateWaterOpticalResponse(opticalInput, opticalParameters);
        opticalInput.thickness = 20.0f;
        const WaterOpticalResult thickResponse =
            EvaluateWaterOpticalResponse(opticalInput, opticalParameters);
        Expect(thickResponse.transmittance.x < thinResponse.transmittance.x
                && thickResponse.transmittance.y < thinResponse.transmittance.y
                && thickResponse.transmittance.z < thinResponse.transmittance.z
                && thickResponse.singleScattering.x
                    >= thinResponse.singleScattering.x,
            "Beer-Lambert response must be monotonic with thickness.");

        opticalInput.thickness = 4.0f;
        opticalInput.viewCosine = 1.0f;
        const WaterOpticalResult normalView =
            EvaluateWaterOpticalResponse(opticalInput, opticalParameters);
        opticalInput.viewCosine = 0.0f;
        opticalInput.lightCosine = -1.0f;
        const WaterOpticalResult grazingBacklit =
            EvaluateWaterOpticalResponse(opticalInput, opticalParameters);
        Expect(grazingBacklit.fresnel > normalView.fresnel
                && grazingBacklit.backlit.x > 0.0f,
            "Grazing Fresnel and backlit transmission are not responsive.");

        opticalParameters.absorption.x =
            std::numeric_limits<float>::quiet_NaN();
        opticalParameters.scattering.y =
            std::numeric_limits<float>::infinity();
        opticalParameters.phaseG =
            std::numeric_limits<float>::quiet_NaN();
        opticalInput.thickness =
            std::numeric_limits<float>::infinity();
        const WaterOpticalResult extremeResponse =
            EvaluateWaterOpticalResponse(opticalInput, opticalParameters);
        Expect(std::isfinite(extremeResponse.fresnel)
                && std::isfinite(extremeResponse.phase)
                && std::isfinite(extremeResponse.transmittance.x)
                && std::isfinite(extremeResponse.singleScattering.y)
                && std::isfinite(extremeResponse.thinLayer.z)
                && std::isfinite(extremeResponse.backlit.x),
            "Extreme optical inputs produced a non-finite response.");

        const WaterFoamResult calmFoam = EvaluateWaterFoamResponse({
            0.0f, 1.0f, 0.1f});
        Expect(calmFoam.effectiveFoam == 0.0f
                && calmFoam.diffuseWeight == 0.0f
                && calmFoam.refractionWeight == 1.0f
                && std::abs(calmFoam.roughness - 0.1f) < 1.0e-6f,
            "World detail must not create foam on calm water.");
        const WaterFoamResult defaultFoam = EvaluateWaterFoamResponse({
            0.2f, 0.5f, 0.1f});
        const WaterFoamResult wakeFoam = EvaluateWaterFoamResponse({
            0.65f, 0.8f, 0.1f});
        const WaterFoamResult strongWindFoam = EvaluateWaterFoamResponse({
            0.95f, 0.35f, 0.1f});
        Expect(defaultFoam.effectiveFoam > 0.0f
                && wakeFoam.effectiveFoam > defaultFoam.effectiveFoam
                && strongWindFoam.effectiveFoam > wakeFoam.effectiveFoam
                && defaultFoam.roughness > calmFoam.roughness
                && wakeFoam.refractionWeight
                    < defaultFoam.refractionWeight
                && strongWindFoam.diffuseWeight
                    > wakeFoam.diffuseWeight,
            "Simulated foam must monotonically raise aeration/roughness and suppress refraction across default, wake, and strong-wind fixtures.");
        const WaterFoamResult invalidFoam = EvaluateWaterFoamResponse({
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::quiet_NaN()});
        Expect(std::isfinite(invalidFoam.roughness)
                && invalidFoam.effectiveFoam == 0.0f
                && invalidFoam.refractionWeight == 1.0f,
            "Non-finite foam inputs must retain the calm finite fallback.");

        // Perspective device-depth differences must never be treated as metres.
        const auto projection = DirectX::XMMatrixPerspectiveFovLH(1.0f, 1.6f, 0.1f, 2000.0f);
        DirectX::XMFLOAT4X4 inverseProjection;
        DirectX::XMStoreFloat4x4(&inverseProjection, DirectX::XMMatrixInverse(nullptr, projection));
        const auto deviceDepth = [&](float metres) {
            const auto clip = DirectX::XMVector4Transform(DirectX::XMVectorSet(0, 0, metres, 1), projection);
            return DirectX::XMVectorGetZ(clip) / DirectX::XMVectorGetW(clip);
        };
        for (const float waterDistance : {5.0f, 50.0f, 500.0f})
        {
            const float water = ReconstructWaterLinearDepthMeters({0.5f, 0.5f}, deviceDepth(waterDistance),
                inverseProjection, {}, {0, 0, 1});
            const float receiver = ReconstructWaterLinearDepthMeters({0.5f, 0.5f}, deviceDepth(waterDistance + 8.0f),
                inverseProjection, {}, {0, 0, 1});
            Expect(std::abs(receiver - water - 8.0f) < 0.4f,
                "Optical depth must remain metric when perspective depth approaches one.");
        }
        std::cout << "Water optics settings tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
