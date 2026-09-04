#include "UI/WaterOpticsPanel.h"

#include "Renderer/Features/Ocean/OceanSettings.h"
#include "Renderer/Features/Ocean/OceanStatistics.h"

#include <imgui.h>

namespace Prism::UI
{
namespace
{
void SliderCount(const char* label, std::uint32_t& count, int minimum, int maximum)
{
    int value = static_cast<int>(count);
    if (ImGui::SliderInt(label, &value, minimum, maximum))
        count = static_cast<std::uint32_t>(value);
}
}

void DrawWaterOpticsPanel(Renderer::OceanSettings& ocean,
    const Renderer::OceanStatistics* stats)
{
    using namespace Renderer;
    if (!ImGui::CollapsingHeader("HPWater Ocean Optics", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::PushID("HPWaterOptics");
    auto& settings = ocean.optics;
    ImGui::TextWrapped("Shared FFT geometry at every distance; optical quality is independent of simulation quality.");
    int quality = static_cast<int>(settings.quality);
    if (ImGui::Combo("Optical quality", &quality, "Normal\0High\0Extreme\0"))
        settings.quality = static_cast<WaterOpticsQuality>(quality);
    if (ImGui::Button("Reset optics defaults"))
    {
        const auto serial = settings.historyResetSerial + 1u;
        settings = WaterOpticsSettings::HpWaterReference();
        settings.historyResetSerial = serial;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset optical history")) ++settings.historyResetSerial;

    if (ImGui::TreeNode("Material / scattering"))
    {
        ImGui::SliderFloat3("Absorption (1/m)", &settings.material.absorption.x, 0.0f, 1.0f);
        ImGui::SliderFloat3("Scattering (1/m)", &settings.material.scattering.x, 0.0f, 1.0f);
        ImGui::SliderFloat("Roughness", &settings.material.roughness, 0.001f, 1.0f);
        ImGui::SliderFloat("Index of refraction", &settings.material.indexOfRefraction, 1.0f, 2.0f);
        ImGui::SliderFloat("Phase anisotropy", &settings.material.phaseG, -0.95f, 0.95f);
        ImGui::SliderFloat("Thin layer", &settings.material.thinLayerStrength, 0.0f, 2.0f);
        ImGui::SliderFloat("Backlit transmission", &settings.material.backlitStrength, 0.0f, 2.0f);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Refraction"))
    {
        ImGui::Checkbox("Enable refraction", &settings.refraction.enabled);
        ImGui::Checkbox("Depth ray march", &settings.refraction.highPrecision);
        SliderCount("Requested ray samples", settings.refraction.rayMarchSampleCount, 1, 8);
        ImGui::SliderFloat("Distortion", &settings.refraction.distortionStrength, 0.0f, 0.2f);
        ImGui::SliderFloat("Maximum UV offset", &settings.refraction.maximumUvOffset, 0.0f, 0.25f);
        ImGui::SliderFloat("Thickness offset (m)", &settings.refraction.thicknessOffsetMeters, 0.0f, 2.0f);
        ImGui::SliderFloat("Maximum thickness (m)", &settings.refraction.maximumThicknessMeters, 0.1f, 200.0f);
        ImGui::SliderFloat("Ray step scale", &settings.refraction.rayStepScale, 1.0f, 3.0f);
        if (settings.refraction.highPrecision && settings.quality == WaterOpticsQuality::Normal)
            ImGui::TextWrapped("Normal quality uses approximate refraction (0 ray-march samples).");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Caustics"))
    {
        ImGui::Checkbox("Enable caustics", &settings.caustics.enabled);
        int mode = static_cast<int>(settings.caustics.mode);
        if (ImGui::Combo("Requested caustic mode", &mode, "Disabled\0Single channel\0RGB dispersion\0"))
            settings.caustics.mode = static_cast<WaterCausticsMode>(mode);
        SliderCount("Map resolution", settings.caustics.resolution, 64, 2048);
        ImGui::SliderFloat("Near coverage (m)", &settings.caustics.nearCoverageMeters, 16.0f, 1024.0f);
        ImGui::SliderFloat("Middle coverage (m)", &settings.caustics.middleCoverageMeters, 64.0f, 4096.0f);
        ImGui::SliderFloat("Caustic intensity", &settings.caustics.intensity, 0.0f, 8.0f);
        ImGui::SliderFloat("Dispersion", &settings.caustics.dispersion, 0.0f, 0.05f);
        ImGui::SliderFloat("Coverage edge fade", &settings.caustics.edgeFadeFraction, 0.01f, 0.49f);
        const auto effectiveMode = GetEffectiveWaterCausticsMode(settings);
        ImGui::Text("Effective: %s", effectiveMode == WaterCausticsMode::Disabled ? "disabled"
            : effectiveMode == WaterCausticsMode::SingleChannel ? "single channel" : "RGB dispersion");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Underwater volumetrics"))
    {
        ImGui::Checkbox("Enable volumetrics", &settings.volumetrics.enabled);
        ImGui::SliderFloat("Resolution scale", &settings.volumetrics.resolutionScale, 0.25f, 1.0f);
        SliderCount("Integration samples", settings.volumetrics.sampleCount, 1, 128);
        ImGui::SliderFloat("Maximum distance (m)", &settings.volumetrics.maximumDistanceMeters, 1.0f, 500.0f);
        ImGui::SliderFloat("History weight", &settings.volumetrics.historyWeight, 0.0f, 0.99f);
        ImGui::SliderFloat("Depth rejection (m)", &settings.volumetrics.depthRejectionMeters, 0.001f, 4.0f);
        SliderCount("Bilateral radius", settings.volumetrics.atrousIterations, 0, 3);
        ImGui::SliderFloat("Waterline hysteresis (m)", &settings.volumetrics.surfaceHysteresisMeters, 0.0f, 1.0f);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Distance quality"))
    {
        ImGui::SliderFloat("Near end (m)", &settings.distance.nearEndMeters, 10.0f, 500.0f);
        ImGui::SliderFloat("Middle end (m)", &settings.distance.middleEndMeters, 100.0f, 2000.0f);
        ImGui::SliderFloat("Far reference (m)", &settings.distance.farEndMeters, 1000.0f, 20000.0f);
        ImGui::SliderFloat("Tier transition", &settings.distance.transitionFraction, 0.01f, 0.49f);
        ImGui::TextWrapped("The far tier retains the full spectral surface; these ranges only control optical work.");
        ImGui::TreePop();
    }
    int debug = static_cast<int>(settings.debugView);
    if (ImGui::Combo("Optical debug view", &debug,
            "Final\0Water mask\0Water depth\0Normal / roughness\0Absorption\0Scattering\0Foam\0Thickness\0Refraction hit\0Distance tiers\0Caustic energy\0Caustic cascades\0Volume accumulation\0Volume history rejection\0"))
        settings.debugView = static_cast<WaterOpticsDebugView>(debug);
    ImGui::TextDisabled("Debug selection does not reset simulation or optical history.");
    (void)settings.ValidateAndNormalize();
    if (stats != nullptr && ImGui::TreeNodeEx("Effective modes / diagnostics", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("Optics: %s | %ux%u | %u views", stats->waterOpticsActive ? "active" : "inactive",
            stats->outputWidth, stats->outputHeight, stats->activeRenderedViews);
        ImGui::Text("Refraction: %s, %u samples, %ux%u",
            !settings.refraction.enabled ? "disabled" : stats->waterRefractionRayMarchActive ? "ray march" : "approximate",
            stats->waterRefractionEffectiveSamples, stats->waterRefractionWidth, stats->waterRefractionHeight);
        ImGui::Text("Volume: %s, %ux%u", stats->waterVolumetricsActive ? "active" : "disabled / above water",
            stats->waterVolumetricWidth, stats->waterVolumetricHeight);
        ImGui::Text("Medium: %s (%s)", stats->waterCameraUnderwater ? "underwater" : "above water",
            stats->waterMediumFallback ? "mean sea level fallback" : "coherent GPU height");
        ImGui::Text("History: optical %s / volume %s | medium v%llu",
            stats->waterOpticsHistoryValid ? "valid" : "rejected", stats->waterVolumetricHistoryValid ? "valid" : "rejected",
            static_cast<unsigned long long>(stats->waterMediumVersion));
        ImGui::Text("Visibility %.3f | Refraction %.3f | Composite %.3f ms",
            stats->waterVisibilityMilliseconds, stats->waterRefractionMilliseconds, stats->waterCompositeMilliseconds);
        ImGui::Text("Caustics %.3f | Volume %.3f | Reconstruction %.3f ms",
            stats->waterCausticsMilliseconds, stats->waterVolumetricsMilliseconds, stats->waterVolumetricReconstructionMilliseconds);
        ImGui::Text("Water pool %.1f MiB | visibility draws %u | dispatches %u",
            stats->waterAllocatedMegabytes, stats->waterVisibilityDrawCount, stats->waterOpticsDispatchCount);
        if (stats->waterCoverageAvailable)
            ImGui::Text("Measured water coverage %.1f%%", stats->waterPixelCoverage * 100.0f);
        else ImGui::TextDisabled("Water coverage: awaiting GPU readback");
        ImGui::TreePop();
    }
    ImGui::PopID();
}
}
