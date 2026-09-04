#include "Core/Application/RenderFrameQueue.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <type_traits>
#include <utility>

static_assert(std::is_move_constructible_v<Prism::Core::FrameEnvelope>);

namespace Prism::Core
{
namespace
{
[[nodiscard]] std::uint64_t SteadyClockNanoseconds() noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

[[noreturn]] void ThrowQueueUnavailable(
    const RenderFrameQueueState state,
    const std::exception_ptr& failure)
{
    if (state == RenderFrameQueueState::Failed && failure)
    {
        std::rethrow_exception(failure);
    }
    throw std::runtime_error(
        "RenderFrameQueue is no longer accepting work.");
}
} // namespace

RenderFrameQueue::RenderFrameQueue(const std::size_t capacity)
    : m_capacity(capacity)
{
    if (m_capacity == 0 || m_capacity > MaximumCapacity)
    {
        throw std::invalid_argument(
            "RenderFrameQueue capacity must be within its fixed limit.");
    }
}

std::uint64_t RenderFrameQueue::Push(FrameEnvelope frame)
{
    return PushPayload(std::move(frame));
}

std::uint64_t RenderFrameQueue::Push(RenderControlCommand command)
{
    return Push(QueuedRenderControl{
        std::move(command),
        std::make_shared<RenderControlCompletionState>()});
}

std::uint64_t RenderFrameQueue::Push(QueuedRenderControl command)
{
    return PushPayload(std::move(command));
}

std::optional<std::uint64_t> RenderFrameQueue::TryPush(
    FrameEnvelope frame)
{
    return TryPushPayload(std::move(frame));
}

std::optional<std::uint64_t> RenderFrameQueue::TryPush(
    RenderControlCommand command)
{
    return TryPush(QueuedRenderControl{
        std::move(command),
        std::make_shared<RenderControlCompletionState>()});
}

std::optional<std::uint64_t> RenderFrameQueue::TryPush(
    QueuedRenderControl command)
{
    return TryPushPayload(std::move(command));
}

std::uint64_t RenderFrameQueue::PushPayload(
    RenderExecutionPayload payload)
{
    ValidatePayload(payload);
    std::unique_lock lock(m_mutex);
    const bool waitsForSpace =
        m_state == RenderFrameQueueState::Open
        && m_entries.size() == m_capacity;
    if (waitsForSpace)
    {
        ++m_waitingProducerCount;
    }
    m_spaceAvailable.wait(lock, [this]
    {
        return m_state != RenderFrameQueueState::Open
            || m_entries.size() < m_capacity;
    });
    if (waitsForSpace)
    {
        --m_waitingProducerCount;
    }
    if (m_state != RenderFrameQueueState::Open)
    {
        ThrowQueueUnavailable(m_state, m_failure);
    }
    const std::uint64_t acceptanceId =
        AcceptPayload(std::move(payload));
    lock.unlock();
    m_workAvailable.notify_one();
    return acceptanceId;
}

std::optional<std::uint64_t> RenderFrameQueue::TryPushPayload(
    RenderExecutionPayload payload)
{
    ValidatePayload(payload);
    std::unique_lock lock(m_mutex);
    if (m_state != RenderFrameQueueState::Open)
    {
        ThrowQueueUnavailable(m_state, m_failure);
    }
    if (m_entries.size() == m_capacity)
    {
        return std::nullopt;
    }
    const std::uint64_t acceptanceId =
        AcceptPayload(std::move(payload));
    lock.unlock();
    m_workAvailable.notify_one();
    return acceptanceId;
}

std::optional<RenderFrameQueueEntry> RenderFrameQueue::WaitPop()
{
    std::unique_lock lock(m_mutex);
    m_workAvailable.wait(lock, [this]
    {
        return !m_entries.empty()
            || m_state != RenderFrameQueueState::Open;
    });
    if (m_entries.empty())
    {
        if (m_state == RenderFrameQueueState::Draining)
        {
            m_state = RenderFrameQueueState::Closed;
        }
        if (m_state == RenderFrameQueueState::Failed)
        {
            ThrowQueueUnavailable(m_state, m_failure);
        }
        return std::nullopt;
    }

    RenderFrameQueueEntry entry = std::move(m_entries.front());
    m_entries.pop_front();
    ++m_consumedCount;
    lock.unlock();
    m_spaceAvailable.notify_one();
    return entry;
}

void RenderFrameQueue::Close() noexcept
{
    {
        std::lock_guard lock(m_mutex);
        if (m_state == RenderFrameQueueState::Open)
        {
            m_state = RenderFrameQueueState::Draining;
        }
    }
    m_workAvailable.notify_all();
    m_spaceAvailable.notify_all();
}

void RenderFrameQueue::Fail(std::exception_ptr failure) noexcept
{
    {
        std::lock_guard lock(m_mutex);
        if (m_state == RenderFrameQueueState::Closed
            || m_state == RenderFrameQueueState::Failed)
        {
            return;
        }
        if (!failure)
        {
            failure = std::make_exception_ptr(std::runtime_error(
                "Render execution failed without an exception."));
        }
        m_failure = std::move(failure);
        m_state = RenderFrameQueueState::Failed;
        m_cancelledCount += m_entries.size();
        for (const RenderFrameQueueEntry& entry : m_entries)
        {
            if (const auto* control =
                    std::get_if<QueuedRenderControl>(&entry.payload);
                control && control->completion)
            {
                control->completion->Fail(failure);
            }
        }
        m_entries.clear();
    }
    m_workAvailable.notify_all();
    m_spaceAvailable.notify_all();
}

void RenderFrameQueue::RethrowFailure() const
{
    std::exception_ptr failure;
    {
        std::lock_guard lock(m_mutex);
        failure = m_failure;
    }
    if (failure)
    {
        std::rethrow_exception(failure);
    }
}

RenderFrameQueueStatistics
RenderFrameQueue::GetStatistics() const noexcept
{
    std::lock_guard lock(m_mutex);
    return {
        m_acceptedCount,
        m_consumedCount,
        m_cancelledCount,
        m_entries.size(),
        m_peakWaitingCount,
        m_waitingProducerCount,
        m_capacity,
        m_state};
}

void RenderFrameQueue::ValidatePayload(
    const RenderExecutionPayload& payload) const
{
    std::visit([](const auto& value)
    {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, FrameEnvelope>)
        {
            ValidateFrameEnvelope(value);
        }
        else
        {
            const RenderControlCommand& command = value.command;
            if (!value.completion)
            {
                throw std::invalid_argument(
                    "Queued render control requires completion state.");
            }
            if (!command.id)
            {
                throw std::invalid_argument(
                    "Render control command requires a non-zero ID.");
            }
            if (const auto* resize =
                    std::get_if<ResizeRenderViewCommand>(&command.payload);
                resize && (!resize->viewId
                    || resize->width == 0
                    || resize->height == 0))
            {
                throw std::invalid_argument(
                    "Resize control requires a view and non-zero extent.");
            }
            if (!command.targetFrame
                || !command.sceneEpoch
                || !command.viewEpoch)
            {
                throw std::invalid_argument(
                    "Render control requires frame, scene epoch, and view epoch identity.");
            }
            if (const auto* quality =
                    std::get_if<ChangeRenderQualityCommand>(
                        &command.payload);
                quality && quality->settingsRevision == 0)
            {
                throw std::invalid_argument(
                    "Quality control requires a non-zero settings revision.");
            }
            if (const auto* framePacing =
                    std::get_if<ChangeFramePacingCommand>(
                        &command.payload);
                framePacing != nullptr)
            {
                RHI::ValidateFramePacingConfiguration(
                    framePacing->configuration);
                if (framePacing->generation == 0)
                {
                    throw std::invalid_argument(
                        "Frame-pacing control requires a non-zero generation.");
                }
            }
            const auto* sceneSwitch =
                std::get_if<SwitchRenderSceneCommand>(
                    &command.payload);
            if (sceneSwitch
                    && (!sceneSwitch->nextSceneEpoch
                        || sceneSwitch->nextSceneEpoch
                            <= command.sceneEpoch))
            {
                throw std::invalid_argument(
                    "Scene switch requires a newer scene epoch.");
            }
            if (sceneSwitch != nullptr
                && sceneSwitch->demoScene.has_value()
                && sceneSwitch->aspectRatio <= 0.0f)
            {
                throw std::invalid_argument(
                    "Demo scene switch requires a positive aspect ratio.");
            }
            const auto* capture =
                std::get_if<CaptureRenderFrameCommand>(
                    &command.payload);
            if (capture != nullptr && capture->requestId == 0)
            {
                throw std::invalid_argument(
                    "Capture control requires a non-zero request ID.");
            }
            if (capture != nullptr && capture->outputPath.empty())
            {
                throw std::invalid_argument(
                    "Capture control requires a non-empty output path.");
            }
            if (const auto* assetWork =
                    std::get_if<ProcessAssetRenderWorkCommand>(
                        &command.payload);
                assetWork != nullptr
                    && assetWork->logicalFrameId == 0)
            {
                throw std::invalid_argument(
                    "Asset render work control requires a non-zero logical frame ID.");
            }
            if (const auto* streamingReport =
                    std::get_if<WriteAssetStreamingReportCommand>(
                        &command.payload);
                streamingReport != nullptr
                    && streamingReport->outputPath.empty())
            {
                throw std::invalid_argument(
                    "Asset streaming report control requires a non-empty output path.");
            }
            if (const auto* assetImport =
                    std::get_if<ImportRenderAssetCommand>(
                        &command.payload);
                assetImport != nullptr
                    && assetImport->sourcePath.empty())
            {
                throw std::invalid_argument(
                    "Asset import control requires a non-empty source path.");
            }
        }
    }, payload);
}

std::uint64_t RenderFrameQueue::AcceptPayload(
    RenderExecutionPayload payload)
{
    if (const auto* frame = std::get_if<FrameEnvelope>(&payload))
    {
        const std::uint64_t logicalFrameId =
            frame->frame->GetLogicalFrameId().value;
        if (logicalFrameId <= m_lastLogicalFrameId)
        {
            throw std::invalid_argument(
                "Render frames must be accepted in increasing logical order.");
        }
        if (m_pendingBeforeFrameId != 0
            && logicalFrameId != m_pendingBeforeFrameId)
        {
            throw std::invalid_argument(
                "A frame bypassed reliable before-frame controls for another target.");
        }
        if ((m_lastSceneEpoch != 0
                && frame->sceneEpoch.value != m_lastSceneEpoch)
            || (m_lastViewEpoch != 0
                && frame->viewEpoch.value != m_lastViewEpoch)
            || (m_lastSettingsRevision != 0
                && frame->settingsRevision
                    != m_lastSettingsRevision))
        {
            throw std::invalid_argument(
                "Render frame used stale or unannounced execution identities.");
        }
        m_lastLogicalFrameId = logicalFrameId;
        m_lastSceneEpoch = frame->sceneEpoch.value;
        m_lastViewEpoch = frame->viewEpoch.value;
        m_lastSettingsRevision = frame->settingsRevision;
        m_pendingBeforeFrameId = 0;
    }
    else
    {
        const RenderControlCommand& command =
            std::get<QueuedRenderControl>(payload).command;
        const std::uint64_t commandId = command.id.value;
        if (commandId <= m_lastControlCommandId)
        {
            throw std::invalid_argument(
                "Render controls must be accepted in increasing ID order.");
        }

        const std::uint64_t targetFrame = command.targetFrame.value;
        if (command.boundary == RenderControlBoundary::BeforeFrame
            && targetFrame <= m_lastLogicalFrameId)
        {
            throw std::invalid_argument(
                "Before-frame control was accepted after its target frame.");
        }
        if (command.boundary == RenderControlBoundary::BeforeFrame
            && m_pendingBeforeFrameId != 0
            && targetFrame != m_pendingBeforeFrameId)
        {
            throw std::invalid_argument(
                "Before-frame controls cannot bypass an earlier target boundary.");
        }
        if (command.boundary == RenderControlBoundary::AfterFrame
            && targetFrame != m_lastLogicalFrameId)
        {
            throw std::invalid_argument(
                "After-frame control was accepted before its target frame.");
        }

        const bool firstEpoch = m_lastSceneEpoch == 0;
        if (!firstEpoch
            && command.sceneEpoch.value != m_lastSceneEpoch)
        {
            throw std::invalid_argument(
                "Render control used a stale or unannounced scene epoch.");
        }
        const bool isSceneSwitch =
            std::holds_alternative<SwitchRenderSceneCommand>(
                command.payload);
        const bool isResize =
            std::holds_alternative<ResizeRenderViewCommand>(
                command.payload);
        const auto* const quality =
            std::get_if<ChangeRenderQualityCommand>(
                &command.payload);
        if (m_lastViewEpoch != 0
            && ((!isResize
                    && command.viewEpoch.value != m_lastViewEpoch)
                || (isResize
                    && command.viewEpoch.value < m_lastViewEpoch)))
        {
            throw std::invalid_argument(
                "Render control used a stale or unannounced view epoch.");
        }
        if (quality != nullptr
            && m_lastSettingsRevision != 0
            && quality->settingsRevision
                <= m_lastSettingsRevision)
        {
            throw std::invalid_argument(
                "Render quality revision must advance monotonically.");
        }

        m_lastControlCommandId = commandId;
        if (command.boundary == RenderControlBoundary::BeforeFrame)
        {
            m_pendingBeforeFrameId = targetFrame;
        }
        m_lastViewEpoch = command.viewEpoch.value;
        if (quality != nullptr)
        {
            m_lastSettingsRevision = quality->settingsRevision;
        }
        if (isSceneSwitch)
        {
            m_lastSceneEpoch =
                std::get<SwitchRenderSceneCommand>(command.payload)
                    .nextSceneEpoch.value;
        }
        else
        {
            m_lastSceneEpoch = command.sceneEpoch.value;
        }
    }

    const std::uint64_t acceptanceId = m_nextAcceptanceId++;
    const std::size_t waitingDepthAtAcceptance = m_entries.size() + 1;
    m_entries.push_back({
        acceptanceId,
        SteadyClockNanoseconds(),
        waitingDepthAtAcceptance,
        std::move(payload)});
    ++m_acceptedCount;
    m_peakWaitingCount = (std::max)(
        m_peakWaitingCount,
        m_entries.size());
    return acceptanceId;
}
} // namespace Prism::Core
