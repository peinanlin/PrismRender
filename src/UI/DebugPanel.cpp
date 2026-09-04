#include "UI/DebugPanel.h"

#include "Renderer/RendererStatistics.h"
#include "Renderer/RenderSettings.h"

#include <imgui.h>

namespace Prism::UI
{
std::optional<std::uint32_t> DebugPanel::Draw(
    const FrameStats& stats,
    Renderer::RenderSettings& settings,
    const Renderer::RendererStatistics& rendererStatistics) const
{
    std::optional<std::uint32_t> requestedDemoScene;
    ImGui::Begin("PrismRender Debug");
    ImGui::Text("Renderer Labs - D3D12 / Vulkan RHI");
    ImGui::Separator();
    ImGui::Text("Adapter: %s", stats.adapterName.c_str());
    ImGui::Text("Graphics API: %s", stats.graphicsApiName.c_str());
    ImGui::Text("Resolution: %u x %u", stats.windowWidth, stats.windowHeight);
    ImGui::Text("Frame Index: %u", stats.frameIndex);
    ImGui::Text("Render Objects: %u", stats.renderObjectCount);
    ImGui::Text(
        "Active Lights (Point / Spot): %u / %u",
        stats.activePointLightCount,
        stats.activeSpotLightCount);
    ImGui::Text("Assets (Mesh / Texture / Material): %u / %u / %u",
                stats.meshAssetCount,
                stats.textureAssetCount,
                stats.materialAssetCount);
    ImGui::Text("glTF Import: %s", stats.gltfImportEnabled ? "Enabled" : "Disabled");
    ImGui::Text("Fallback Scene: %s", stats.usingFallbackScene ? "Yes" : "No");
    if (!stats.sceneSourceLabel.empty())
    {
        ImGui::Text("Scene Source: %s", stats.sceneSourceLabel.c_str());
    }
    if (!stats.sceneLoadMessage.empty())
    {
        ImGui::TextWrapped("Scene Status: %s", stats.sceneLoadMessage.c_str());
    }
    if (!stats.environmentMapStatus.empty())
    {
        ImGui::TextWrapped("Environment: %s", stats.environmentMapStatus.c_str());
    }
    if (stats.assetStreamingEnabled)
    {
        ImGui::Separator();
        ImGui::TextUnformatted("Asset Streaming");
        ImGui::Text(
            "Queued / Resident / Evicted / Failed: %u / %u / %u / %u",
            stats.streamingQueuedCount,
            stats.streamingResidentCount,
            stats.streamingEvictedCount,
            stats.streamingFailedCount);
        ImGui::Text(
            "Residency: %.2f / %.2f MiB",
            static_cast<double>(stats.streamingResidentBytes)
                / (1024.0 * 1024.0),
            static_cast<double>(stats.streamingBudgetBytes)
                / (1024.0 * 1024.0));
        ImGui::Text(
            "Completed Uploads / Evictions: %llu / %llu",
            static_cast<unsigned long long>(
                stats.streamingCompletedUploads),
            static_cast<unsigned long long>(
                stats.streamingEvictionCount));
    }
    if (!stats.demoScenes.empty())
    {
        const std::uint32_t activeIndex =
            stats.activeDemoSceneIndex < stats.demoScenes.size()
            ? stats.activeDemoSceneIndex
            : 0u;
        const DemoSceneOption& activeScene =
            stats.demoScenes[activeIndex];
        if (!stats.demoSceneSwitchingEnabled)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::BeginCombo(
                "Demo Scene",
                activeScene.displayName.c_str()))
        {
            for (std::uint32_t index = 0;
                 index < stats.demoScenes.size();
                 ++index)
            {
                const bool selected = index == activeIndex;
                if (ImGui::Selectable(
                        stats.demoScenes[index]
                            .displayName.c_str(),
                        selected)
                    && !selected)
                {
                    requestedDemoScene = index;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (!stats.demoSceneSwitchingEnabled)
        {
            ImGui::EndDisabled();
        }
        ImGui::TextWrapped(
            "Demo Goal: %s",
            activeScene.purpose.c_str());
    }
    ImGui::Text("Camera: (%.2f, %.2f, %.2f)", stats.cameraPosition.x, stats.cameraPosition.y, stats.cameraPosition.z);
    ImGui::Text("Pitch / Yaw: %.2f / %.2f deg", stats.cameraPitch, stats.cameraYaw);
    ImGui::Text("FOV / Near / Far: %.1f / %.2f / %.1f", stats.cameraFovYDegrees, stats.cameraNearPlane, stats.cameraFarPlane);
    ImGui::Text("Move Speed: %.2f", stats.cameraMoveSpeed);
    ImGui::Text("Look Mode: %s", stats.cameraLookActive ? "RMB held" : "Idle");
    ImGui::Text(
        "Render Path: %s",
        settings.deferredRenderingEnabled
            ? "Deferred + Clustered"
            : (settings.forwardPlusEnabled
                ? "Forward+"
                : "Forward (4 lights)"));
    ImGui::Separator();
    ImGui::TextUnformatted("Scene Objects");
    if (stats.sceneObjectSummaries.empty())
    {
        ImGui::TextUnformatted("No render objects.");
    }
    else
    {
        for (const std::string& summary : stats.sceneObjectSummaries)
        {
            ImGui::BulletText("%s", summary.c_str());
        }
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Toggles");
    ImGui::Checkbox(
        "Relative-to-Eye Coordinates",
        &settings.relativeToEyeEnabled);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Subtract the double-precision camera origin before uploading float transforms.\n"
            "The Large World / RTE Lab is configured to isolate this path.");
    }
    ImGui::Checkbox("PBR", &settings.pbrEnabled);
    ImGui::Checkbox("IBL", &settings.iblEnabled);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Image-based lighting master switch: environment diffuse and specular lighting.");
    }
    ImGui::Checkbox("Split-Sum IBL", &settings.iblSplitSumEnabled);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Use irradiance + roughness-prefiltered environment + BRDF LUT.\n"
            "Disabled uses a cheaper raw-cubemap Fresnel approximation.");
    }
    ImGui::Checkbox("Skybox", &settings.skyboxEnabled);
    ImGui::SliderFloat(
        "Sky Environment Blend",
        &settings.skyEnvironmentBlend,
        0.0f,
        1.0f,
        "%.2f");
    ImGui::Checkbox("Shadows", &settings.shadowsEnabled);
    ImGui::Checkbox("Cascade Shadows", &settings.cascadeShadowsEnabled);
    constexpr const char* ShadowFilterNames[] = {
        "Hard",
        "PCF 3x3",
        "PCF 5x5",
        "PCSS",
        "VSM",
        "EVSM"};
    int shadowFilterIndex = static_cast<int>(settings.shadowFilterMode);
    if (ImGui::Combo(
            "Shadow Filter",
            &shadowFilterIndex,
            ShadowFilterNames,
            IM_ARRAYSIZE(ShadowFilterNames)))
    {
        settings.shadowFilterMode =
            static_cast<Renderer::ShadowFilterMode>(shadowFilterIndex);
    }
    ImGui::Checkbox("Cascade Debug Tint", &settings.cascadeDebugEnabled);
    ImGui::Checkbox("HDR", &settings.hdrEnabled);
    ImGui::Checkbox("Bloom", &settings.bloomEnabled);
    ImGui::Checkbox("Tonemapping", &settings.tonemappingEnabled);
    ImGui::Checkbox("Deferred Rendering", &settings.deferredRenderingEnabled);
    ImGui::Checkbox("Forward+ (when Forward)", &settings.forwardPlusEnabled);
    ImGui::Checkbox("Direct Lighting", &settings.directLightingEnabled);
    ImGui::Checkbox("Point Lights", &settings.pointLightsEnabled);
    ImGui::Checkbox(
        "Clustered Lighting",
        &settings.clusteredLightingEnabled);
    ImGui::Checkbox(
        "Spot Lights",
        &settings.spotLightsEnabled);
    ImGui::Checkbox(
        "Local Light Shadows",
        &settings.localLightShadowsEnabled);
    ImGui::Checkbox("Normal Mapping", &settings.normalMappingEnabled);
    ImGui::Checkbox("Occlusion", &settings.occlusionEnabled);
    ImGui::Checkbox("Emissive", &settings.emissiveEnabled);
    ImGui::Checkbox("Alpha Mask", &settings.alphaMaskEnabled);
    ImGui::Checkbox("Frustum Culling", &settings.frustumCullingEnabled);
    ImGui::Checkbox("Hi-Z Occlusion Culling", &settings.occlusionCullingEnabled);
    ImGui::Checkbox(
        "Game Camera Output",
        &settings.gameCameraEnabled);
    if (settings.gameCameraEnabled)
    {
        ImGui::TextDisabled(
            "Game View and GPU culling use the Game Camera; Scene View uses the Editor Camera.");
    }
    ImGui::Checkbox("GPU Instancing", &settings.gpuInstancingEnabled);
    if (settings.gpuInstancingEnabled
        && !rendererStatistics.gpuInstancingSupported)
    {
        ImGui::TextDisabled(
            "GPU instancing is unavailable on this shader/backend path; using direct object draws.");
    }
    ImGui::Checkbox(
        "GPU Visibility Readback",
        &settings.gpuVisibilityReadbackEnabled);
    ImGui::Checkbox("GPU Driven", &settings.gpuDrivenEnabled);
    if (settings.gpuDrivenEnabled
        && !rendererStatistics.gpuDrivenSupported)
    {
        ImGui::TextDisabled(
            "GPU-driven draws are unavailable on this backend; using the CPU submission path.");
    }
    ImGui::Checkbox("Virtual Terrain", &settings.virtualTerrainEnabled);
    ImGui::Checkbox(
        "Terrain Virtual Texture",
        &settings.terrainVirtualTextureEnabled);
    ImGui::Checkbox(
        "Terrain Material Colors",
        &settings.terrainMaterialColorsEnabled);
    ImGui::Checkbox(
        "Terrain Tile Boundaries",
        &settings.terrainTileDebugEnabled);
    if (settings.virtualTerrainEnabled)
    {
        ImGui::SliderFloat(
            "Terrain World Size",
            &settings.terrainWorldSize,
            128.0f,
            8192.0f,
            "%.0f m");
        ImGui::SliderFloat(
            "Terrain Tile Border",
            &settings.terrainTileBorderWidth,
            0.5f,
            24.0f,
            "%.1f m");
    }
    if (settings.fluid.enabled)
    {
        ImGui::BeginDisabled();
    }
    ImGui::Checkbox(
        "Temporal AA",
        &settings.temporalAntiAliasingEnabled);
    if (settings.fluid.enabled)
    {
        ImGui::EndDisabled();
        ImGui::TextDisabled(
            "TAA is bypassed until fluid motion vectors are available.");
    }
    ImGui::Checkbox("GTAO", &settings.gtaoEnabled);
    ImGui::Checkbox(
        "Screen Space Reflections",
        &settings.screenSpaceReflectionsEnabled);
    ImGui::Checkbox(
        "Planar Reflections",
        &settings.planarReflectionsEnabled);
    ImGui::SliderFloat(
        "Reflection Plane Y",
        &settings.planarReflectionPlaneHeight,
        -2.0f,
        4.0f,
        "%.2f");
    ImGui::SliderFloat(
        "Planar Reflection Intensity",
        &settings.planarReflectionIntensity,
        0.0f,
        1.0f);
    ImGui::SliderFloat("Ambient", &settings.ambientIntensity, 0.0f, 0.5f);
    ImGui::SliderFloat("IBL Intensity", &settings.iblIntensity, 0.0f, 2.0f);
    ImGui::SliderFloat("IBL Diffuse", &settings.iblDiffuseStrength, 0.0f, 2.0f);
    ImGui::SliderFloat("IBL Specular", &settings.iblSpecularStrength, 0.0f, 2.0f);
    ImGui::SliderFloat("IBL Reflection Blend", &settings.iblReflectionBlend, 0.0f, 1.0f);
    ImGui::SliderFloat("Sky Blend", &settings.iblHorizonSharpness, 0.2f, 4.0f);
    ImGui::ColorEdit3("Sky Zenith", &settings.skyZenithColor.x);
    ImGui::ColorEdit3("Sky Horizon", &settings.skyHorizonColor.x);
    ImGui::ColorEdit3("Ground", &settings.groundColor.x);
    ImGui::SliderFloat(
        "Atmosphere Brightness",
        &settings.atmosphereBrightness,
        0.25f,
        4.0f,
        "%.2f");
    ImGui::SliderFloat("Exposure", &settings.exposure, 0.1f, 3.0f);
    ImGui::SliderFloat("Bloom Threshold", &settings.bloomThreshold, 0.1f, 3.0f);
    ImGui::SliderFloat("Bloom Intensity", &settings.bloomIntensity, 0.0f, 2.0f);
    ImGui::SliderFloat("Shadow Bias", &settings.shadowBias, 0.0001f, 0.02f, "%.4f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Shadow Distance", &settings.shadowDistance, 10.0f, 250.0f, "%.1f");
    ImGui::SliderFloat("Cascade Split", &settings.cascadeSplitLambda, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Cascade Blend", &settings.cascadeBlendFraction, 0.0f, 0.25f, "%.2f");
    ImGui::Spacing();
    ImGui::TextUnformatted("Renderer Statistics");
    ImGui::Text("Visible Objects: %u", rendererStatistics.visibleObjects);
    ImGui::Text("Culled Objects: %u", rendererStatistics.culledObjects);
    ImGui::Text("Rendered Objects: %u", rendererStatistics.renderedObjects);
    ImGui::Text("Draw Calls: %u", rendererStatistics.drawCalls);
    ImGui::Text("Shadow / Forward / Post: %u / %u / %u",
                rendererStatistics.shadowDrawCalls,
                rendererStatistics.forwardDrawCalls,
                rendererStatistics.postProcessDrawCalls);
    ImGui::Text(
        "Directional / Local Shadow: %u / %u",
        rendererStatistics.directionalShadowDrawCalls,
        rendererStatistics.localShadowDrawCalls);
    ImGui::Text(
        "Shadow Layers Rendered / Cached: %u / %u",
        rendererStatistics.shadowLayersRendered,
        rendererStatistics.shadowLayersCached);
    ImGui::Text(
        "Shadow Casters Candidate / Culled: %u / %u",
        rendererStatistics.shadowCasterCandidates,
        rendererStatistics.shadowCastersCulled);
    ImGui::Text(
        "Shadow Instanced Draws: %u",
        rendererStatistics.shadowInstancedDrawCalls);
    ImGui::Text("Instanced Draws: %u", rendererStatistics.instancedDrawCalls);
    ImGui::Text("Instanced Objects: %u", rendererStatistics.instancedObjects);
    ImGui::Text("Instance Batches: %u", rendererStatistics.instanceBatchCount);
    ImGui::Text("Cached PSOs: %u", rendererStatistics.cachedPsoCount);
    ImGui::Text(
        "GPU Candidates / Indirect Draws: %u / %u",
        rendererStatistics.gpuDrivenCandidateObjects,
        rendererStatistics.indirectDrawCalls);
    if (settings.gpuDrivenEnabled
        && rendererStatistics.gpuDrivenSupported)
    {
        if (rendererStatistics.gpuVisibilityStatisticsValid)
        {
            ImGui::Text(
                "GPU Visible / LOD Rejected: %u / %u",
                rendererStatistics.gpuVisibleObjects,
                rendererStatistics.gpuLodRejectedObjects);
            ImGui::Text(
                "GPU Frustum / Hi-Z Culled: %u / %u",
                rendererStatistics.gpuFrustumCulledObjects,
                rendererStatistics.gpuOcclusionCulledObjects);
            ImGui::Text(
                "GPU Disabled: %u",
                rendererStatistics.gpuDisabledObjects);
        }
        else
        {
            ImGui::TextUnformatted(
                "GPU visibility readback: pending");
        }
    }
    if (settings.virtualTerrainEnabled)
    {
        ImGui::Text(
            "VT Resident / Requested: %u / %u",
            rendererStatistics.virtualTextureResidentPages,
            rendererStatistics.virtualTextureRequestedPages);
        ImGui::Text(
            "VT Hits / Misses / Evictions: %u / %u / %u",
            rendererStatistics.virtualTexturePageHits,
            rendererStatistics.virtualTexturePageMisses,
            rendererStatistics.virtualTextureEvictions);
    }
    ImGui::Separator();
    ImGui::TextUnformatted("CPU Profiling (ms)");
    ImGui::Text("Culling: %.3f", rendererStatistics.cullingCpuMs);
    ImGui::Text("Shadow Pass: %.3f", rendererStatistics.shadowPassCpuMs);
    ImGui::Text("Forward Pass: %.3f", rendererStatistics.forwardPassCpuMs);
    ImGui::Text("Bloom Pass: %.3f", rendererStatistics.bloomPassCpuMs);
    ImGui::Text("Tonemap Pass: %.3f", rendererStatistics.tonemapPassCpuMs);
    ImGui::Text("Renderer Total: %.3f", rendererStatistics.totalRendererCpuMs);
    ImGui::Separator();
    ImGui::TextUnformatted("GPU Profiling (ms)");
    ImGui::Text("Shadow: %.3f", rendererStatistics.shadowPassGpuMs);
    ImGui::Text("Geometry: %.3f", rendererStatistics.geometryPassGpuMs);
    ImGui::Text("Bloom: %.3f", rendererStatistics.bloomPassGpuMs);
    ImGui::Text("Tonemap: %.3f", rendererStatistics.tonemapPassGpuMs);
    ImGui::Text("Renderer Total: %.3f", rendererStatistics.totalRendererGpuMs);
    ImGui::Spacing();
    ImGui::TextUnformatted("Notes");
    ImGui::BulletText("Frustum culling uses conservative CPU-side bounding spheres.");
    ImGui::BulletText("GPU instancing batches render objects that share mesh and material.");
    ImGui::BulletText("PSO cache keeps pipeline creation out of steady-state frame work.");
    ImGui::End();
    return requestedDemoScene;
}
} // namespace Prism::UI
