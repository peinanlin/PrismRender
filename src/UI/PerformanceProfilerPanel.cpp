#include "UI/PerformanceProfilerPanel.h"
#include "UI/PerformanceProfilerPanelModel.h"

#include "Core/Profiling/FrameProfiler.h"
#include "Core/Profiling/SceneViewRefreshController.h"

#include <imgui.h>

#include <algorithm>

namespace Prism::UI
{
namespace
{
template <typename Enum>
constexpr std::size_t Index(const Enum value) noexcept
{
    return static_cast<std::size_t>(value);
}

void DrawDuration(
    const char* const label,
    const Core::FrameProfileDuration& duration)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    if (duration.available)
    {
        ImGui::Text("%.3f ms", duration.milliseconds);
    }
    else
    {
        ImGui::TextDisabled("unavailable");
    }
}
} // namespace

std::optional<RHI::FramePacingProfile> PerformanceProfilerPanel::Draw(
    Core::FrameProfiler& profiler,
    Core::SceneViewRefreshController& sceneViewRefresh)
{
    if (!m_open)
    {
        return std::nullopt;
    }
    const bool expanded = ImGui::Begin(
        "Performance Profiler", &m_open);
    if (!PerformanceProfilerPanelModel::ShouldReadCompletedFrame(
            m_open, expanded))
    {
        ImGui::End();
        return std::nullopt;
    }

    int level = static_cast<int>(
        profiler.GetRequestedLevel());
    if (ImGui::Combo(
            "Profiling level",
            &level,
            "Off\0Basic\0Detailed\0Capture\0"))
    {
        profiler.SetRequestedLevel(
            static_cast<Core::ProfilingLevel>(level));
    }
    int scenePolicy = static_cast<int>(
        sceneViewRefresh.GetPolicy());
    if (ImGui::Combo(
            "Scene View refresh",
            &scenePolicy,
            "Catalog default\0Live\0On interaction\0"
            "30 Hz\0Paused\0"))
    {
        sceneViewRefresh.SetPolicy(
            static_cast<Core::SceneViewRefreshPolicy>(
                scenePolicy));
    }

    const auto latest = profiler.GetLatest();
    if (!latest.has_value())
    {
        ImGui::TextDisabled("Waiting for the first completed frame...");
        ImGui::End();
        return std::nullopt;
    }
    const Core::FrameProfilerSnapshot& frame = *latest;
    std::optional<RHI::FramePacingProfile> requestedFramePacing;
    int pacingProfile = frame.framePacing.profile == "low-latency" ? 1
        : frame.framePacing.profile == "benchmark" ? 2 : 0;
    if (ImGui::Combo(
            "Frame pacing",
            &pacingProfile,
            "Interactive smooth\0Low latency\0Benchmark\0"))
    {
        requestedFramePacing = pacingProfile == 1
            ? RHI::FramePacingProfile::LowLatency
            : pacingProfile == 2
                ? RHI::FramePacingProfile::Benchmark
                : RHI::FramePacingProfile::InteractiveSmooth;
    }
    const double editorLoopFps =
        frame.editorLoop.available
            && frame.editorLoop.milliseconds > 0.0
        ? 1000.0 / frame.editorLoop.milliseconds
        : 0.0;
    ImGui::Text("Completed frame: %llu",
        static_cast<unsigned long long>(frame.frameId));
    ImGui::Text("Editor Loop FPS: %.1f", editorLoopFps);
    ImGui::SameLine();
    ImGui::TextDisabled(
        "actual: %s",
        Core::ToString(frame.actualLevel).data());

    if (ImGui::BeginTable(
            "##ProfilerSummary",
            2,
            ImGuiTableFlags_BordersInnerH
                | ImGuiTableFlags_SizingStretchProp))
    {
        DrawDuration("Editor Loop", frame.editorLoop);
        DrawDuration("Main active",
            frame.cpuLanes[Index(Core::FrameCpuLane::Main)].active);
        DrawDuration("Main wait",
            frame.cpuLanes[Index(Core::FrameCpuLane::Main)].waiting);
        DrawDuration("Render lane",
            frame.cpuLanes[Index(Core::FrameCpuLane::Render)].active);
        DrawDuration("Render completion wait",
            frame.pipeline.completionWait);
        DrawDuration("Main N+1 overlap",
            frame.pipeline.mainPreparationOverlap);
        DrawDuration("Input to Present",
            frame.pipeline.inputToPresent);
        DrawDuration("Worker",
            frame.cpuLanes[Index(Core::FrameCpuLane::Worker)].active);
        DrawDuration("GPU critical hint", frame.gpuFrame);
        DrawDuration("Display admission",
            frame.waits[Index(Core::FrameWaitPhase::DisplayAdmission)]);
        DrawDuration("Frame limiter",
            frame.waits[Index(Core::FrameWaitPhase::FrameLimiter)]);
        DrawDuration("Submit",
            frame.waits[Index(Core::FrameWaitPhase::Submit)]);
        DrawDuration("Native Present",
            frame.waits[Index(Core::FrameWaitPhase::NativePresent)]);
        DrawDuration("Frame fence",
            frame.waits[Index(Core::FrameWaitPhase::FrameFence)]);
        DrawDuration("Profiler overhead", frame.profilerOverhead);
        ImGui::EndTable();
    }
    if (frame.framePacing.available)
    {
        ImGui::Text(
            "Pacing: %s | requested=%s effective=%s native=%s",
            frame.framePacing.profile.c_str(),
            frame.framePacing.requestedPresentation.c_str(),
            frame.framePacing.effectivePresentation.c_str(),
            frame.framePacing.nativePresentMode.c_str());
        ImGui::Text(
            "queue=%u/%u images=%u resources=%u sync=%u tearing=%s generation=%llu/%llu",
            frame.framePacing.configuredMaxQueuedFrames,
            frame.framePacing.effectiveMaxQueuedFrames,
            frame.framePacing.swapchainImageCount,
            frame.framePacing.frameResourceSlotCount,
            frame.framePacing.syncInterval,
            frame.framePacing.tearingEnabled ? "enabled" : "disabled",
            static_cast<unsigned long long>(
                frame.framePacing.requestedGeneration),
            static_cast<unsigned long long>(
                frame.framePacing.effectiveGeneration));
        if (!frame.framePacing.fallbackReason.empty())
        {
            ImGui::TextDisabled("Fallback: %s",
                frame.framePacing.fallbackReason.c_str());
        }
        if (frame.framePacing.profile == "benchmark")
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                "Benchmark mode may tear and increase power use.");
        }
    }
    ImGui::TextDisabled(
        "frame=%llu acceptance=%llu epochs=%llu/%llu settings=%llu queue=%u peak=%u",
        static_cast<unsigned long long>(frame.pipeline.logicalFrameId),
        static_cast<unsigned long long>(frame.pipeline.acceptanceId),
        static_cast<unsigned long long>(frame.pipeline.sceneEpoch),
        static_cast<unsigned long long>(frame.pipeline.viewEpoch),
        static_cast<unsigned long long>(frame.pipeline.settingsRevision),
        frame.pipeline.waitingDepthAtAcceptance,
        frame.pipeline.peakWaitingDepth);

    for (const Core::ProfiledView viewId : {
             Core::ProfiledView::Game,
             Core::ProfiledView::Scene})
    {
        const Core::FrameViewProfile& view =
            frame.views[Index(viewId)];
        ImGui::SeparatorText(Core::ToString(viewId).data());
        ImGui::Text(
            "active=%s rendered=%s policy=%s reason=%s extent=%ux%u",
            view.active ? "yes" : "no",
            view.rendered ? "yes" : "no",
            Core::ToString(view.refreshPolicy).data(),
            Core::ToString(view.refreshReason).data(),
            view.width,
            view.height);
        if (view.cpuRender.available)
            ImGui::Text("CPU: %.3f ms", view.cpuRender.milliseconds);
        else
            ImGui::TextDisabled("CPU: unavailable");
        if (view.gpuTotal.available)
            ImGui::Text("GPU: %.3f ms", view.gpuTotal.milliseconds);
        else
            ImGui::TextDisabled("GPU: unavailable");
        if (view.gpuTotal.available)
        {
            ImGui::Text("Resolved CPU frame %llu, generation %llu",
                static_cast<unsigned long long>(view.gpuResolvedFrameId),
                static_cast<unsigned long long>(view.gpuGeneration));
        }
    }

    const bool timelineExpanded =
        ImGui::CollapsingHeader("Recent 240 completed frames");
    if (PerformanceProfilerPanelModel::ShouldReadHistory(
            m_open, expanded, timelineExpanded))
    {
        const auto history = profiler.GetHistory();
        const PerformanceProfilerTimeline timeline =
            PerformanceProfilerPanelModel::BuildTimeline(history);
        ImGui::PlotLines(
            "Editor Loop ms",
            timeline.editorLoopMilliseconds.data(),
            static_cast<int>(timeline.count),
            0,
            nullptr,
            0.0f,
            33.333f,
            ImVec2(0.0f, 90.0f));
    }
    ImGui::End();
    return requestedFramePacing;
}
} // namespace Prism::UI
