#pragma once

#include "Core/Application/RenderControlCommand.h"
#include "Core/Profiling/FrameProfilerSnapshot.h"
#include "Renderer/RenderSettings.h"
#include "RHI/FramePacing.h"
#include "Scene/RenderFramePacket.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Prism::UI
{
class UiDrawPacket;
}

namespace Prism::Core
{
struct RenderViewFrameState
{
    Scene::RenderViewId viewId;
    Renderer::RenderSettings settings;
    bool active = false;
};

struct FrameEnvelope
{
    std::shared_ptr<const Scene::RenderFramePacket> frame;
    RenderEpoch sceneEpoch{1};
    RenderEpoch viewEpoch{1};
    std::uint64_t settingsRevision = 1;
    std::vector<RenderViewFrameState> views;
    std::shared_ptr<const UI::UiDrawPacket> uiDrawPacket;
    // Main-lane timestamp captured when input processing for this logical
    // frame begins. The execution service fills this when omitted by tests.
    std::uint64_t producerInputTimestampNanoseconds = 0;
    // Frozen by the main lane and applied by the render execution lane before
    // BeginFrame. These are frame diagnostics, not live backend state.
    ProfilingLevel profilingLevel = ProfilingLevel::Basic;
    bool framePacingEnabled = false;
    // Diagnostic requests are frozen with the frame. The execution lane may
    // inspect live renderer/backend state only while producing these snapshots.
    bool collectPerformanceStatistics = false;
    bool collectRenderGraphReport = false;
    bool collectGpuTimingReport = false;
    std::optional<RHI::FrameAdmissionResult> frameAdmission;
    std::optional<RHI::FramePacingState> framePacingState;
    // Set only when a reliable capture command targets this frame. The
    // execution lane records the selected final output before UI/present.
    std::optional<Scene::RenderViewId> captureViewId;
};

void ValidateFrameEnvelope(const FrameEnvelope& envelope);

struct PreparedRenderFrame
{
    std::shared_ptr<const Scene::RenderFramePacket> frame;
    RenderEpoch sceneEpoch;
    RenderEpoch viewEpoch;
    std::uint64_t settingsRevision = 0;
    std::vector<RenderViewFrameState> views;
    std::shared_ptr<const UI::UiDrawPacket> uiDrawPacket;
    std::optional<Scene::RenderViewId> captureViewId;

    [[nodiscard]] const RenderViewFrameState* FindView(
        Scene::RenderViewId viewId) const noexcept;
};

// Keeps temporal inputs tied to completed render work. Producer-side camera
// history is deliberately replaced with the previous camera actually rendered
// for each view, so hidden or cancelled frames cannot advance motion history.
class RenderFrameExecutionState final
{
public:
    [[nodiscard]] PreparedRenderFrame Prepare(
        const FrameEnvelope& envelope);
    void Commit(const PreparedRenderFrame& prepared);
    void Cancel(const PreparedRenderFrame& prepared) noexcept;
    void InvalidateView(
        Scene::RenderViewId viewId,
        Scene::RenderViewHistoryInvalidation reason);

    [[nodiscard]] Scene::LogicalFrameId
        GetLastCompletedFrame() const noexcept;

private:
    struct ViewHistory
    {
        Scene::Camera camera;
        bool valid = false;
        Scene::RenderViewHistoryInvalidation pendingInvalidation =
            Scene::RenderViewHistoryInvalidation::None;
    };

    [[nodiscard]] static std::size_t ViewIndex(
        Scene::RenderViewId viewId);

    std::array<ViewHistory, 2> m_viewHistory;
    Scene::LogicalFrameId m_lastCompletedFrame;
    Scene::LogicalFrameId m_preparedFrame;
    double m_lastCompletedSimulationTime = 0.0;
    bool m_completedSimulationTimeValid = false;
};
} // namespace Prism::Core
