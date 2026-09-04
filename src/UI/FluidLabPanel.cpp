#include "UI/FluidLabPanel.h"

#include "Renderer/Features/Fluid/FluidSettings.h"
#include "Renderer/Features/Fluid/FluidStatistics.h"

#include <imgui.h>

#include <algorithm>

namespace Prism::UI
{
void FluidLabPanel::Draw(
    Renderer::FluidSettings& settings,
    const Renderer::FluidStatistics& statistics) const
{
    if (!ImGui::CollapsingHeader(
            "Position Based Fluids",
            ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    const bool customPipeline = settings.demoPipeline
        == Renderer::FluidDemoPipeline::Custom;
    const bool particlePipeline = settings.demoPipeline
        == Renderer::FluidDemoPipeline::PbfParticles;
    const bool causticsPipeline = settings.demoPipeline
        == Renderer::FluidDemoPipeline::ScreenSpaceCaustics;
    const bool toonPipeline = settings.demoPipeline
        == Renderer::FluidDemoPipeline::ScreenSpaceToonFoam;
    const char* pipelineLabel = "Custom Fluid Lab";
    switch (settings.demoPipeline)
    {
    case Renderer::FluidDemoPipeline::PbfParticles:
        pipelineLabel = "PBF + Particle Preview";
        break;
    case Renderer::FluidDemoPipeline::ScreenSpaceRealistic:
        pipelineLabel = "PBF + Realistic Screen-Space Fluid";
        break;
    case Renderer::FluidDemoPipeline::ScreenSpaceCaustics:
        pipelineLabel = "Realistic Fluid + Caustics";
        break;
    case Renderer::FluidDemoPipeline::ScreenSpaceToonFoam:
        pipelineLabel = "Toon Fluid + Density Foam";
        break;
    case Renderer::FluidDemoPipeline::Custom:
        break;
    }
    ImGui::Text("Demo Pipeline: %s", pipelineLabel);

    ImGui::Checkbox("Enable Fluid", &settings.enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Pause", &settings.paused);
    ImGui::SameLine();
    if (ImGui::Button("Reset Fluid"))
    {
        settings.resetRequested = true;
    }
    ImGui::TextDisabled(
        "Impulse keys: J/L = -/+X, K/I = -/+Y, U/O = -/+Z");
    ImGui::Checkbox(
        "Scene View Fluid Preview (expensive)",
        &settings.editorPreviewEnabled);
    ImGui::TextDisabled(
        "Game/Final Output remains enabled; Scene View otherwise skips its second simulation.");

    if (ImGui::TreeNodeEx(
            "Simulation",
            ImGuiTreeNodeFlags_DefaultOpen))
    {
        constexpr const char* SpawnLayouts[] = {
            "Single Block",
            "Double Dam Break"};
        int spawnLayout = static_cast<int>(settings.spawnLayout);
        if (ImGui::Combo(
                "Initial Layout",
                &spawnLayout,
                SpawnLayouts,
                IM_ARRAYSIZE(SpawnLayouts)))
        {
            settings.spawnLayout =
                static_cast<Renderer::FluidSpawnLayout>(spawnLayout);
            settings.resetRequested = true;
        }
        int iterations = static_cast<int>(
            settings.solverIterations);
        if (ImGui::SliderInt(
                "Constraint Iterations",
                &iterations,
                1,
                10))
        {
            settings.solverIterations =
                static_cast<unsigned int>(iterations);
        }
        int substeps = static_cast<int>(settings.substepCount);
        if (ImGui::SliderInt("Substeps", &substeps, 1, 4))
        {
            settings.substepCount =
                static_cast<unsigned int>(substeps);
        }
        constexpr unsigned int NeighborCapacities[] = {
            32u,
            64u,
            96u,
            128u,
            192u,
            256u};
        constexpr const char* NeighborCapacityLabels[] = {
            "32",
            "64",
            "96",
            "128",
            "192",
            "256"};
        int neighborCapacityIndex = 0;
        for (int index = 0;
             index < IM_ARRAYSIZE(NeighborCapacities);
             ++index)
        {
            if (settings.maxNeighborsPerParticle
                == NeighborCapacities[index])
            {
                neighborCapacityIndex = index;
                break;
            }
        }
        if (ImGui::Combo(
                "Neighbor Capacity",
                &neighborCapacityIndex,
                NeighborCapacityLabels,
                IM_ARRAYSIZE(NeighborCapacityLabels)))
        {
            settings.maxNeighborsPerParticle =
                NeighborCapacities[neighborCapacityIndex];
        }
        ImGui::TextDisabled(
            "Higher capacity reduces neighbor clipping but increases GPU memory use.");
        ImGui::SliderFloat(
            "Time Scale",
            &settings.timeScale,
            0.0f,
            2.0f);
        ImGui::SliderFloat(
            "Smoothing Radius",
            &settings.smoothingRadius,
            0.08f,
            0.35f,
            "%.3f");
        ImGui::SliderFloat(
            "Particle Radius",
            &settings.particleRadius,
            0.015f,
            0.10f,
            "%.3f");
        ImGui::SliderFloat(
            "Viscosity",
            &settings.viscosity,
            0.0f,
            0.20f,
            "%.3f");
        ImGui::SliderFloat(
            "Vorticity",
            &settings.vorticity,
            0.0f,
            1.0f,
            "%.3f");
        ImGui::SliderFloat(
            "Gravity Y",
            &settings.gravity.y,
            -25.0f,
            5.0f,
            "%.2f m/s^2");
        ImGui::TreePop();
    }

    if (particlePipeline
        && ImGui::TreeNodeEx(
            "Particle Preview",
            ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled(
            "Thickness, filtering, normals, foam, and caustics passes are not scheduled.");
        ImGui::SliderFloat(
            "Particle Preview Scale",
            &settings.renderParticleRadiusScale,
            0.5f,
            1.5f,
            "%.2f");
        ImGui::SliderFloat(
            "Minimum Particle Density",
            &settings.minimumRenderDensityRatio,
            0.0f,
            0.8f,
            "%.2f rho0");
        ImGui::TreePop();
    }

    if (!particlePipeline && ImGui::TreeNodeEx(
            "Surface Reconstruction",
            ImGuiTreeNodeFlags_DefaultOpen))
    {
        constexpr const char* Modes[] = {
            "Realistic",
            "Toon",
            "Particle Mask",
            "Depth",
            "Thickness",
            "Normals",
            "Foam"};
        if (customPipeline)
        {
            int mode = static_cast<int>(settings.renderMode);
            if (ImGui::Combo(
                    "Display Mode",
                    &mode,
                    Modes,
                    IM_ARRAYSIZE(Modes)))
            {
                settings.renderMode =
                    static_cast<Renderer::FluidRenderMode>(mode);
            }
        }
        else
        {
            ImGui::TextDisabled(
                toonPipeline
                    ? "Display Mode: Toon Surface"
                    : "Display Mode: Realistic Surface");
        }
        ImGui::SliderFloat(
            "Surface Particle Scale",
            &settings.renderParticleRadiusScale,
            0.75f,
            2.5f,
            "%.2f");
        ImGui::SliderFloat(
            "Minimum Surface Density",
            &settings.minimumRenderDensityRatio,
            0.0f,
            0.8f,
            "%.2f rho0");
        int splashNeighborCount = static_cast<int>(
            settings.minimumSplashNeighborCount);
        if (ImGui::SliderInt(
                "Splash Neighbor Minimum",
                &splashNeighborCount,
                1,
                32))
        {
            settings.minimumSplashNeighborCount =
                static_cast<unsigned int>(splashNeighborCount);
        }
        ImGui::SliderFloat(
            "Splash Velocity Threshold",
            &settings.splashVelocityThreshold,
            0.0f,
            10.0f,
            "%.2f m/s");
        ImGui::SliderFloat(
            "Splash Density Ceiling",
            &settings.splashDensityRatioThreshold,
            0.0f,
            1.0f,
            "%.2f rho0");
        ImGui::TextDisabled(
            "Only detached fast particles below both support thresholds are hidden.");
        ImGui::SliderFloat(
            "Surface Fringe Cutoff",
            &settings.surfaceCoverageThreshold,
            0.0f,
            2.0f,
            "%.2f");
        ImGui::TextDisabled(
            "Optional; high values can remove thin sheets and wave crests.");
        int bilateralIterations = static_cast<int>(
            settings.bilateralIterations);
        if (ImGui::SliderInt(
                "Bilateral Iterations",
                &bilateralIterations,
                1,
                8))
        {
            settings.bilateralIterations =
                static_cast<unsigned int>(
                    bilateralIterations);
        }
        int bilateralRadius = static_cast<int>(
            settings.bilateralRadius);
        if (ImGui::SliderInt(
                "Bilateral Radius",
                &bilateralRadius,
                1,
                15))
        {
            settings.bilateralRadius =
                static_cast<unsigned int>(bilateralRadius);
        }
        ImGui::SliderFloat(
            "Depth Sigma",
            &settings.bilateralDepthSigma,
            0.005f,
            2.0f,
            "%.3f");
        ImGui::SliderFloat(
            "Spatial Sigma",
            &settings.bilateralSpatialSigma,
            0.5f,
            8.0f,
            "%.2f");
        int normalSmoothingRadius = static_cast<int>(
            settings.normalSmoothingRadius);
        if (ImGui::SliderInt(
                "Normal Smoothing Radius",
                &normalSmoothingRadius,
                1,
                8))
        {
            settings.normalSmoothingRadius =
                static_cast<unsigned int>(
                    normalSmoothingRadius);
        }
        int silhouetteSmoothingRadius = static_cast<int>(
            settings.silhouetteSmoothingRadius);
        if (ImGui::SliderInt(
                "Silhouette Smoothing Radius",
                &silhouetteSmoothingRadius,
                0,
                8))
        {
            settings.silhouetteSmoothingRadius =
                static_cast<unsigned int>(
                    silhouetteSmoothingRadius);
        }
        ImGui::TreePop();
    }

    if (!particlePipeline && ImGui::TreeNodeEx(
            "Water Shading",
            ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::ColorEdit3(
            "Water Color",
            &settings.waterColor.x);
        ImGui::ColorEdit3(
            "Absorption",
            &settings.absorption.x,
            ImGuiColorEditFlags_HDR
                | ImGuiColorEditFlags_Float);
        ImGui::ColorEdit3(
            "Scattering",
            &settings.scattering.x,
            ImGuiColorEditFlags_HDR
                | ImGuiColorEditFlags_Float);
        ImGui::SliderFloat(
            "Index of Refraction",
            &settings.ior,
            1.0f,
            1.6f,
            "%.3f");
        ImGui::SliderFloat(
            "Refraction Scale",
            &settings.refractionScale,
            0.0f,
            0.08f,
            "%.3f");
        ImGui::SliderFloat(
            "Reflection Strength",
            &settings.reflectionStrength,
            0.0f,
            1.5f);
        ImGui::SliderFloat(
            "Thickness Scale",
            &settings.thicknessScale,
            0.1f,
            6.0f);
        ImGui::TreePop();
    }

    if ((customPipeline || toonPipeline)
        && ImGui::TreeNode("Toon and Foam"))
    {
        if (customPipeline)
        {
            ImGui::Checkbox("Toon Shading", &settings.toonEnabled);
        }
        else
        {
            ImGui::TextDisabled(
                "Toon shading and density foam are fixed on for this demo.");
        }
        int toonBands = static_cast<int>(settings.toonBands);
        if (ImGui::SliderInt("Toon Bands", &toonBands, 2, 8))
        {
            settings.toonBands =
                static_cast<unsigned int>(toonBands);
        }
        ImGui::SliderFloat(
            "Toon Edge Width",
            &settings.toonEdgeWidth,
            0.25f,
            4.0f);
        if (customPipeline)
        {
            ImGui::Checkbox("Density Foam", &settings.foamEnabled);
        }
        ImGui::SliderFloat(
            "Foam Density Threshold",
            &settings.foamDensityThreshold,
            0.2f,
            0.9f,
            "%.2f rho0");
        int erosion = static_cast<int>(
            settings.foamErosionIterations);
        if (ImGui::SliderInt(
                "Foam Cleanup Strength",
                &erosion,
                0,
                4))
        {
            settings.foamErosionIterations =
                static_cast<unsigned int>(erosion);
        }
        ImGui::TreePop();
    }

    if ((customPipeline || causticsPipeline)
        && ImGui::TreeNode("Caustics"))
    {
        if (customPipeline)
        {
            ImGui::Checkbox(
                "Enable Caustics",
                &settings.causticsEnabled);
        }
        else
        {
            ImGui::TextDisabled(
                "Caustics are fixed on for this demo; toon and foam stay off.");
        }
        ImGui::TextWrapped(
            "Receiver-space image-space photon gather (not a light-space photon splat).");
        ImGui::Checkbox(
            "Show Caustics Only",
            &settings.causticsDebugView);
        ImGui::SliderFloat(
            "Caustic Intensity",
            &settings.causticsIntensity,
            0.0f,
            5.0f);
        ImGui::SliderFloat(
            "Photon Refraction",
            &settings.causticsRefractionScalePixels,
            0.0f,
            64.0f,
            "%.1f px");
        ImGui::SliderFloat(
            "Focus Strength",
            &settings.causticsFocusStrength,
            0.0f,
            8.0f);
        int blurRadius = static_cast<int>(
            settings.causticsBlurRadius);
        if (ImGui::SliderInt(
                "Caustic Blur Radius",
                &blurRadius,
                0,
                12))
        {
            settings.causticsBlurRadius =
                static_cast<unsigned int>(blurRadius);
        }
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Fluid GPU Statistics");
    ImGui::Text(
        "Particles / Grid Cells: %u / %u",
        statistics.particleCount,
        statistics.gridCellCount);
    ImGui::Text(
        "Substeps / Iterations / Dispatches: %u / %u / %u",
        statistics.substepCount,
        statistics.solverIterations,
        statistics.dispatchCount);
    ImGui::Text(
        "Max Cell Occupancy / Capacity: %u / %u",
        statistics.maximumCellOccupancy,
        statistics.maxParticlesPerCell);
    ImGui::Text(
        "Neighbor Overflowed Particles / Capacity: %u / %u",
        statistics.neighborOverflowCount,
        statistics.maxNeighborsPerParticle);
    ImGui::Text(
        "Grid Overflow / Invalid: %u / %u%s",
        statistics.gridOverflowCount,
        statistics.invalidParticleCount,
        statistics.diagnosticsValid ? "" : " (pending)");
    ImGui::Text(
        "Fluid Buffers: %.2f MiB",
        static_cast<double>(statistics.allocatedBufferBytes)
            / (1024.0 * 1024.0));
    if (statistics.resourceRebuiltThisFrame)
    {
        ImGui::TextColored(
            {0.95f, 0.72f, 0.20f, 1.0f},
            "Simulation resources rebuilt this frame");
    }
}
} // namespace Prism::UI
