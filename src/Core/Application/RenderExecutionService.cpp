#include "Core/Application/RenderExecutionService.h"

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

[[nodiscard]] double NanosecondsToMilliseconds(
    const std::uint64_t nanoseconds) noexcept
{
    return static_cast<double>(nanoseconds) / 1'000'000.0;
}
} // namespace

const RenderViewRuntimeFeedback* RenderFrameFeedback::FindView(
    const Scene::RenderViewId viewId) const noexcept
{
    for (const RenderViewRuntimeFeedback& view : views)
    {
        if (view.viewId == viewId)
        {
            return &view;
        }
    }
    return nullptr;
}

RenderExecutionMode ParseRenderExecutionMode(
    const std::string_view value)
{
    if (value.empty() || value == "threaded")
    {
        return RenderExecutionMode::Threaded;
    }
    if (value == "inline")
    {
        return RenderExecutionMode::Inline;
    }
    throw std::invalid_argument(
        "Render execution mode must be inline or threaded.");
}

std::string_view ToString(const RenderExecutionMode mode)
{
    switch (mode)
    {
    case RenderExecutionMode::Inline: return "inline";
    case RenderExecutionMode::Threaded: return "threaded";
    }
    return "unknown";
}

RenderExecutionService::RenderExecutionService(
    const RenderExecutionMode mode,
    std::unique_ptr<IRenderExecutionTarget> target,
    const std::size_t waitingCapacity)
    : m_mode(mode),
      m_queue(waitingCapacity),
      m_target(std::move(target)),
      m_frameCompletionCapacity(waitingCapacity + 2)
{
    if (!m_target)
    {
        throw std::invalid_argument(
            "RenderExecutionService requires an execution target.");
    }
    if (m_mode == RenderExecutionMode::Threaded)
    {
        try
        {
            m_thread = std::thread([this] { ThreadMain(); });
        }
        catch (...)
        {
            ShutdownTarget();
            throw;
        }
        std::unique_lock lock(m_stateMutex);
        m_completion.wait(lock, [this]
        {
            return m_initializationComplete;
        });
        const std::exception_ptr failure = m_failure;
        lock.unlock();
        if (failure)
        {
            if (m_thread.joinable())
            {
                m_thread.join();
            }
            std::rethrow_exception(failure);
        }
    }
    else
    {
        try
        {
            m_initializationAttempted = true;
            m_target->Initialize();
            m_initializationComplete = true;
        }
        catch (...)
        {
            const std::exception_ptr failure = std::current_exception();
            m_initializationComplete = true;
            m_queue.Fail(failure);
            RecordFailure(failure);
            ShutdownTarget();
            std::rethrow_exception(failure);
        }
    }
}

RenderExecutionService::~RenderExecutionService()
{
    Shutdown();
}

std::uint64_t RenderExecutionService::SubmitFrame(
    FrameEnvelope frame)
{
    if (frame.producerInputTimestampNanoseconds == 0)
    {
        frame.producerInputTimestampNanoseconds =
            SteadyClockNanoseconds();
    }
    return SubmitPayload(std::move(frame));
}

RenderControlTicket RenderExecutionService::SubmitControl(
    RenderControlCommand command)
{
    const RenderControlCommandId commandId = command.id;
    auto completion =
        std::make_shared<RenderControlCompletionState>();
    const std::uint64_t acceptanceId = SubmitPayload(
        QueuedRenderControl{
            std::move(command),
            completion});
    return RenderControlTicket(
        acceptanceId,
        commandId,
        std::move(completion));
}

std::uint64_t RenderExecutionService::SubmitPayload(
    RenderExecutionPayload payload)
{
    // Serializes the inline consumer and the one-time inline -> threaded lane
    // handoff. Threaded producers remain short-lived here: only queue
    // acceptance is serialized, never execution or completion waiting.
    std::lock_guard submissionLock(m_inlineMutex);
    RenderExecutionMode mode = RenderExecutionMode::Inline;
    {
        std::lock_guard lock(m_stateMutex);
        if (m_shutdownStarted)
        {
            throw std::runtime_error(
                "RenderExecutionService is shutting down.");
        }
        mode = m_mode;
    }
    if (mode == RenderExecutionMode::Inline)
    {
        const std::uint64_t acceptanceId = std::visit(
            [this](auto&& value)
            {
                return m_queue.Push(std::move(value));
            }, std::move(payload));
        RunInlineEntry();
        RethrowFailure();
        return acceptanceId;
    }

    return std::visit([this](auto&& value)
    {
        return m_queue.Push(std::move(value));
    }, std::move(payload));
}

void RenderExecutionService::StartThreadedExecution()
{
    std::lock_guard lifecycleLock(m_lifecycleMutex);
    std::lock_guard submissionLock(m_inlineMutex);
    {
        std::lock_guard lock(m_stateMutex);
        if (m_shutdownStarted)
        {
            throw std::logic_error(
                "Cannot migrate a shutting-down render execution service.");
        }
        if (m_mode == RenderExecutionMode::Threaded)
        {
            return;
        }
    }

    const RenderFrameQueueStatistics statistics =
        m_queue.GetStatistics();
    if (statistics.waitingCount != 0
        || statistics.acceptedCount != statistics.consumedCount)
    {
        throw std::logic_error(
            "Render execution lane migration requires an empty queue.");
    }

    {
        std::lock_guard lock(m_stateMutex);
        m_mode = RenderExecutionMode::Threaded;
        m_adoptInitializedTarget = true;
        m_initializationComplete = false;
        m_executionStopped = false;
    }
    try
    {
        m_thread = std::thread([this] { ThreadMain(); });
    }
    catch (...)
    {
        std::lock_guard lock(m_stateMutex);
        m_mode = RenderExecutionMode::Inline;
        m_adoptInitializedTarget = false;
        m_initializationComplete = true;
        throw;
    }

    std::unique_lock lock(m_stateMutex);
    m_completion.wait(lock, [this]
    {
        return m_initializationComplete;
    });
    const std::exception_ptr failure = m_failure;
    lock.unlock();
    if (failure)
    {
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        std::rethrow_exception(failure);
    }
}

void RenderExecutionService::WaitUntilCompleted(
    const std::uint64_t acceptanceId)
{
    if (acceptanceId == 0)
    {
        throw std::invalid_argument(
            "Render completion requires a non-zero acceptance ID.");
    }
    std::unique_lock lock(m_stateMutex);
    m_completion.wait(lock, [this, acceptanceId]
    {
        return m_completedAcceptanceId >= acceptanceId
            || m_failure
            || m_executionStopped;
    });
    const std::exception_ptr failure = m_failure;
    if (failure)
    {
        lock.unlock();
        std::rethrow_exception(failure);
    }
    if (m_completedAcceptanceId < acceptanceId)
    {
        throw std::runtime_error(
            "Render execution stopped before the requested work completed.");
    }
}

std::vector<RenderFrameCompletion>
RenderExecutionService::ConsumeFrameCompletions()
{
    std::lock_guard lock(m_stateMutex);
    std::vector<RenderFrameCompletion> result;
    result.reserve(m_frameCompletions.size());
    while (!m_frameCompletions.empty())
    {
        result.push_back(std::move(m_frameCompletions.front()));
        m_frameCompletions.pop_front();
    }
    return result;
}

std::optional<RenderFrameFeedback>
RenderExecutionService::ConsumeLatestFrameFeedback()
{
    std::lock_guard lock(m_stateMutex);
    std::optional<RenderFrameFeedback> result =
        std::move(m_latestFrameFeedback);
    m_latestFrameFeedback.reset();
    return result;
}

void RenderExecutionService::Shutdown() noexcept
{
    std::lock_guard lifecycleLock(m_lifecycleMutex);
    {
        std::unique_lock lock(m_stateMutex);
        if (m_shutdownStarted)
        {
            m_completion.wait(lock, [this]
            {
                return m_shutdownComplete;
            });
            return;
        }
        m_shutdownStarted = true;
    }
    m_queue.Close();
    m_completion.notify_all();

    if (m_mode == RenderExecutionMode::Threaded)
    {
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }
    else
    {
        try
        {
            while (std::optional<RenderFrameQueueEntry> entry =
                       m_queue.WaitPop())
            {
                ExecuteEntry(std::move(*entry));
            }
        }
        catch (...)
        {
            const std::exception_ptr failure =
                std::current_exception();
            m_queue.Fail(failure);
            RecordFailure(failure);
        }
        ShutdownTarget();
        {
            std::lock_guard lock(m_stateMutex);
            m_executionStopped = true;
        }
        m_completion.notify_all();
    }
    {
        std::lock_guard lock(m_stateMutex);
        m_shutdownComplete = true;
    }
    m_completion.notify_all();
}

void RenderExecutionService::RethrowFailure() const
{
    std::exception_ptr failure;
    {
        std::lock_guard lock(m_stateMutex);
        failure = m_failure;
    }
    if (failure)
    {
        std::rethrow_exception(failure);
    }
    m_queue.RethrowFailure();
}

RenderExecutionMode RenderExecutionService::GetMode() const noexcept
{
    std::lock_guard lock(m_stateMutex);
    return m_mode;
}

RenderFrameQueueStatistics
RenderExecutionService::GetQueueStatistics() const noexcept
{
    return m_queue.GetStatistics();
}

void RenderExecutionService::ExecuteEntry(RenderFrameQueueEntry entry)
{
    const std::uint64_t executionStartNanoseconds =
        SteadyClockNanoseconds();
    std::optional<RenderFrameCompletion> frameCompletion;
    RenderFramePipelineDiagnostics pipeline{};
    pipeline.waitingDepthAtAcceptance =
        entry.waitingDepthAtAcceptance;
    pipeline.queueWaitCpuMs = NanosecondsToMilliseconds(
        executionStartNanoseconds
            >= entry.acceptedTimestampNanoseconds
        ? executionStartNanoseconds
            - entry.acceptedTimestampNanoseconds
        : 0);
    RenderTargetFrameResult targetFrameResult{};
    std::visit([this, &targetFrameResult](const auto& value)
    {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, FrameEnvelope>)
        {
            targetFrameResult = m_target->ExecuteFrame(value);
            m_lastExecutedLogicalFrameId =
                value.frame->GetLogicalFrameId().value;
        }
        else
        {
            try
            {
                const std::uint64_t targetFrame =
                    value.command.targetFrame.value;
                if ((value.command.boundary
                            == RenderControlBoundary::BeforeFrame
                        && targetFrame
                            <= m_lastExecutedLogicalFrameId)
                    || (value.command.boundary
                            == RenderControlBoundary::AfterFrame
                        && targetFrame
                            != m_lastExecutedLogicalFrameId))
                {
                    throw std::runtime_error(
                        "Render control executed at the wrong frame boundary.");
                }
                RenderControlAcknowledgement acknowledgement =
                    m_target->ExecuteControl(value.command);
                if (acknowledgement.commandId
                        != value.command.id
                    || acknowledgement.sceneEpoch
                        != value.command.sceneEpoch
                    || acknowledgement.viewEpoch
                        != value.command.viewEpoch)
                {
                    throw std::runtime_error(
                        "Render control acknowledgement identity mismatch.");
                }
                value.completion->Complete(
                    std::move(acknowledgement));
                if (std::holds_alternative<
                        StopRenderExecutionCommand>(
                            value.command.payload))
                {
                    // Stop accepting after this ordered boundary, but drain
                    // every item that was already accepted ahead of Close().
                    m_queue.Close();
                }
            }
            catch (...)
            {
                value.completion->Fail(std::current_exception());
                throw;
            }
        }
    }, entry.payload);

    if (const auto* frame =
            std::get_if<FrameEnvelope>(&entry.payload))
    {
        const std::shared_ptr<const Scene::RenderSceneData>& sceneData =
            frame->frame->GetSceneData();
        frameCompletion = RenderFrameCompletion{
            entry.acceptanceId,
            frame->frame->GetLogicalFrameId(),
            sceneData->GetSceneGeneration(),
            sceneData->GetDataRevision(),
            frame->sceneEpoch,
            frame->viewEpoch,
            frame->settingsRevision,
            targetFrameResult.captureRequestId,
            targetFrameResult.captureResolved,
            std::move(targetFrameResult.captureError)};
        if (targetFrameResult.presentTimestampNanoseconds
                >= frame->producerInputTimestampNanoseconds)
        {
            pipeline.inputToPresentCpuMs =
                NanosecondsToMilliseconds(
                    targetFrameResult.presentTimestampNanoseconds
                    - frame->producerInputTimestampNanoseconds);
        }
        pipeline.peakWaitingDepth =
            m_queue.GetStatistics().peakWaitingCount;
    }

    // Completion means the execution service no longer retains producer
    // inputs. ExecuteEntry itself outlives the completion notification, so
    // release the consumed packet before publishing the acceptance ID.
    if (auto* frame = std::get_if<FrameEnvelope>(&entry.payload))
    {
        frame->frame.reset();
        frame->uiDrawPacket.reset();
        frame->views.clear();
    }

    {
        std::lock_guard lock(m_stateMutex);
        if (frameCompletion.has_value())
        {
            if (m_frameCompletions.size()
                >= m_frameCompletionCapacity)
            {
                throw std::runtime_error(
                    "Render frame completion feedback exceeded its bounded capacity.");
            }
            m_frameCompletions.push_back(
                *frameCompletion);
            PublishFrameFeedbackLocked(
                *frameCompletion,
                pipeline,
                std::move(targetFrameResult));
        }
        m_completedAcceptanceId = entry.acceptanceId;
    }
    m_completion.notify_all();
}

void RenderExecutionService::PublishFrameFeedbackLocked(
    const RenderFrameCompletion& completion,
    RenderFramePipelineDiagnostics pipeline,
    RenderTargetFrameResult result)
{
    if (completion.acceptanceId == 0
        || !completion.logicalFrameId
        || !completion.sceneGeneration
        || !completion.dataRevision)
    {
        throw std::invalid_argument(
            "Render feedback requires complete frame identity.");
    }

    RenderFrameFeedback feedback{};
    feedback.acceptanceId = completion.acceptanceId;
    feedback.logicalFrameId = completion.logicalFrameId;
    feedback.sceneGeneration = completion.sceneGeneration;
    feedback.dataRevision = completion.dataRevision;
    feedback.sceneEpoch = completion.sceneEpoch;
    feedback.viewEpoch = completion.viewEpoch;
    feedback.settingsRevision = completion.settingsRevision;
    feedback.pipeline = pipeline;
    feedback.graphicsApi = result.graphicsApi;
    feedback.executionThreadId = result.executionThreadId;
    feedback.timings = result.timings;
    feedback.taskStatisticsAtFrameStart =
        result.taskStatisticsAtFrameStart;
    feedback.taskStatisticsAtFrameEnd =
        result.taskStatisticsAtFrameEnd;
    feedback.framePacingStatistics =
        std::move(result.framePacingStatistics);
    feedback.frameAdmission = std::move(result.frameAdmission);
    feedback.framePacingState = std::move(result.framePacingState);
    feedback.deviceStatistics =
        std::move(result.deviceStatistics);
    feedback.renderGraphReport =
        std::move(result.renderGraphReport);
    feedback.gpuTimingCaptureMetadata =
        std::move(result.gpuTimingCaptureMetadata);
    feedback.frameContextIndex = result.frameContextIndex;
    feedback.views.reserve(result.views.size());
    for (RenderViewRuntimeFeedback& view : result.views)
    {
        if (!view.viewId || feedback.FindView(view.viewId) != nullptr)
        {
            throw std::invalid_argument(
                "Render feedback contains a zero or duplicate view ID.");
        }
        if (view.visibilityFeedback != nullptr)
        {
            const Scene::RenderViewFeedbackIdentity& identity =
                view.visibilityFeedback->GetIdentity();
            if (identity.sceneGeneration
                    != completion.sceneGeneration
                || identity.dataRevision
                    != completion.dataRevision
                || identity.viewId != view.viewId
                || identity.logicalFrameId.value
                    > completion.logicalFrameId.value)
            {
                view.visibilityFeedback.reset();
                ++feedback.rejectedVisibilityFeedbackCount;
            }
        }
        feedback.views.push_back(std::move(view));
    }
    m_latestFrameFeedback = std::move(feedback);
}

void RenderExecutionService::RunInlineEntry()
{
    try
    {
        std::optional<RenderFrameQueueEntry> entry = m_queue.WaitPop();
        if (!entry)
        {
            throw std::runtime_error(
                "Inline render execution lost accepted work.");
        }
        ExecuteEntry(std::move(*entry));
    }
    catch (...)
    {
        const std::exception_ptr failure = std::current_exception();
        m_queue.Fail(failure);
        RecordFailure(failure);
    }
}

void RenderExecutionService::ThreadMain() noexcept
{
    try
    {
        bool adoptInitializedTarget = false;
        {
            std::lock_guard lock(m_stateMutex);
            adoptInitializedTarget = m_adoptInitializedTarget;
            if (!adoptInitializedTarget)
            {
                m_initializationAttempted = true;
            }
        }
        if (adoptInitializedTarget)
        {
            m_target->AdoptExecutionLane();
        }
        else
        {
            m_target->Initialize();
        }
        {
            std::lock_guard lock(m_stateMutex);
            m_initializationComplete = true;
        }
        m_completion.notify_all();
        for (;;)
        {
            std::optional<RenderFrameQueueEntry> entry =
                m_queue.WaitPop();
            if (!entry)
            {
                break;
            }
            ExecuteEntry(std::move(*entry));
        }
    }
    catch (...)
    {
        const std::exception_ptr failure = std::current_exception();
        m_queue.Fail(failure);
        RecordFailure(failure);
        {
            std::lock_guard lock(m_stateMutex);
            m_initializationComplete = true;
        }
        m_completion.notify_all();
    }
    ShutdownTarget();
    {
        std::lock_guard lock(m_stateMutex);
        m_executionStopped = true;
    }
    m_completion.notify_all();
}

void RenderExecutionService::RecordFailure(
    std::exception_ptr failure) noexcept
{
    {
        std::lock_guard lock(m_stateMutex);
        if (!m_failure)
        {
            m_failure = std::move(failure);
        }
    }
    m_completion.notify_all();
}

void RenderExecutionService::ShutdownTarget() noexcept
{
    {
        std::lock_guard lock(m_stateMutex);
        if (m_targetShutdown || !m_initializationAttempted)
        {
            return;
        }
        m_targetShutdown = true;
    }
    m_target->Shutdown();
}
} // namespace Prism::Core
