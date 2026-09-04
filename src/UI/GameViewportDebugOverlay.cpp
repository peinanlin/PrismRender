#include "UI/GameViewportDebugOverlay.h"

#include "Renderer/Features/Ocean/OceanSettings.h"
#include "Renderer/RendererStatistics.h"
#include "Renderer/RenderSettings.h"
#include "UI/FluidLabPanel.h"
#include "UI/OceanLabPanel.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace Prism::UI
{
namespace
{
ImVec4 GetFrameRateColor(const float framesPerSecond)
{
    if (framesPerSecond >= 55.0f)
    {
        return {0.38f, 0.90f, 0.48f, 1.0f};
    }
    if (framesPerSecond >= 30.0f)
    {
        return {1.0f, 0.74f, 0.28f, 1.0f};
    }
    return {1.0f, 0.34f, 0.30f, 1.0f};
}

void DrawMetric(const char* label, const char* value)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(value);
}

void DrawCascadeMaskCheckbox(
    const char* id,
    std::uint32_t& mask,
    const std::uint32_t cascade)
{
    bool enabled = (mask & (1u << cascade)) != 0u;
    if (ImGui::Checkbox(id, &enabled))
    {
        if (enabled)
        {
            mask |= 1u << cascade;
        }
        else
        {
            mask &= ~(1u << cascade);
        }
    }
}

void DrawWaveWorksStageControls(Renderer::RenderSettings& settings)
{
    Renderer::OceanDebugSettings& debug = settings.ocean.debug;
    constexpr std::uint32_t allCascades =
        Renderer::OceanDebugSettings::AllCascadeMask;
    if (ImGui::Button("Enable All Stages"))
    {
        debug.spectralDisplacementEnabled = true;
        debug.spectralGradientEnabled = true;
        debug.spectralFoldingEnabled = true;
        debug.spectralFoamHistoryEnabled = true;
        debug.spectralMomentsEnabled = true;
        debug.cascadeMask = allCascades;
        debug.displacementCascadeMask = allCascades;
        debug.gradientCascadeMask = allCascades;
        debug.foldingCascadeMask = allCascades;
        debug.foamHistoryCascadeMask = allCascades;
        debug.momentsCascadeMask = allCascades;
    }
    ImGui::SameLine();
    if (ImGui::Button("Disable Stages"))
    {
        debug.spectralDisplacementEnabled = false;
        debug.spectralGradientEnabled = false;
        debug.spectralFoldingEnabled = false;
        debug.spectralFoamHistoryEnabled = false;
        debug.spectralMomentsEnabled = false;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Keep the per-cascade selections and disable all five spectral consumers.");
    }

    if (ImGui::BeginTable(
            "##WaveWorksStageMasks",
            6,
            ImGuiTableFlags_BordersInnerV
                | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_SizingFixedFit
                | ImGuiTableFlags_NoSavedSettings))
    {
        ImGui::TableSetupColumn(
            "Stage",
            ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("On");
        ImGui::TableSetupColumn("C0");
        ImGui::TableSetupColumn("C1");
        ImGui::TableSetupColumn("C2");
        ImGui::TableSetupColumn("C3");
        ImGui::TableHeadersRow();

        const auto drawMaskRow = [](
            const char* label,
            bool* master,
            std::uint32_t& mask,
            const char* tooltip)
        {
            ImGui::PushID(label);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", tooltip);
            }
            ImGui::TableSetColumnIndex(1);
            if (master != nullptr)
            {
                ImGui::Checkbox("##Master", master);
            }
            else
            {
                ImGui::TextDisabled("-");
            }
            for (std::uint32_t cascade = 0u;
                 cascade < Renderer::OceanDebugSettings::CascadeCount;
                 ++cascade)
            {
                ImGui::TableSetColumnIndex(
                    static_cast<int>(cascade + 2u));
                ImGui::PushID(static_cast<int>(cascade));
                DrawCascadeMaskCheckbox("##Cascade", mask, cascade);
                ImGui::PopID();
            }
            ImGui::PopID();
        };

        drawMaskRow(
            "Cascade Master",
            nullptr,
            debug.cascadeMask,
            "A disabled cascade is removed from every spectral stage below.");
        drawMaskRow(
            "Displacement",
            &debug.spectralDisplacementEnabled,
            debug.displacementCascadeMask,
            "Dx, height, and Dz sampled by the vertex/domain stage.");
        drawMaskRow(
            "Gradient",
            &debug.spectralGradientEnabled,
            debug.gradientCascadeMask,
            "Slope accumulation and final surface-normal reconstruction.");
        drawMaskRow(
            "Folding",
            &debug.spectralFoldingEnabled,
            debug.foldingCascadeMask,
            "Immediate Jacobian folding and breaking-crest hats.");
        drawMaskRow(
            "Foam History",
            &debug.spectralFoamHistoryEnabled,
            debug.foamHistoryCascadeMask,
            "Persistent, advected, and decaying spectral foam energy.");
        drawMaskRow(
            "Moments",
            &debug.spectralMomentsEnabled,
            debug.momentsCascadeMask,
            "Unresolved slope variance used to broaden the GGX lobe.");
        ImGui::EndTable();
    }

    const float coarsestPeriod =
        std::max(settings.ocean.simulationPeriodMeters, 1.0f);
    ImGui::TextDisabled(
        "Cascade periods: C0 %.3g m | C1 %.3g m | C2 %.3g m | C3 %.3g m",
        coarsestPeriod / 64.0f,
        coarsestPeriod / 16.0f,
        coarsestPeriod / 4.0f,
        coarsestPeriod);
    ImGui::TextWrapped(
        "Stage isolation changes final map consumption only. FFT and foam history resources keep advancing for stable re-enable and fixed RenderGraph dependencies.");
}
} // namespace

void GameViewportDebugOverlay::DrawVisibilityToggle()
{
    ImGui::SameLine();
    if (m_visible)
    {
        ImGui::PushStyleColor(
            ImGuiCol_Button,
            ImVec4(0.22f, 0.38f, 0.62f, 1.0f));
    }
    if (ImGui::Button("Stats"))
    {
        m_visible = !m_visible;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Show or hide the Game debug overlay");
    }
    if (m_visible)
    {
        ImGui::PopStyleColor();
    }
}

bool GameViewportDebugOverlay::Draw(
    const ViewportPerformanceStats& stats,
    Renderer::RenderSettings& settings,
    const Renderer::RendererStatistics& rendererStatistics,
    const bool oceanLabActive,
    const bool waveWorksActive,
    const bool fluidLabActive,
    const float viewportX,
    const float viewportY,
    const float viewportWidth,
    const float viewportHeight)
{
    if (!m_visible
        || viewportWidth < 120.0f
        || viewportHeight < 60.0f)
    {
        return false;
    }

    constexpr float Margin = 12.0f;
    const float desiredWidth = m_expanded ? 356.0f : 210.0f;
    const float desiredHeight = m_expanded
        ? (waveWorksActive ? 560.0f : 400.0f)
        : 42.0f;
    const ImVec2 overlaySize{
        std::min(desiredWidth, viewportWidth - Margin * 2.0f),
        std::min(desiredHeight, viewportHeight - Margin * 2.0f)};
    ImGui::SetCursorScreenPos(
        {viewportX + Margin, viewportY + Margin});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(9.0f, 8.0f));
    ImGui::PushStyleColor(
        ImGuiCol_ChildBg,
        ImVec4(0.025f, 0.032f, 0.045f, 0.86f));
    ImGui::PushStyleColor(
        ImGuiCol_Border,
        ImVec4(0.28f, 0.42f, 0.62f, 0.72f));

    const ImGuiChildFlags childFlags =
        ImGuiChildFlags_Borders
        | ImGuiChildFlags_AlwaysUseWindowPadding;
    const ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoSavedSettings;
    const bool open = ImGui::BeginChild(
        "##GameDebugOverlay",
        overlaySize,
        childFlags,
        windowFlags);
    bool hovered = ImGui::IsWindowHovered(
        ImGuiHoveredFlags_ChildWindows);
    if (open)
    {
        char header[96]{};
        if (m_expanded)
        {
            std::snprintf(
                header,
                sizeof(header),
                "v  Game Debug");
        }
        else
        {
            std::snprintf(
                header,
                sizeof(header),
                ">  Editor Loop FPS %.1f  |  Draw %u",
                stats.framesPerSecond,
                stats.drawCalls);
        }
        if (ImGui::Button(
                header,
                ImVec2(-1.0f, 0.0f)))
        {
            m_expanded = !m_expanded;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                m_expanded
                    ? "Collapse performance statistics"
                    : "Expand performance statistics");
        }

        if (m_expanded)
        {
            ImGui::Separator();
            if (ImGui::CollapsingHeader(
                    "Performance",
                    ImGuiTreeNodeFlags_DefaultOpen)
                && ImGui::BeginTable(
                    "##GamePerformanceMetrics",
                    2,
                    ImGuiTableFlags_SizingStretchProp
                        | ImGuiTableFlags_NoSavedSettings))
            {
                ImGui::TableSetupColumn(
                    "Metric",
                    ImGuiTableColumnFlags_WidthStretch,
                    0.60f);
                ImGui::TableSetupColumn(
                    "Value",
                    ImGuiTableColumnFlags_WidthStretch,
                    0.40f);

                char value[96]{};
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Editor Loop FPS");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(
                    GetFrameRateColor(stats.framesPerSecond),
                    "%.1f",
                    stats.framesPerSecond);

                std::snprintf(
                    value,
                    sizeof(value),
                    "%.2f ms",
                    stats.frameTimeMs);
                DrawMetric("Frame Time", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    "%s",
                    stats.profilingLevel.data());
                DrawMetric("Profiling Level", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    "Game %s / Scene %s",
                    (stats.activeViewMask & 0x1u) != 0u
                        ? "active"
                        : "hidden",
                    (stats.activeViewMask & 0x2u) != 0u
                        ? "active"
                        : "hidden");
                DrawMetric("Active Views", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    stats.mainCpuAvailable
                        ? "%.2f ms"
                        : "unavailable",
                    stats.mainCpuMs);
                DrawMetric("Main CPU", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    stats.renderCpuAvailable
                        ? "%.2f ms"
                        : "unavailable",
                    stats.renderCpuMs);
                DrawMetric("Render CPU", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    stats.workerCpuAvailable
                        ? "%.2f ms"
                        : "unavailable",
                    stats.workerCpuMs);
                DrawMetric("Worker CPU", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    stats.gpuFrameAvailable
                        ? "%.2f ms"
                        : "unavailable",
                    stats.gpuFrameMs);
                DrawMetric("GPU Frame", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    stats.submitPresentWaitAvailable
                        ? "%.2f ms"
                        : "unavailable",
                    stats.submitPresentWaitMs);
                DrawMetric("Submit / Present Wait", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    "%u",
                    stats.drawCalls);
                DrawMetric("Draw Calls", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    "%u / %u / %u",
                    stats.shadowDrawCalls,
                    stats.geometryDrawCalls,
                    stats.postProcessDrawCalls);
                DrawMetric("Shadow / Geo / Post", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    "%u / %u",
                    stats.directionalShadowDrawCalls,
                    stats.localShadowDrawCalls);
                DrawMetric("Directional / Local", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    "%u / %u",
                    stats.shadowLayersRendered,
                    stats.shadowLayersCached);
                DrawMetric("Shadow Layers R / C", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    "%u / %u",
                    stats.visibleObjects,
                    stats.renderedObjects);
                DrawMetric("Visible / Rendered", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    "%u",
                    stats.culledObjects);
                DrawMetric("Culled", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    "%.2f ms",
                    stats.rendererCpuMs);
                DrawMetric("Renderer CPU", value);
                std::snprintf(
                    value,
                    sizeof(value),
                    "%.2f ms",
                    stats.rendererGpuMs);
                DrawMetric("Renderer GPU", value);
                ImGui::EndTable();
            }
            if (waveWorksActive
                && ImGui::CollapsingHeader(
                    "WaveWorks Stage Isolation",
                    ImGuiTreeNodeFlags_DefaultOpen))
            {
                DrawWaveWorksStageControls(settings);
            }
            if (oceanLabActive
                && ImGui::CollapsingHeader("Ocean Lab Parameters"))
            {
                OceanLabPanel{}.Draw(
                    settings,
                    &rendererStatistics);
            }
            if (fluidLabActive
                && ImGui::CollapsingHeader("Fluid Lab Parameters"))
            {
                FluidLabPanel{}.Draw(
                    settings.fluid,
                    rendererStatistics.fluid);
            }
        }
    }
    hovered |= ImGui::IsWindowHovered(
        ImGuiHoveredFlags_ChildWindows);
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    return hovered;
}
} // namespace Prism::UI
