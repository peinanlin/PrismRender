#include "UI/OceanLabPanel.h"
#include "UI/WaterOpticsPanel.h"

#include "Renderer/Features/Ocean/OceanSettings.h"
#include "Renderer/Features/Ocean/OceanSpectrumMath.h"
#include "Renderer/RenderSettings.h"
#include "Renderer/RendererStatistics.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace Prism::UI
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;

struct OceanPerformanceBudget
{
    float gpuMilliseconds;
    float memoryMegabytes;
};

constexpr OceanPerformanceBudget GetOceanPerformanceBudget(
    const Renderer::OceanSimulationQuality quality)
{
    switch (quality)
    {
    case Renderer::OceanSimulationQuality::Normal:
        return {1.0f, 10.0f};
    case Renderer::OceanSimulationQuality::High:
        return {2.0f, 40.0f};
    case Renderer::OceanSimulationQuality::Extreme:
        return {5.0f, 160.0f};
    }
    return {5.0f, 160.0f};
}

float DirectionDegrees(const DirectX::XMFLOAT2 direction)
{
    float degrees = std::atan2(direction.y, direction.x) * 180.0f / Pi;
    if (degrees < 0.0f)
    {
        degrees += 360.0f;
    }
    return degrees;
}

void SetDirectionDegrees(
    DirectX::XMFLOAT2& direction,
    float degrees)
{
    degrees = std::fmod(degrees, 360.0f);
    if (degrees < 0.0f)
    {
        degrees += 360.0f;
    }
    const float radians = degrees * Pi / 180.0f;
    direction = {std::cos(radians), std::sin(radians)};
}

void DrawFoamControls(
    const char* id,
    Renderer::OceanFoamSettings& foam,
    const bool localWaves)
{
    ImGui::PushID(id);
    ImGui::SliderFloat(
        "Whitecaps Threshold", &foam.whitecapsThreshold, 0.0f, 1.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Immediate wave-hat threshold. Independent from history generation.");
    ImGui::SliderFloat(
        "Generation Threshold", &foam.generationThreshold, 0.0f, 1.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Folding threshold that injects persistent turbulent foam energy.");
    ImGui::SliderFloat(
        "Generation Amount", &foam.generationAmount, 0.0f, 1.0f);
    ImGui::SliderFloat(
        "Dissipation Speed", &foam.dissipationSpeed, 0.0f, 1.0f);
    ImGui::SliderFloat(
        "Falloff Speed", &foam.falloffSpeed,
        localWaves ? 0.5f : 0.95f, 0.99f, "%.3f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(localWaves
            ? "One-second wake-foam retention (higher leaves a longer trail)."
            : "Per-reference-step wind-foam retention (higher persists longer).");
    ImGui::PopID();
}

void SynchronizeSpectralCompatibilityFields(
    Renderer::RenderSettings& settings)
{
    settings.fftOceanEnabled = settings.ocean.debug.renderWater;
    settings.oceanCameraFollowEnabled =
        settings.ocean.cameraFollowEnabled;
    settings.oceanPatchLength =
        settings.ocean.simulationPeriodMeters;
    settings.oceanWindDirection = settings.ocean.baseWind.direction;
    settings.oceanWindSpeed = settings.ocean.baseWind.speed;
    settings.oceanSpectrumAmplitude =
        settings.ocean.baseWind.amplitudeMultiplier;
    settings.oceanChoppiness = settings.ocean.lateralMultiplier;
    settings.oceanDeepWaterColor =
        settings.ocean.shading.deepWaterColor;
    settings.oceanScatteringColor =
        settings.ocean.shading.scatteringColor;
    settings.oceanFoamColor = settings.ocean.shading.foamColor;
}

void DrawLegacyLab(Renderer::RenderSettings& settings)
{
    if (!ImGui::CollapsingHeader(
            "FFT Ocean Lab",
            ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }
    ImGui::TextDisabled(
        "Original PrismRender Phillips-spectrum comparison scene.");
    ImGui::Checkbox("Simulate / Render Water", &settings.fftOceanEnabled);
    ImGui::Checkbox(
        "Camera-following Clipmap",
        &settings.oceanCameraFollowEnabled);
    ImGui::SliderFloat(
        "Simulation Period",
        &settings.oceanPatchLength,
        64.0f,
        1024.0f,
        "%.0f m");
    ImGui::SliderFloat(
        "Wind Speed",
        &settings.oceanWindSpeed,
        1.0f,
        40.0f,
        "%.1f m/s");
    ImGui::SliderFloat(
        "Spectrum Amplitude",
        &settings.oceanSpectrumAmplitude,
        0.05f,
        50.0f,
        "%.2f",
        ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat(
        "Choppiness",
        &settings.oceanChoppiness,
        0.0f,
        2.5f);
    ImGui::ColorEdit3(
        "Deep Water Color", &settings.oceanDeepWaterColor.x);
    ImGui::ColorEdit3(
        "Scattering Color", &settings.oceanScatteringColor.x);
    ImGui::ColorEdit3(
        "Foam Color", &settings.oceanFoamColor.x);
    settings.SyncLegacyOceanFields();
}

void DrawWaveWorksLab(Renderer::RenderSettings& settings,
    const Renderer::RendererStatistics* rendererStatistics)
{
    if (!ImGui::CollapsingHeader(
            settings.ocean.opticsModel == Renderer::OceanOpticsModel::HpWater
                ? "Shared FFT / local waves" : "WaveWorks Ocean Lab",
            ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    Renderer::OceanSettings& ocean = settings.ocean;
    ImGui::TextDisabled(
        "Clean-room PrismRender implementation; no NVIDIA runtime dependency.");
    if (ImGui::Button("Load WaveWorks Reference"))
    {
        const auto optics = ocean.optics;
        const auto opticsModel = ocean.opticsModel;
        ocean = Renderer::OceanSettings::WaveWorksReference();
        ocean.optics = optics;
        ocean.opticsModel = opticsModel;
    }

    if (ImGui::TreeNodeEx("General settings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextUnformatted("Implementation: Native spectral (JONSWAP)");
        const char* qualities[] = {
            "Normal (128)", "High (256)", "Extreme (512)"};
        int quality = static_cast<int>(ocean.quality);
        if (ImGui::Combo(
                "Quality",
                &quality,
                qualities,
                IM_ARRAYSIZE(qualities)))
        {
            ocean.quality =
                static_cast<Renderer::OceanSimulationQuality>(quality);
        }
        const char* simulationApis[] = {
            "CPU reference", "Portable compute (Slang)"};
        int simulationApi = static_cast<int>(ocean.simulationApi);
        if (ImGui::Combo("Simulation API", &simulationApi,
                simulationApis, IM_ARRAYSIZE(simulationApis)))
        {
            ocean.simulationApi =
                static_cast<Renderer::OceanSimulationApi>(simulationApi);
        }
        ImGui::Checkbox("Async Compute", &ocean.asyncComputeEnabled);
        ImGui::Checkbox("Simulate Water", &ocean.debug.simulateWater);
        ImGui::Checkbox("Render Water", &ocean.debug.renderWater);
        ImGui::TextDisabled(
            "Simulation pause keeps the last coherent maps visible; render pause removes the ocean pass.");
        ImGui::SeparatorText("Reset controls");
        if (ImGui::Button("Reset Simulation History"))
        {
            ++ocean.debug.historyResetSerial;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Local Waves"))
        {
            ++ocean.debug.localResetSerial;
        }
        if (ImGui::Button("Full Ocean Reset"))
        {
            ++ocean.debug.fullResetSerial;
        }
        ImGui::Checkbox(
            "Camera-following Surface", &ocean.cameraFollowEnabled);
        ImGui::Checkbox("Use Beaufort Scale", &ocean.useBeaufortScale);
        ImGui::SliderFloat(
            "Simulation Period",
            &ocean.simulationPeriodMeters,
            100.0f,
            10000.0f,
            "%.0f m",
            ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat(
            "Time Scale", &ocean.timeScale, 0.0f, 4.0f);
        ImGui::SliderFloat(
            "Lateral Multiplier", &ocean.lateralMultiplier, 0.0f, 4.0f);
        ImGui::SliderFloat(
            "UV Warping Amplitude",
            &ocean.shading.uvWarpingAmplitude,
            0.0f,
            0.2f);
        ImGui::SliderFloat(
            "UV Warping Frequency",
            &ocean.shading.uvWarpingFrequency,
            0.0f,
            8.0f);
        ImGui::SeparatorText("Surface / Lighting");
        ImGui::SliderFloat(
            "Sun Angle",
            &ocean.shading.sunAngleDegrees,
            0.0f,
            90.0f,
            "%.1f deg");
        ImGui::SliderFloat(
            "Sun Intensity", &ocean.shading.sunIntensity, 0.0f, 10.0f);
        ImGui::SliderFloat(
            "Beckmann Roughness",
            &ocean.shading.beckmannRoughness,
            0.000001f,
            0.01f,
            "%.6f",
            ImGuiSliderFlags_Logarithmic);
        ImGui::Checkbox(
            "Microfacet Fresnel", &ocean.shading.useMicrofacetFresnel);
        ImGui::Checkbox(
            "Microfacet Specular", &ocean.shading.useMicrofacetSpecular);
        ImGui::Checkbox(
            "Microfacet Reflection", &ocean.shading.useMicrofacetReflection);
        ImGui::ColorEdit3(
            "Deep Water Color", &ocean.shading.deepWaterColor.x);
        ImGui::ColorEdit3(
            "Scattering Color", &ocean.shading.scatteringColor.x);
        ImGui::InputFloat4(
            "Water Color Intensity", &ocean.shading.waterColorIntensity.x);
        ImGui::ColorEdit3(
            "Foam Color", &ocean.shading.foamColor.x);
        ImGui::ColorEdit3(
            "Underwater Foam Color",
            &ocean.shading.underwaterFoamColor.x);
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Wind waves", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SeparatorText("Base Wind");
        float direction = DirectionDegrees(ocean.baseWind.direction);
        if (ImGui::SliderFloat(
                "Direction##Base", &direction, 0.0f, 360.0f, "%.1f deg"))
        {
            SetDirectionDegrees(ocean.baseWind.direction, direction);
        }
        if (ocean.useBeaufortScale)
        {
            ImGui::SliderFloat("Speed##Base", &ocean.baseWind.speed,
                0.0f, 12.0f, "%.1f Beaufort");
            ImGui::TextDisabled("Effective wind: %.2f m/s",
                Renderer::BeaufortToMetersPerSecond(ocean.baseWind.speed));
        }
        else
        {
            ImGui::SliderFloat("Speed##Base", &ocean.baseWind.speed,
                0.0f, 50.0f, "%.2f m/s");
        }
        ImGui::SliderFloat(
            "Fetch##Base",
            &ocean.baseWind.fetchKilometers,
            0.001f,
            1000.0f,
            "%.3f km",
            ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat(
            "Dependency##Base", &ocean.baseWind.dependency, 0.0f, 2.0f);
        ImGui::SliderFloat(
            "Spectrum Peaking##Base",
            &ocean.baseWind.spectrumPeaking,
            1.0f,
            20.0f);
        ImGui::SliderFloat(
            "Cutoff Length##Base",
            &ocean.baseWind.smallWavesCutoffLength,
            0.0f,
            100.0f,
            "%.2f m");
        ImGui::SliderFloat(
            "Cutoff Power##Base",
            &ocean.baseWind.smallWavesCutoffPower,
            0.0f,
            4.0f);
        ImGui::SliderFloat(
            "Amplitude##Base",
            &ocean.baseWind.amplitudeMultiplier,
            0.0f,
            4.0f);

        ImGui::SeparatorText("Swell");
        direction = DirectionDegrees(ocean.swell.direction);
        if (ImGui::SliderFloat(
                "Direction##Swell", &direction, 0.0f, 360.0f, "%.1f deg"))
        {
            SetDirectionDegrees(ocean.swell.direction, direction);
        }
        ImGui::SliderFloat(
            "Speed##Swell", &ocean.swell.speed, 0.0f, 40.0f, "%.2f m/s");
        ImGui::SliderFloat(
            "Fetch##Swell",
            &ocean.swell.fetchKilometers,
            0.001f,
            1000.0f,
            "%.3f km",
            ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat(
            "Dependency##Swell", &ocean.swell.dependency, 0.0f, 2.0f);
        ImGui::SliderFloat(
            "Spectrum Peaking##Swell",
            &ocean.swell.spectrumPeaking,
            1.0f,
            20.0f);
        ImGui::SliderFloat(
            "Cutoff Length##Swell",
            &ocean.swell.smallWavesCutoffLength,
            0.0f,
            200.0f,
            "%.2f m");
        ImGui::SliderFloat(
            "Cutoff Power##Swell",
            &ocean.swell.smallWavesCutoffPower,
            0.0f,
            4.0f);
        ImGui::SliderFloat(
            "Amplitude##Swell",
            &ocean.swell.amplitudeMultiplier,
            0.0f,
            4.0f);
        ImGui::SeparatorText("Spectral Foam");
        DrawFoamControls("Spectral", ocean.foam, false);
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Local waves"))
    {
        ImGui::Checkbox("Enable Local Waves", &ocean.local.enabled);
        ImGui::Checkbox(
            "Share Spectral Simulation Time",
            &ocean.local.shareSpectralTime);
        ImGui::Checkbox(
            "Demo Rain / Wake Emitters",
            &ocean.local.demoEmittersEnabled);
        if (ocean.local.demoEmittersEnabled)
        {
            ImGui::Checkbox("Rain Emitter", &ocean.local.rainEmitterEnabled);
            ImGui::SliderFloat(
                "Rain Rate",
                &ocean.local.rainRatePerSecond,
                0.0f,
                10.0f,
                "%.2f / s");
            ImGui::Checkbox(
                "Moving Wake Emitter",
                &ocean.local.wakeEmitterEnabled);
            ImGui::SliderFloat(
                "Wake Speed",
                &ocean.local.wakeSpeedMetersPerSecond,
                0.0f,
                50.0f,
                "%.1f m/s");
            ImGui::TextDisabled(
                "Invisible hull: foreground/background elliptical loop");
            if (ImGui::Button("Reset Local Emitters"))
            {
                ocean.local.emitterSeed ^= 0x9e3779b9u;
                ++ocean.debug.localResetSerial;
            }
        }
        ImGui::InputFloat2("Domain Center (m)", &ocean.local.domainCenter.x);
        ImGui::SliderFloat(
            "Domain Size",
            &ocean.local.domainSizeMeters,
            25.0f,
            2000.0f,
            "%.0f m",
            ImGuiSliderFlags_Logarithmic);
        constexpr std::array<std::uint32_t, 5> GridSizes{
            128u, 256u, 512u, 1024u, 2048u};
        constexpr const char* GridLabels[] = {
            "128", "256", "512", "1024", "2048"};
        int gridIndex = 2;
        for (int index = 0; index < static_cast<int>(GridSizes.size()); ++index)
        {
            if (GridSizes[index] == ocean.local.gridSize)
            {
                gridIndex = index;
                break;
            }
        }
        if (ImGui::Combo(
                "Grid Size", &gridIndex, GridLabels, IM_ARRAYSIZE(GridLabels)))
        {
            ocean.local.gridSize = GridSizes[gridIndex];
        }
        ImGui::TextDisabled(
            "GPU resources are capability-clamped; inactive history stops dispatching after its decay window.");
        ImGui::SliderFloat(
            "Amplitude##Local",
            &ocean.local.amplitudeMultiplier,
            0.0f,
            4.0f);
        ImGui::SliderFloat(
            "Lateral Multiplier##Local",
            &ocean.local.lateralMultiplier,
            0.0f,
            4.0f);
        ImGui::SeparatorText("Manual disturbance");
        ImGui::InputFloat2(
            "Position##Manual", &ocean.local.manualPosition.x);
        ImGui::SliderFloat("Radius##Manual",
            &ocean.local.manualRadiusMeters, 0.1f, 25.0f, "%.2f m");
        ImGui::SliderFloat("Strength##Manual",
            &ocean.local.manualStrength, -2.0f, 2.0f);
        float manualDirection = DirectionDegrees(
            ocean.local.manualVelocityDirection);
        if (ImGui::SliderFloat("Direction##Manual", &manualDirection,
                0.0f, 360.0f, "%.1f deg"))
        {
            SetDirectionDegrees(
                ocean.local.manualVelocityDirection, manualDirection);
        }
        if (ImGui::Button("Add Disturbance"))
        {
            ++ocean.local.manualDisturbanceSerial;
        }
        DrawFoamControls("Local", ocean.local.foam, true);
        ImGui::TextDisabled(
            "Deterministic disturbances, GPU propagation, persistent foam and surface composition are active.");
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Quadtree / geometry parameters"))
    {
        int cellsPerPatch = static_cast<int>(ocean.geometry.cellsPerPatch);
        if (ImGui::SliderInt("Cells per Patch", &cellsPerPatch, 4, 256))
        {
            ocean.geometry.cellsPerPatch =
                static_cast<std::uint32_t>(cellsPerPatch);
        }
        ImGui::SliderFloat(
            "Minimum Patch Length",
            &ocean.geometry.minimumPatchLength,
            1.0f,
            100.0f,
            "%.1f m");
        ImGui::SliderFloat(
            "Maximum Edge Length",
            &ocean.geometry.maximumEdgeLengthPixels,
            1.0f,
            100.0f,
            "%.1f px");
        ImGui::SliderFloat(
            "Mean Sea Level",
            &ocean.geometry.meanSeaLevel,
            -100.0f,
            100.0f,
            "%.1f m");
        int maximumLod = static_cast<int>(ocean.geometry.maximumLod);
        if (ImGui::SliderInt("Maximum LOD", &maximumLod, 0, 31))
        {
            ocean.geometry.maximumLod =
                static_cast<std::uint32_t>(maximumLod);
        }
        ImGui::SliderFloat(
            "Geomorphing Degree",
            &ocean.geometry.geomorphingDegree,
            0.0f,
            1.0f);
        ImGui::Checkbox(
            "Diamond Pattern", &ocean.geometry.generateDiamondPattern);
        ImGui::Checkbox(
            "Prefer Tessellation", &ocean.geometry.preferTessellation);
        ImGui::TextDisabled(
            "Geometry is capability gated: adaptive quadtree/tessellation or clipmap fallback.");
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Statistics"))
    {
        const std::uint32_t resolution =
            Renderer::OceanSettings::ResolutionForQuality(ocean.quality);
        const auto patchLengths =
            Renderer::OceanSettings::ReferenceCascadePatchLengths();
        ImGui::Text("Requested cascade resolution: %u", resolution);
        ImGui::Text(
            "Reference patch lengths: %.3f / %.1f / %.0f / %.0f m",
            patchLengths[0], patchLengths[1], patchLengths[2], patchLengths[3]);
        ImGui::Text("Readback FIFO: %u", ocean.query.readbackFifoEntries);
        ImGui::Checkbox("GPU Queries", &ocean.query.enableGpuQueries);
        ImGui::Checkbox("CPU Readback", &ocean.query.enableCpuReadback);
        ImGui::Checkbox("GPU Timers", &ocean.query.enableGpuTimers);
        ImGui::Checkbox("CPU Timers", &ocean.query.enableCpuTimers);
        int readbackEntries = static_cast<int>(
            ocean.query.readbackFifoEntries);
        if (ImGui::SliderInt("Readback FIFO Entries",
                &readbackEntries, 1, 120))
        {
            ocean.query.readbackFifoEntries =
                static_cast<std::uint32_t>(readbackEntries);
        }
        if (rendererStatistics != nullptr)
        {
            const Renderer::OceanStatistics& statistics =
                rendererStatistics->ocean;
            ImGui::Text("Backend: %s",
                std::string(RHI::ToString(statistics.graphicsApi)).c_str());
            ImGui::Text("Geometry path: %s",
                statistics.tessellationActive
                    ? "adaptive patch PSO (capability gated)"
                    : "camera-following clipmap fallback");
            ImGui::Text("Simulation: %s / render: %s",
                ocean.debug.simulateWater ? "running" : "paused",
                ocean.debug.renderWater ? "enabled" : "disabled");
            ImGui::Text("Active resolution: %u", statistics.cascadeResolution);
            ImGui::Text("Output: %u x %u, rendered views: %u",
                statistics.outputWidth,
                statistics.outputHeight,
                statistics.activeRenderedViews);
            ImGui::Text("Ocean textures: %.2f MiB",
                statistics.allocatedMegabytes);
            ImGui::Text("Dispatches: %u", statistics.dispatchCount);
            ImGui::Text("Spectrum: %.3f ms", statistics.spectrumMilliseconds);
            ImGui::Text("IFFT: %.3f ms", statistics.fftMilliseconds);
            ImGui::Text("Maps: %.3f ms", statistics.mapMilliseconds);
            ImGui::Text("Foam: %.3f ms", statistics.foamMilliseconds);
            ImGui::Text("Mips: %.3f ms", statistics.mipMilliseconds);
            ImGui::Text("Ocean GPU total: %.3f ms",
                statistics.gpuTotalMilliseconds);
            ImGui::Text("Ocean surface / renderer: %.3f / %.3f ms",
                statistics.surfaceMilliseconds,
                statistics.rendererGpuMilliseconds);
            const OceanPerformanceBudget budget =
                GetOceanPerformanceBudget(ocean.quality);
            ImGui::Text("Quality budget: %.1f ms / %.0f MiB",
                budget.gpuMilliseconds, budget.memoryMegabytes);
            const bool gpuBudgetExceeded = statistics.gpuTimersAvailable
                && statistics.gpuTotalMilliseconds > budget.gpuMilliseconds;
            const bool memoryBudgetExceeded =
                statistics.allocatedMegabytes > budget.memoryMegabytes;
            if (gpuBudgetExceeded || memoryBudgetExceeded)
            {
                ImGui::TextColored({1.0f, 0.38f, 0.22f, 1.0f},
                    "Performance warning: %s%s exceeds the selected quality budget.",
                    gpuBudgetExceeded ? "GPU time" : "",
                    gpuBudgetExceeded && memoryBudgetExceeded
                        ? " and memory" : (memoryBudgetExceeded ? "memory" : ""));
            }
            ImGui::Text("CPU total: %.3f ms, render latency: %.1f frames",
                statistics.cpuTotalMilliseconds,
                statistics.renderLatencyFrames);
            ImGui::Text("Query readback latency: %.1f frames",
                statistics.readbackLatencyFrames);
            ImGui::Text("Queries: %u pending, %u completed, %llu expired",
                statistics.queryPending,
                statistics.queryCompleted,
                static_cast<unsigned long long>(statistics.queryExpired));
            ImGui::Text("Query batches/copies: %llu / %llu (%s)",
                static_cast<unsigned long long>(statistics.queryGpuBatches),
                static_cast<unsigned long long>(statistics.queryReadbackCopies),
                statistics.queryResourceAllocated ? "allocated" : "idle");
            ImGui::Text("Geometry nodes: %u visible / %u total",
                statistics.visibleGeometryNodes,
                statistics.geometryNodes);
            ImGui::Text("Geometry instances uploaded: %u",
                statistics.geometryInstanceCount);
            ImGui::Text("Geometry refinement: %u iterations, max LOD %u",
                statistics.geometryRefinementIterations,
                statistics.geometryMaxLod);
            ImGui::Text("Geometry max tessellation factor: %.1f",
                statistics.geometryMaxTessellationFactor);
            ImGui::Text("Geometry CPU selection: %.3f ms",
                statistics.geometryMilliseconds);
            ImGui::Text("Cascade publication: v%llu",
                static_cast<unsigned long long>(statistics.publishedVersion));
            ImGui::Text("Settings requested / active: v%llu / v%llu",
                static_cast<unsigned long long>(
                    statistics.requestedSettingsVersion),
                static_cast<unsigned long long>(
                    statistics.activeSettingsVersion));
            ImGui::Text(
                "Local waves: %u grid / %.2f MiB (resource %llu)",
                statistics.localWaveGridSize,
                statistics.localWaveAllocatedMegabytes,
                    static_cast<unsigned long long>(
                    statistics.localWaveResourceGeneration));
            ImGui::Text("Local wave time: %s, %.3f ms",
                statistics.localWaveSharedTime ? "shared" : "fixed",
                statistics.localWaveMilliseconds);
            ImGui::Text("Local emitter / dispatch: %s / %s",
                statistics.localWaveEmitterActive ? "active" : "idle",
                statistics.localWaveSimulationActive ? "active" : "idle");
            if (statistics.pendingDirtyScopes != 0u)
            {
                ImGui::TextColored(
                    {0.95f, 0.78f, 0.25f, 1.0f},
                    "Pending rebuild scopes: 0x%02X",
                    statistics.pendingDirtyScopes);
            }
            else
            {
                ImGui::TextDisabled("No pending ocean rebuilds.");
            }
            if (!statistics.gpuTimersAvailable)
            {
                ImGui::TextDisabled("GPU ocean timings unavailable.");
            }
            if (!statistics.cpuTimersAvailable)
            {
                ImGui::TextDisabled("CPU ocean timings unavailable.");
            }
        }
        ImGui::TextColored(
            {0.95f, 0.72f, 0.20f, 1.0f},
            "Four-cascade IFFT, persistent foam, local RenderGraph state, and GPU statistics are active.");
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Debug"))
    {
        ImGui::Checkbox("Wireframe", &ocean.debug.wireframe);
        ImGui::Checkbox("Show Cascades", &ocean.debug.showCascades);
        ImGui::Checkbox("Show Foam Energy", &ocean.debug.showFoamEnergy);
        ImGui::Checkbox("Show Slope Moments", &ocean.debug.showSlopeMoments);
        ImGui::Checkbox("Show Geometry LOD", &ocean.debug.showGeometryLod);
        ImGui::Checkbox("Show Normals", &ocean.debug.showNormals);
        ImGui::TreePop();
    }

    ocean.implementation = Renderer::OceanImplementation::SpectralOcean;
    std::string validationWarning;
    ocean.ValidateAndNormalize(&validationWarning);
    if (!validationWarning.empty())
    {
        ImGui::TextColored(
            {1.0f, 0.68f, 0.22f, 1.0f},
            "Validation: %s",
            validationWarning.c_str());
    }
    SynchronizeSpectralCompatibilityFields(settings);
}
} // namespace

void OceanLabPanel::Draw(Renderer::RenderSettings& settings,
    const Renderer::RendererStatistics* statistics) const
{
    if (settings.ocean.implementation
        == Renderer::OceanImplementation::SpectralOcean)
    {
        if (settings.ocean.opticsModel == Renderer::OceanOpticsModel::HpWater)
            DrawWaterOpticsPanel(settings.ocean, statistics != nullptr ? &statistics->ocean : nullptr);
        DrawWaveWorksLab(settings, statistics);
        return;
    }
    DrawLegacyLab(settings);
}
} // namespace Prism::UI
