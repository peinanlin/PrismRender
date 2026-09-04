#pragma once

#include "Scene/RenderFramePacket.h"
#include "Scene/RenderSceneIdentity.h"

#include <array>
#include <cstdint>

namespace Prism::Scene
{
class Camera;
class RenderScene;

// Bounded, per-session dynamic input state. It is deliberately separate from
// RenderSceneData so camera navigation, lighting and temporal settings never
// advance the immutable object-data revision.
class RenderDynamicInputState final
{
public:
    void ResetForScene(SceneGeneration sceneGeneration);
    void BeginFrame(
        LogicalFrameId logicalFrameId,
        double simulationTimeSeconds,
        double simulationDeltaSeconds = 1.0 / 60.0);
    bool SynchronizeLights(const RenderScene& scene);
    bool SynchronizeView(
        RenderViewId viewId,
        const Camera& camera,
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t historySettingsRevision);
    void NotifyHistoryInvalidation(
        RenderViewId viewId,
        RenderViewHistoryInvalidation reason);

    [[nodiscard]] SceneGeneration GetSceneGeneration() const noexcept;
    [[nodiscard]] LogicalFrameId GetLogicalFrameId() const noexcept;
    [[nodiscard]] double GetSimulationTimeSeconds() const noexcept;
    [[nodiscard]] double GetSimulationDeltaSeconds() const noexcept;
    [[nodiscard]] const RenderFrameDynamicData& GetDynamicData() const noexcept;
    [[nodiscard]] const RenderView* FindView(RenderViewId viewId) const;

private:
    struct ViewState
    {
        RenderView view;
        std::uint64_t historySettingsRevision = 0;
        RenderViewHistoryInvalidation pendingInvalidation =
            RenderViewHistoryInvalidation::None;
        bool initialized = false;
    };

    [[nodiscard]] static std::size_t ViewIndex(RenderViewId viewId);
    [[nodiscard]] ViewState& GetViewState(RenderViewId viewId);
    [[nodiscard]] const ViewState& GetViewState(RenderViewId viewId) const;

    SceneGeneration m_sceneGeneration{1};
    LogicalFrameId m_logicalFrameId{};
    double m_simulationTimeSeconds = 0.0;
    double m_simulationDeltaSeconds = 1.0 / 60.0;
    RenderFrameDynamicData m_dynamicData{};
    std::array<ViewState, 2> m_views{};
};
} // namespace Prism::Scene
