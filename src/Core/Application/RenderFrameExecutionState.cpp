#include "Core/Application/RenderFrameExecutionState.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace Prism::Core
{
namespace
{
constexpr double SimulationStepTolerance = 1.0e-9;

bool IsKnownView(const Scene::RenderViewId viewId) noexcept
{
    return viewId == Scene::GameRenderViewId
        || viewId == Scene::SceneRenderViewId;
}
} // namespace

void ValidateFrameEnvelope(const FrameEnvelope& envelope)
{
    if (!envelope.frame)
    {
        throw std::invalid_argument(
            "FrameEnvelope requires an immutable frame packet.");
    }
    if (!envelope.sceneEpoch || !envelope.viewEpoch
        || envelope.settingsRevision == 0)
    {
        throw std::invalid_argument(
            "FrameEnvelope requires scene, view, and settings identities.");
    }
    if (envelope.views.size()
        != envelope.frame->GetViews().size())
    {
        throw std::invalid_argument(
            "FrameEnvelope requires one frozen settings value per packet view.");
    }

    std::unordered_set<std::uint64_t> viewIds;
    for (const RenderViewFrameState& view : envelope.views)
    {
        if (!IsKnownView(view.viewId)
            || envelope.frame->FindView(view.viewId) == nullptr)
        {
            throw std::invalid_argument(
                "FrameEnvelope contains settings for an unknown render view.");
        }
        if (!viewIds.insert(view.viewId.value).second)
        {
            throw std::invalid_argument(
                "FrameEnvelope contains duplicate render-view settings.");
        }
    }
    if (envelope.captureViewId.has_value())
    {
        const auto selected = std::ranges::find_if(
            envelope.views,
            [&envelope](const RenderViewFrameState& view)
            {
                return view.viewId == *envelope.captureViewId;
            });
        if (selected == envelope.views.end() || !selected->active)
        {
            throw std::invalid_argument(
                "FrameEnvelope capture view must be an active frozen view.");
        }
    }
}

const RenderViewFrameState* PreparedRenderFrame::FindView(
    const Scene::RenderViewId viewId) const noexcept
{
    const auto found = std::ranges::find_if(
        views,
        [viewId](const RenderViewFrameState& view)
        {
            return view.viewId == viewId;
        });
    return found != views.end() ? &*found : nullptr;
}

PreparedRenderFrame RenderFrameExecutionState::Prepare(
    const FrameEnvelope& envelope)
{
    ValidateFrameEnvelope(envelope);
    if (m_preparedFrame)
    {
        throw std::logic_error(
            "A render frame is already prepared for execution.");
    }

    const Scene::LogicalFrameId logicalFrameId =
        envelope.frame->GetLogicalFrameId();
    if (m_lastCompletedFrame
        && logicalFrameId.value <= m_lastCompletedFrame.value)
    {
        throw std::invalid_argument(
            "Rendered logical frame IDs must advance monotonically.");
    }

    const double simulationTime =
        envelope.frame->GetSimulationTimeSeconds();
    const double simulationDelta =
        envelope.frame->GetSimulationDeltaSeconds();
    if (m_completedSimulationTimeValid)
    {
        const double expectedDelta =
            simulationTime - m_lastCompletedSimulationTime;
        if (expectedDelta < -SimulationStepTolerance
            || std::abs(expectedDelta - simulationDelta)
                > SimulationStepTolerance)
        {
            throw std::invalid_argument(
                "Frame simulation step does not match the previous completed render frame.");
        }
    }

    std::vector<Scene::RenderView> effectiveViews =
        envelope.frame->GetViews();
    for (Scene::RenderView& view : effectiveViews)
    {
        const std::size_t index = ViewIndex(view.id);
        const auto stateIterator = std::ranges::find_if(
            envelope.views,
            [&view](const RenderViewFrameState& candidate)
            {
                return candidate.viewId == view.id;
            });
        if (stateIterator == envelope.views.end())
        {
            throw std::invalid_argument(
                "FrameEnvelope is missing frozen view state.");
        }
        const RenderViewFrameState& viewState = *stateIterator;

        const ViewHistory& history = m_viewHistory[index];
        view.previousCamera = history.camera;
        view.previousCameraValid = history.valid;
        const Scene::RenderViewHistoryInvalidation pending =
            history.pendingInvalidation | view.historyInvalidation;
        view.historyInvalidation = pending;
        if (viewState.active
            && pending != Scene::RenderViewHistoryInvalidation::None)
        {
            view.previousCameraValid = false;
        }
    }

    PreparedRenderFrame prepared{};
    prepared.frame = std::make_shared<const Scene::RenderFramePacket>(
        logicalFrameId,
        simulationTime,
        envelope.frame->GetSceneData(),
        envelope.frame->GetDynamicData(),
        std::move(effectiveViews),
        simulationDelta);
    prepared.sceneEpoch = envelope.sceneEpoch;
    prepared.viewEpoch = envelope.viewEpoch;
    prepared.settingsRevision = envelope.settingsRevision;
    prepared.views = envelope.views;
    prepared.uiDrawPacket = envelope.uiDrawPacket;
    prepared.captureViewId = envelope.captureViewId;
    m_preparedFrame = logicalFrameId;
    return prepared;
}

void RenderFrameExecutionState::Commit(
    const PreparedRenderFrame& prepared)
{
    if (!prepared.frame
        || prepared.frame->GetLogicalFrameId() != m_preparedFrame)
    {
        throw std::logic_error(
            "Only the currently prepared render frame may complete.");
    }

    for (const Scene::RenderView& view : prepared.frame->GetViews())
    {
        const RenderViewFrameState* const viewState =
            prepared.FindView(view.id);
        if (viewState == nullptr)
        {
            throw std::logic_error(
                "Prepared render frame lost its frozen view state.");
        }
        ViewHistory& history = m_viewHistory[ViewIndex(view.id)];
        if (viewState->active)
        {
            history.camera = view.camera;
            history.valid = true;
            history.pendingInvalidation =
                Scene::RenderViewHistoryInvalidation::None;
        }
        else
        {
            const Scene::RenderView* const source =
                prepared.frame->FindView(view.id);
            history.pendingInvalidation =
                history.pendingInvalidation
                | (source != nullptr
                    ? source->historyInvalidation
                    : Scene::RenderViewHistoryInvalidation::None);
        }
    }

    m_lastCompletedFrame = prepared.frame->GetLogicalFrameId();
    m_lastCompletedSimulationTime =
        prepared.frame->GetSimulationTimeSeconds();
    m_completedSimulationTimeValid = true;
    m_preparedFrame = {};
}

void RenderFrameExecutionState::Cancel(
    const PreparedRenderFrame& prepared) noexcept
{
    if (prepared.frame
        && prepared.frame->GetLogicalFrameId() == m_preparedFrame)
    {
        m_preparedFrame = {};
    }
}

void RenderFrameExecutionState::InvalidateView(
    const Scene::RenderViewId viewId,
    const Scene::RenderViewHistoryInvalidation reason)
{
    if (reason == Scene::RenderViewHistoryInvalidation::None)
    {
        return;
    }
    ViewHistory& history = m_viewHistory[ViewIndex(viewId)];
    history.pendingInvalidation =
        history.pendingInvalidation | reason;
}

Scene::LogicalFrameId
RenderFrameExecutionState::GetLastCompletedFrame() const noexcept
{
    return m_lastCompletedFrame;
}

std::size_t RenderFrameExecutionState::ViewIndex(
    const Scene::RenderViewId viewId)
{
    if (viewId == Scene::GameRenderViewId)
    {
        return 0;
    }
    if (viewId == Scene::SceneRenderViewId)
    {
        return 1;
    }
    throw std::invalid_argument(
        "Render execution only supports the stable Game and Scene views.");
}
} // namespace Prism::Core
