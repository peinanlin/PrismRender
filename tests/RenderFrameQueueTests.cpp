#include "Core/Application/CaptureAutomationController.h"
#include "Core/Application/RenderExecutionService.h"
#include "Core/Application/RenderFrameExecutionState.h"
#include "Scene/RenderSceneData.h"
#include "Scene/DemoSceneCatalog.h"

#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
static_assert(std::is_move_constructible_v<Prism::Core::FrameEnvelope>);

void Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <typename Callback>
void Reject(Callback&& callback)
{
    bool rejected = false;
    try
    {
        callback();
    }
    catch (const std::exception&)
    {
        rejected = true;
    }
    Expect(rejected, "Invalid render execution input was accepted.");
}

void SetEnvironmentValue(
    const char* const name,
    const char* const value)
{
#if defined(_WIN32)
    if (_putenv_s(name, value) != 0)
    {
        throw std::runtime_error("Failed to update test environment.");
    }
#else
    const int result = value[0] == '\0'
        ? unsetenv(name)
        : setenv(name, value, 1);
    if (result != 0)
    {
        throw std::runtime_error("Failed to update test environment.");
    }
#endif
}

void ClearCaptureAutomationEnvironment()
{
    for (const char* const name : {
             "PRISM_RENDER_CAPTURE_PATH",
             "PRISM_RENDER_CAPTURE_DELAY_FRAMES",
             "PRISM_RENDER_CAPTURE_DIAGNOSTICS_PATH",
             "PRISM_RENDER_FRAME_DIAGNOSTICS_PATH",
             "PRISM_RENDER_CAPTURE_SEQUENCE_PATH",
             "PRISM_RENDER_CAPTURE_SEQUENCE_OUTPUT",
             "PRISM_RENDER_EXIT_AFTER_CAPTURE",
             "PRISM_RENDER_RDG_REPORT_PATH",
             "PRISM_RENDER_GPU_TIMING_REPORT_PATH",
             "PRISM_RENDER_EXIT_AFTER_RDG_REPORT",
             "PRISM_RENDER_ASSET_STREAMING_REPORT_PATH",
             "PRISM_RENDER_ASSET_STREAMING_SCENE_REPORT_PATH",
             "PRISM_RENDER_MAX_FRAMES",
             "PRISM_RENDER_WATER_VALIDATION_SEQUENCE"})
    {
        SetEnvironmentValue(name, "");
    }
}

Prism::Core::FrameEnvelope MakeFrame(
    const std::uint64_t frameId,
    const double cameraX = 0.0,
    const bool active = true,
    const std::uint64_t settingsRevision = 1,
    const double simulationDeltaSeconds = 1.0 / 60.0)
{
    using namespace Prism::Scene;
    auto scene = std::make_shared<const RenderSceneData>(
        SceneGeneration{1},
        RenderSceneDataRevision{1},
        std::vector<RenderObject>{},
        std::vector<RenderSceneObjectMetadata>{},
        std::vector<RenderSceneObjectAssetBindings>{});
    RenderView view{};
    view.id = RenderViewId{1};
    view.width = 1280;
    view.height = 800;
    view.camera.SetWorldPosition({cameraX, 0.0, -5.0});
    view.previousCamera.SetWorldPosition({999.0, 0.0, -5.0});
    view.previousCameraValid = true;
    auto packet = std::make_shared<const RenderFramePacket>(
        LogicalFrameId{frameId},
        static_cast<double>(frameId) / 60.0,
        std::move(scene),
        RenderFrameDynamicData{},
        std::vector<RenderView>{view},
        simulationDeltaSeconds);
    Prism::Renderer::RenderSettings settings{};
    settings.exposure = static_cast<float>(cameraX + 1.0);
    return {
        std::move(packet),
        Prism::Core::RenderEpoch{1},
        Prism::Core::RenderEpoch{1},
        settingsRevision,
        std::vector<Prism::Core::RenderViewFrameState>{
            {RenderViewId{1}, settings, active}}};
}

Prism::Core::RenderControlCommand MakeControl(
    const std::uint64_t commandId,
    const std::uint64_t targetFrame)
{
    Prism::Core::RenderControlCommand command{};
    command.id = Prism::Core::RenderControlCommandId{commandId};
    command.targetFrame = Prism::Scene::LogicalFrameId{targetFrame};
    command.sceneEpoch = Prism::Core::RenderEpoch{1};
    command.viewEpoch = Prism::Core::RenderEpoch{1};
    command.boundary = Prism::Core::RenderControlBoundary::AfterFrame;
    command.payload = Prism::Core::ResetRenderHistoryCommand{
        Prism::Scene::RenderViewId{1}};
    return command;
}

Prism::Core::RenderControlCommand MakeCaptureControl(
    const std::uint64_t commandId,
    const std::uint64_t targetFrame,
    const std::uint64_t requestId)
{
    Prism::Core::RenderControlCommand command{};
    command.id = Prism::Core::RenderControlCommandId{commandId};
    command.targetFrame = Prism::Scene::LogicalFrameId{targetFrame};
    command.sceneEpoch = Prism::Core::RenderEpoch{1};
    command.viewEpoch = Prism::Core::RenderEpoch{1};
    command.boundary = Prism::Core::RenderControlBoundary::BeforeFrame;
    command.payload = Prism::Core::CaptureRenderFrameCommand{
        requestId,
        "capture.bmp"};
    return command;
}

Prism::Core::RenderControlCommand MakeStopControl(
    const std::uint64_t commandId,
    const std::uint64_t targetFrame)
{
    Prism::Core::RenderControlCommand command =
        MakeControl(commandId, targetFrame);
    command.payload = Prism::Core::StopRenderExecutionCommand{};
    return command;
}

struct TargetState
{
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::string> order;
    std::vector<std::thread::id> threads;
    std::size_t shutdownCount = 0;
    bool initializeCalled = false;
    std::size_t executionLaneAdoptionCount = 0;
    bool failInitialize = false;
    bool failFirstFrame = false;
    bool failFirstControl = false;
    bool rejectFirstControl = false;
    bool mismatchFirstControl = false;
    bool blockFirstFrame = false;
    bool firstFrameEntered = false;
    bool releaseFirstFrame = false;
    std::uint64_t activeCaptureRequestId = 0;
    std::size_t capturePendingFrames = 0;
    std::uint32_t visibilityMismatch = 0;
    Prism::Core::ProfilingLevel lastProfilingLevel =
        Prism::Core::ProfilingLevel::Basic;
    bool lastFramePacingEnabled = false;
};

class RecordingTarget final
    : public Prism::Core::IRenderExecutionTarget
{
public:
    explicit RecordingTarget(std::shared_ptr<TargetState> state)
        : m_state(std::move(state))
    {
    }

    void Initialize() override
    {
        std::lock_guard lock(m_state->mutex);
        m_state->initializeCalled = true;
        m_state->order.push_back("initialize");
        m_state->threads.push_back(std::this_thread::get_id());
        if (m_state->failInitialize)
        {
            throw std::runtime_error("injected initialization failure");
        }
    }

    void AdoptExecutionLane() override
    {
        std::lock_guard lock(m_state->mutex);
        ++m_state->executionLaneAdoptionCount;
        m_state->order.push_back("adopt-lane");
        m_state->threads.push_back(std::this_thread::get_id());
    }

    [[nodiscard]] Prism::Core::RenderTargetFrameResult ExecuteFrame(
        const Prism::Core::FrameEnvelope& frame) override
    {
        const std::uint64_t frameId =
            frame.frame->GetLogicalFrameId().value;
        std::uint64_t captureRequestId = 0;
        bool captureResolved = false;
        {
            std::unique_lock lock(m_state->mutex);
            m_state->order.push_back("frame:" +
                std::to_string(frameId));
            m_state->threads.push_back(std::this_thread::get_id());
            m_state->lastProfilingLevel = frame.profilingLevel;
            m_state->lastFramePacingEnabled =
                frame.framePacingEnabled;
            if (m_state->blockFirstFrame && frameId == 1)
            {
                m_state->firstFrameEntered = true;
                m_state->condition.notify_all();
                m_state->condition.wait(lock, [this]
                {
                    return m_state->releaseFirstFrame;
                });
            }
            captureRequestId = m_state->activeCaptureRequestId;
            if (captureRequestId != 0
                && m_state->capturePendingFrames > 0)
            {
                --m_state->capturePendingFrames;
            }
            else
            {
                m_state->activeCaptureRequestId = 0;
                captureResolved = captureRequestId != 0;
            }
        }
        if (m_state->failFirstFrame && frameId == 1)
        {
            throw std::runtime_error("injected render failure");
        }
        Prism::Core::RenderTargetFrameResult result{
            captureRequestId,
            captureResolved,
            {}};
        result.frameContextIndex =
            static_cast<std::uint32_t>(frameId % 3);
        result.taskStatisticsAtFrameStart.executorName = "pool";
        result.taskStatisticsAtFrameStart
            .cumulativeExecutionNanoseconds = frameId * 100;
        result.taskStatisticsAtFrameEnd.executorName = "pool";
        result.taskStatisticsAtFrameEnd
            .cumulativeExecutionNanoseconds = frameId * 100 + 25;
        Prism::RHI::FramePacingStatistics pacing{};
        pacing.generation = frameId;
        result.framePacingStatistics = pacing;
        Prism::Renderer::RendererStatistics statistics{};
        statistics.drawCalls = static_cast<std::uint32_t>(frameId);
        result.views.push_back({
            Prism::Scene::GameRenderViewId,
            statistics,
            std::make_shared<const Prism::Scene::RenderViewFeedback>(
                Prism::Scene::RenderViewFeedbackIdentity{
                    Prism::Scene::SceneGeneration{
                        m_state->visibilityMismatch == 1 ? 2u : 1u},
                    Prism::Scene::RenderSceneDataRevision{
                        m_state->visibilityMismatch == 2 ? 2u : 1u},
                    m_state->visibilityMismatch == 3
                        ? Prism::Scene::SceneRenderViewId
                        : Prism::Scene::GameRenderViewId,
                    Prism::Scene::LogicalFrameId{
                        frameId
                            + (m_state->visibilityMismatch == 4
                                ? 1u : 0u)}},
                std::vector<Prism::Scene::GpuVisibilityReason>{},
                Prism::Scene::Camera{})});
        return result;
    }

    [[nodiscard]] Prism::Core::RenderControlAcknowledgement ExecuteControl(
        const Prism::Core::RenderControlCommand& command) override
    {
        std::lock_guard lock(m_state->mutex);
        m_state->order.push_back("control:" +
            std::to_string(command.id.value));
        m_state->threads.push_back(std::this_thread::get_id());
        if (m_state->failFirstControl)
        {
            throw std::runtime_error("injected control failure");
        }
        if (const auto* capture =
                std::get_if<Prism::Core::CaptureRenderFrameCommand>(
                    &command.payload))
        {
            m_state->activeCaptureRequestId = capture->requestId;
        }
        const bool drain = std::holds_alternative<
            Prism::Core::DrainRenderExecutionCommand>(command.payload);
        const bool assetWork = std::holds_alternative<
            Prism::Core::ProcessAssetRenderWorkCommand>(command.payload);
        const bool streamingReport = std::holds_alternative<
            Prism::Core::WriteAssetStreamingReportCommand>(command.payload);
        std::optional<Prism::Asset::AssetImportResult> importResult;
        if (std::holds_alternative<
                Prism::Core::ImportRenderAssetCommand>(command.payload))
        {
            importResult.emplace();
            importResult->success = true;
        }
        return {
            m_state->mismatchFirstControl
                ? Prism::Core::RenderControlCommandId{
                    command.id.value + 1}
                : command.id,
            command.targetFrame,
            command.sceneEpoch,
            command.viewEpoch,
            !m_state->rejectFirstControl,
            drain,
            assetWork ? 2u : 0u,
            assetWork ? 1u : 0u,
            assetWork ? 3u : 0u,
            streamingReport,
            false,
            std::move(importResult)};
    }

    void Shutdown() noexcept override
    {
        std::lock_guard lock(m_state->mutex);
        m_state->order.push_back("shutdown");
        m_state->threads.push_back(std::this_thread::get_id());
        ++m_state->shutdownCount;
    }

private:
    std::shared_ptr<TargetState> m_state;
};

struct PipelineLatchState
{
    class Latch
    {
    public:
        void CountDown()
        {
            {
                std::lock_guard lock(m_mutex);
                m_ready = true;
            }
            m_condition.notify_all();
        }

        void Wait()
        {
            std::unique_lock lock(m_mutex);
            m_condition.wait(lock, [this] { return m_ready; });
        }

    private:
        std::mutex m_mutex;
        std::condition_variable m_condition;
        bool m_ready = false;
    };

    Latch renderFrameEntered;
    Latch releaseRenderFrame;
    std::atomic<bool> firstRenderCompleted = false;
    std::mutex mutex;
    std::vector<std::uint64_t> executedFrames;
};

class PipelineLatchTarget final
    : public Prism::Core::IRenderExecutionTarget
{
public:
    explicit PipelineLatchTarget(
        std::shared_ptr<PipelineLatchState> state)
        : m_state(std::move(state))
    {
    }

    void Initialize() override {}

    [[nodiscard]] Prism::Core::RenderTargetFrameResult ExecuteFrame(
        const Prism::Core::FrameEnvelope& frame) override
    {
        const std::uint64_t frameId =
            frame.frame->GetLogicalFrameId().value;
        if (frameId == 1)
        {
            m_state->renderFrameEntered.CountDown();
            m_state->releaseRenderFrame.Wait();
        }
        {
            std::lock_guard lock(m_state->mutex);
            m_state->executedFrames.push_back(frameId);
        }
        if (frameId == 1)
        {
            m_state->firstRenderCompleted.store(
                true,
                std::memory_order_release);
        }
        Prism::Core::RenderTargetFrameResult result{};
        result.executionThreadId = static_cast<std::uint64_t>(
            std::hash<std::thread::id>{}(
                std::this_thread::get_id()));
        result.presentTimestampNanoseconds =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()).count());
        return result;
    }

    [[nodiscard]] Prism::Core::RenderControlAcknowledgement
        ExecuteControl(
            const Prism::Core::RenderControlCommand&) override
    {
        throw std::logic_error(
            "Pipeline latch target does not accept controls.");
    }

    void Shutdown() noexcept override {}

private:
    std::shared_ptr<PipelineLatchState> m_state;
};

void VerifyService(const Prism::Core::RenderExecutionMode mode)
{
    using namespace Prism::Core;
    const std::thread::id caller = std::this_thread::get_id();
    auto state = std::make_shared<TargetState>();
    RenderExecutionService service(
        mode,
        std::make_unique<RecordingTarget>(state));

    const std::uint64_t first = service.SubmitFrame(MakeFrame(1));
    const RenderControlTicket control = service.SubmitControl(
        MakeControl(1, 1));
    FrameEnvelope secondFrame = MakeFrame(2);
    secondFrame.profilingLevel = ProfilingLevel::Detailed;
    secondFrame.framePacingEnabled = true;
    const std::uint64_t second = service.SubmitFrame(
        std::move(secondFrame));
    Expect(first == 1 && control.GetAcceptanceId() == 2
            && second == 3,
        "Render acceptance IDs are not stable FIFO identities.");
    service.WaitUntilCompleted(second);
    const RenderControlAcknowledgement acknowledgement =
        control.Wait();
    Expect(acknowledgement.succeeded
            && acknowledgement.commandId.value == 1
            && acknowledgement.completedFrame.value == 1
            && acknowledgement.sceneEpoch.value == 1
            && acknowledgement.viewEpoch.value == 1,
        "Reliable render control acknowledgement lost its identity.");
    const std::vector<RenderFrameCompletion> completions =
        service.ConsumeFrameCompletions();
    Expect(
        completions.size() == 2
            && completions[0].acceptanceId == first
            && completions[0].logicalFrameId.value == 1
            && completions[1].acceptanceId == second
            && completions[1].logicalFrameId.value == 2
            && completions[0].sceneGeneration.value == 1
            && completions[0].dataRevision.value == 1
            && completions[0].sceneEpoch.value == 1
            && completions[0].viewEpoch.value == 1
            && completions[0].settingsRevision == 1
            && service.ConsumeFrameCompletions().empty(),
        "Completed-frame feedback lost accepted or rendered identity.");
    const std::optional<RenderFrameFeedback> latestFeedback =
        service.ConsumeLatestFrameFeedback();
    const RenderViewRuntimeFeedback* const gameFeedback =
        latestFeedback.has_value()
        ? latestFeedback->FindView(Prism::Scene::GameRenderViewId)
        : nullptr;
    Expect(latestFeedback.has_value()
            && latestFeedback->logicalFrameId.value == 2
            && gameFeedback != nullptr
            && gameFeedback->statistics.drawCalls == 2
            && gameFeedback->visibilityFeedback != nullptr
            && latestFeedback->taskStatisticsAtFrameStart
                    .cumulativeExecutionNanoseconds == 200
            && latestFeedback->taskStatisticsAtFrameEnd
                    .cumulativeExecutionNanoseconds == 225
            && latestFeedback->framePacingStatistics.has_value()
            && latestFeedback->framePacingStatistics->generation == 2
            && latestFeedback->frameContextIndex == 2
            && !service.ConsumeLatestFrameFeedback().has_value(),
        "Latest-only render feedback did not preserve frame diagnostics safely.");
    service.Shutdown();
    service.Shutdown();

    std::lock_guard lock(state->mutex);
    Expect(state->order == std::vector<std::string>{
            "initialize", "frame:1", "control:1", "frame:2",
            "shutdown"},
        "Inline and threaded execution did not share FIFO behavior.");
    Expect(state->shutdownCount == 1,
        "Render target shutdown was not exactly once.");
    Expect(state->initializeCalled && state->threads.size() == 5,
        "Render target execution trace is incomplete.");
    Expect(state->lastProfilingLevel == ProfilingLevel::Detailed
            && state->lastFramePacingEnabled,
        "Frozen frame diagnostics were not consumed by the execution target.");
    for (const std::thread::id thread : state->threads)
    {
        Expect((mode == RenderExecutionMode::Inline)
                ? thread == caller
                : thread != caller,
            "Render work executed on the wrong lane.");
    }
    const RenderFrameQueueStatistics stats =
        service.GetQueueStatistics();
    Expect(stats.acceptedCount == 3
            && stats.consumedCount == 3
            && stats.cancelledCount == 0
            && stats.waitingCount == 0
            && stats.waitingProducerCount == 0
            && stats.peakWaitingCount <= 1,
        "Render service queue statistics violate the bounded FIFO contract.");
}

void VerifyRenderLaneMaintenanceControls(
    const Prism::Core::RenderExecutionMode mode)
{
    using namespace Prism::Core;
    auto state = std::make_shared<TargetState>();
    RenderExecutionService service(
        mode,
        std::make_unique<RecordingTarget>(state));
    const std::uint64_t frame = service.SubmitFrame(MakeFrame(1));
    service.WaitUntilCompleted(frame);

    RenderControlCommand drain = MakeControl(1, 1);
    drain.payload = DrainRenderExecutionCommand{};
    const RenderControlAcknowledgement drainResult =
        service.SubmitControl(std::move(drain)).Wait();
    Expect(drainResult.succeeded && drainResult.gpuDrained,
        "GPU drain did not complete on the render execution lane.");

    RenderControlCommand assetWork = MakeControl(2, 1);
    assetWork.payload = ProcessAssetRenderWorkCommand{2};
    const RenderControlAcknowledgement assetResult =
        service.SubmitControl(std::move(assetWork)).Wait();
    Expect(assetResult.succeeded
            && !assetResult.gpuDrained
            && assetResult.uploadedAssetCount == 2
            && assetResult.evictedAssetCount == 1
            && assetResult.bindingUpdateCount == 3,
        "Asset render work acknowledgement lost its bounded result values.");

    RenderControlCommand report = MakeControl(3, 1);
    report.payload = WriteAssetStreamingReportCommand{
        "streaming-report.json"};
    const RenderControlAcknowledgement reportResult =
        service.SubmitControl(std::move(report)).Wait();
    Expect(reportResult.succeeded
            && reportResult.assetStreamingReportWritten,
        "Asset streaming report did not execute on the render lane.");

    RenderControlCommand assetImport = MakeControl(4, 1);
    assetImport.payload = ImportRenderAssetCommand{
        "asset.gltf",
        false};
    const RenderControlAcknowledgement importResult =
        service.SubmitControl(std::move(assetImport)).Wait();
    Expect(importResult.succeeded
            && importResult.assetImportResult.has_value()
            && importResult.assetImportResult->success,
        "Asset import result did not cross the reliable control boundary.");
    service.Shutdown();
}

void VerifyInlineStartupLaneHandoff()
{
    using namespace Prism::Core;
    const std::thread::id caller = std::this_thread::get_id();
    auto state = std::make_shared<TargetState>();
    RenderExecutionService service(
        RenderExecutionMode::Inline,
        std::make_unique<RecordingTarget>(state));

    const std::uint64_t inlineFrame =
        service.SubmitFrame(MakeFrame(1));
    service.WaitUntilCompleted(inlineFrame);
    service.StartThreadedExecution();
    service.StartThreadedExecution();
    Expect(service.GetMode() == RenderExecutionMode::Threaded,
        "Execution service did not freeze the selected threaded mode.");
    const std::uint64_t threadedFrame =
        service.SubmitFrame(MakeFrame(2));
    service.WaitUntilCompleted(threadedFrame);
    service.Shutdown();

    std::lock_guard lock(state->mutex);
    Expect(state->order == std::vector<std::string>{
            "initialize", "frame:1", "adopt-lane", "frame:2",
            "shutdown"},
        "Inline startup did not hand an empty service to one resident lane.");
    Expect(state->executionLaneAdoptionCount == 1
            && state->shutdownCount == 1
            && state->threads.size() == 5,
        "Execution lane handoff was repeated or incompletely traced.");
    Expect(state->threads[0] == caller && state->threads[1] == caller,
        "Inline startup unexpectedly left the caller lane.");
    Expect(state->threads[2] != caller
            && state->threads[2] == state->threads[3]
            && state->threads[3] == state->threads[4],
        "Adoption, threaded frame, and shutdown used different lanes.");
}

void VerifyCaptureCompletion(
    const Prism::Core::RenderExecutionMode mode)
{
    using namespace Prism::Core;
    auto state = std::make_shared<TargetState>();
    RenderExecutionService service(
        mode,
        std::make_unique<RecordingTarget>(state));
    (void)service.SubmitFrame(MakeFrame(1));
    const RenderControlTicket capture = service.SubmitControl(
        MakeCaptureControl(1, 2, 41));
    const std::uint64_t targetAcceptance =
        service.SubmitFrame(MakeFrame(2));
    service.WaitUntilCompleted(targetAcceptance);
    Expect(capture.Wait().succeeded,
        "Frame-bound capture command was rejected.");
    const std::vector<RenderFrameCompletion> completions =
        service.ConsumeFrameCompletions();
    Expect(completions.size() == 2
            && completions[0].captureRequestId == 0
            && completions[1].logicalFrameId.value == 2
            && completions[1].captureRequestId == 41
            && completions[1].captureResolved
            && completions[1].captureError.empty(),
        "Capture completion was not tied to the actual target frame.");
    service.Shutdown();
}

Prism::Core::RenderFrameCompletion MakeCompletion(
    const std::uint64_t acceptanceId,
    const std::uint64_t logicalFrameId,
    const std::uint64_t captureRequestId = 0,
    const bool captureResolved = false,
    std::string captureError = {})
{
    return {
        acceptanceId,
        Prism::Scene::LogicalFrameId{logicalFrameId},
        Prism::Scene::SceneGeneration{1},
        Prism::Scene::RenderSceneDataRevision{1},
        Prism::Core::RenderEpoch{1},
        Prism::Core::RenderEpoch{1},
        1,
        captureRequestId,
        captureResolved,
        std::move(captureError)};
}

void VerifyCaptureAutomationCompletionBinding()
{
    using namespace Prism::Core;
    ClearCaptureAutomationEnvironment();
    SetEnvironmentValue(
        "PRISM_RENDER_CAPTURE_PATH",
        "capture-automation-test.bmp");
    SetEnvironmentValue("PRISM_RENDER_EXIT_AFTER_CAPTURE", "1");
    SetEnvironmentValue("PRISM_RENDER_MAX_FRAMES", "3");

    CaptureAutomationController automation;
    automation.Initialize(
        Prism::Scene::DemoSceneId::EditorPreview,
        true,
        false);
    const auto request = automation.PrepareFrameCapture(1);
    Expect(request.has_value()
            && request->requestId == 1
            && request->targetLogicalFrameId == 1
            && automation.GetAcceptedFrameCount() == 1
            && automation.GetCompletedFrameCount() == 0
            && !automation.ShouldExit(),
        "Capture automation confused frame acceptance with completion.");

    Expect(!automation.PrepareFrameCapture(2).has_value()
            && automation.GetAcceptedFrameCount() == 2,
        "A queued frame unexpectedly created a second capture request.");
    const CaptureResolveResult pending =
        automation.ConsumeCompletedFrame(
            MakeCompletion(11, 1, request->requestId, false));
    Expect(!pending.complete
            && automation.GetCompletedFrameCount() == 1
            && !automation.ShouldExit(),
        "A pending readback was reported as a completed capture.");

    const CaptureResolveResult resolved =
        automation.ConsumeCompletedFrame(
            MakeCompletion(12, 2, request->requestId, true));
    Expect(resolved.complete
            && automation.GetCompletedFrameCount() == 2
            && automation.ShouldExit(),
        "Capture success was not bound to the actual completed frame.");
    Reject([&]
    {
        (void)automation.ConsumeCompletedFrame(
            MakeCompletion(10, 3));
    });

    ClearCaptureAutomationEnvironment();
    SetEnvironmentValue("PRISM_RENDER_MAX_FRAMES", "2");
    CaptureAutomationController maximumFrames;
    maximumFrames.Initialize(
        Prism::Scene::DemoSceneId::EditorPreview,
        true,
        false);
    (void)maximumFrames.PrepareFrameCapture(1);
    (void)maximumFrames.PrepareFrameCapture(2);
    (void)maximumFrames.ConsumeCompletedFrame(
        MakeCompletion(20, 1));
    Expect(!maximumFrames.ShouldExit(),
        "Max frames used accepted rather than completed frames.");
    (void)maximumFrames.ConsumeCompletedFrame(
        MakeCompletion(21, 2));
    Expect(maximumFrames.ShouldExit(),
        "Max frames did not observe the actual completion count.");
    ClearCaptureAutomationEnvironment();
}

void VerifyCaptureAcrossReliableEpochChange()
{
    using namespace Prism::Core;
    ClearCaptureAutomationEnvironment();
    SetEnvironmentValue(
        "PRISM_RENDER_CAPTURE_PATH",
        "capture-epoch-test.bmp");
    SetEnvironmentValue("PRISM_RENDER_CAPTURE_DELAY_FRAMES", "2");
    SetEnvironmentValue("PRISM_RENDER_EXIT_AFTER_CAPTURE", "1");

    CaptureAutomationController automation;
    automation.Initialize(
        Prism::Scene::DemoSceneId::EditorPreview,
        true,
        false);
    auto state = std::make_shared<TargetState>();
    state->capturePendingFrames = 1;
    RenderExecutionService service(
        RenderExecutionMode::Threaded,
        std::make_unique<RecordingTarget>(state));

    Expect(!automation.PrepareFrameCapture(1).has_value(),
        "Delayed capture was requested before its accepted frame.");
    (void)service.SubmitFrame(MakeFrame(1));

    const auto request = automation.PrepareFrameCapture(2);
    Expect(request.has_value()
            && request->targetLogicalFrameId == 2,
        "Delayed capture lost its accepted target frame.");
    RenderControlCommand resize = MakeControl(1, 2);
    resize.boundary = RenderControlBoundary::BeforeFrame;
    resize.viewEpoch = RenderEpoch{2};
    resize.payload = ResizeRenderViewCommand{
        Prism::Scene::GameRenderViewId, 960, 540};
    const RenderControlTicket resizeTicket =
        service.SubmitControl(resize);
    RenderControlCommand reset = MakeControl(2, 2);
    reset.boundary = RenderControlBoundary::BeforeFrame;
    reset.viewEpoch = RenderEpoch{2};
    const RenderControlTicket resetTicket =
        service.SubmitControl(reset);
    RenderControlCommand capture = MakeCaptureControl(
        3,
        2,
        request->requestId);
    capture.viewEpoch = RenderEpoch{2};
    capture.payload = CaptureRenderFrameCommand{
        request->requestId,
        request->outputPath};
    const RenderControlTicket captureTicket =
        service.SubmitControl(capture);
    FrameEnvelope second = MakeFrame(2);
    second.viewEpoch = RenderEpoch{2};
    (void)service.SubmitFrame(std::move(second));

    Expect(!automation.PrepareFrameCapture(3).has_value(),
        "Pending capture created a second request after resize/reset.");
    RenderControlCommand sceneSwitch = MakeControl(4, 3);
    sceneSwitch.boundary = RenderControlBoundary::BeforeFrame;
    sceneSwitch.viewEpoch = RenderEpoch{2};
    sceneSwitch.payload = SwitchRenderSceneCommand{RenderEpoch{2}};
    const RenderControlTicket switchTicket =
        service.SubmitControl(sceneSwitch);
    FrameEnvelope third = MakeFrame(3);
    third.sceneEpoch = RenderEpoch{2};
    third.viewEpoch = RenderEpoch{2};
    const std::uint64_t thirdAcceptance =
        service.SubmitFrame(std::move(third));
    service.WaitUntilCompleted(thirdAcceptance);
    Expect(resizeTicket.Wait().succeeded
            && resetTicket.Wait().succeeded
            && captureTicket.Wait().succeeded
            && switchTicket.Wait().succeeded,
        "Reliable controls around capture were not acknowledged.");

    const std::vector<RenderFrameCompletion> completions =
        service.ConsumeFrameCompletions();
    Expect(completions.size() == 3
            && completions[1].logicalFrameId.value == 2
            && completions[1].viewEpoch.value == 2
            && completions[1].captureRequestId
                == request->requestId
            && !completions[1].captureResolved
            && completions[2].sceneEpoch.value == 2
            && completions[2].captureRequestId
                == request->requestId
            && completions[2].captureResolved,
        "Capture completion lost resize/reset or old-epoch identity.");
    CaptureResolveResult resolved{};
    for (const RenderFrameCompletion& completion : completions)
    {
        resolved = automation.ConsumeCompletedFrame(completion);
    }
    Expect(resolved.complete
            && automation.GetCompletedFrameCount() == 3
            && automation.ShouldExit(),
        "Old-epoch completion was not attributed to its original request.");
    service.Shutdown();
    ClearCaptureAutomationEnvironment();
}
} // namespace

int main()
{
    using namespace Prism::Core;
    try
    {
        Expect(ParseRenderExecutionMode("")
                    == RenderExecutionMode::Threaded
                && ParseRenderExecutionMode("inline")
                    == RenderExecutionMode::Inline
                && ParseRenderExecutionMode("threaded")
                    == RenderExecutionMode::Threaded
                && ToString(RenderExecutionMode::Threaded)
                    == "threaded",
            "Render execution mode parsing is unstable.");
        Reject([] { (void)ParseRenderExecutionMode("parallel"); });
        FrameEnvelope invalidCapture = MakeFrame(1, 0.0, false);
        invalidCapture.captureViewId =
            Prism::Scene::GameRenderViewId;
        Reject([&invalidCapture]
        {
            ValidateFrameEnvelope(invalidCapture);
        });
        FrameEnvelope validCapture = MakeFrame(1);
        validCapture.captureViewId =
            Prism::Scene::GameRenderViewId;
        ValidateFrameEnvelope(validCapture);
        VerifyInlineStartupLaneHandoff();
        VerifyRenderLaneMaintenanceControls(
            RenderExecutionMode::Inline);
        VerifyRenderLaneMaintenanceControls(
            RenderExecutionMode::Threaded);
        VerifyCaptureCompletion(RenderExecutionMode::Inline);
        VerifyCaptureCompletion(RenderExecutionMode::Threaded);
        VerifyCaptureAutomationCompletionBinding();
        VerifyCaptureAcrossReliableEpochChange();

        RenderFrameExecutionState executionState;
        FrameEnvelope frozenFirst = MakeFrame(1, 10.0);
        PreparedRenderFrame preparedFirst =
            executionState.Prepare(frozenFirst);
        frozenFirst.views[0].settings.exposure = 999.0f;
        Expect(preparedFirst.views[0].settings.exposure == 11.0f
                && !preparedFirst.frame->FindView(
                    Prism::Scene::GameRenderViewId)
                        ->previousCameraValid,
            "Prepared frame values were not frozen from producer mutation.");
        executionState.Commit(preparedFirst);

        PreparedRenderFrame preparedSecond =
            executionState.Prepare(MakeFrame(2, 20.0));
        const Prism::Scene::RenderView* secondView =
            preparedSecond.frame->FindView(
                Prism::Scene::GameRenderViewId);
        Expect(secondView != nullptr
                && secondView->previousCameraValid
                && secondView->previousCamera
                    .GetWorldPosition().x == 10.0,
            "Motion history did not use the previous completed render frame.");
        executionState.Commit(preparedSecond);

        FrameEnvelope hiddenThird = MakeFrame(3, 30.0, false);
        PreparedRenderFrame preparedHidden =
            executionState.Prepare(hiddenThird);
        executionState.Commit(preparedHidden);
        PreparedRenderFrame preparedRestored =
            executionState.Prepare(MakeFrame(4, 40.0));
        const Prism::Scene::RenderView* restoredView =
            preparedRestored.frame->FindView(
                Prism::Scene::GameRenderViewId);
        Expect(restoredView != nullptr
                && restoredView->previousCameraValid
                && restoredView->previousCamera
                    .GetWorldPosition().x == 20.0,
            "A hidden view advanced camera history without rendering.");
        executionState.Commit(preparedRestored);

        executionState.InvalidateView(
            Prism::Scene::GameRenderViewId,
            Prism::Scene::RenderViewHistoryInvalidation::CameraCut);
        PreparedRenderFrame preparedReset =
            executionState.Prepare(MakeFrame(5, 50.0));
        Expect(!preparedReset.frame->FindView(
                    Prism::Scene::GameRenderViewId)
                    ->previousCameraValid,
            "Reliable history reset did not invalidate the target view.");
        executionState.Commit(preparedReset);
        Reject([&]
        {
            (void)executionState.Prepare(
                MakeFrame(6, 60.0, true, 1, 0.5));
        });

        RenderFrameQueue queue;
        const auto first = queue.TryPush(MakeFrame(1));
        const auto full = queue.TryPush(MakeFrame(2));
        Expect(first == 1 && !full,
            "Single waiting-frame capacity was not enforced.");
        auto firstEntry = queue.WaitPop();
        Expect(firstEntry && firstEntry->acceptanceId == 1,
            "First accepted frame was not consumed first.");
        const auto second = queue.TryPush(MakeFrame(2));
        Expect(second == 2,
            "Queue did not accept work after the consumer freed its slot.");
        auto secondEntry = queue.WaitPop();
        Expect(secondEntry && secondEntry->acceptanceId == 2,
            "Second accepted frame was not consumed exactly once.");
        Reject([&] { (void)queue.TryPush(MakeFrame(2)); });
        const auto command = queue.TryPush(MakeControl(1, 2));
        Expect(command == 3,
            "Control work did not share the ordered acceptance stream.");
        Expect(queue.WaitPop()->acceptanceId == 3,
            "Control work did not preserve queue order.");
        queue.Close();
        Expect(!queue.WaitPop().has_value(),
            "Closed queue did not finish draining.");
        Reject([&] { (void)queue.TryPush(MakeFrame(3)); });

        for (const std::size_t capacity : {1u, 2u})
        {
            RenderFrameQueue boundedQueue(capacity);
            for (std::size_t frame = 1; frame <= capacity; ++frame)
            {
                Expect(boundedQueue.TryPush(MakeFrame(frame)).has_value(),
                    "Configured frame queue rejected work below capacity.");
            }
            Expect(!boundedQueue.TryPush(MakeFrame(capacity + 1)).has_value(),
                "Configured frame queue exceeded its bounded depth.");
            const auto boundedStatistics = boundedQueue.GetStatistics();
            Expect(boundedStatistics.waitingCount == capacity
                    && boundedStatistics.peakWaitingCount == capacity
                    && boundedStatistics.capacity == capacity,
                "Configured frame queue depth was reported incorrectly.");
            boundedQueue.Close();
            while (boundedQueue.WaitPop())
            {
            }
        }

        RenderFrameQueue pacingControlQueue(4);
        RenderControlCommand admission = MakeControl(1, 1);
        admission.boundary = RenderControlBoundary::BeforeFrame;
        admission.payload = AdmitRenderFrameCommand{};
        Expect(pacingControlQueue.Push(admission) == 1,
            "Frame admission was not accepted as reliable before-frame work.");
        RenderControlCommand pacingChange = MakeControl(2, 1);
        pacingChange.boundary = RenderControlBoundary::BeforeFrame;
        pacingChange.payload = ChangeFramePacingCommand{
            Prism::RHI::MakeFramePacingConfiguration(
                Prism::RHI::FramePacingProfile::LowLatency),
            2};
        Expect(pacingControlQueue.Push(pacingChange) == 2
                && pacingControlQueue.Push(MakeFrame(1)) == 3,
            "Frame-pacing change was not ordered before its target frame.");
        RenderControlCommand invalidGeneration = MakeControl(3, 2);
        invalidGeneration.boundary = RenderControlBoundary::BeforeFrame;
        invalidGeneration.payload = ChangeFramePacingCommand{
            Prism::RHI::MakeFramePacingConfiguration(
                Prism::RHI::FramePacingProfile::Benchmark),
            0};
        Reject([&] { (void)pacingControlQueue.Push(invalidGeneration); });
        Expect(std::holds_alternative<QueuedRenderControl>(
                    pacingControlQueue.WaitPop()->payload)
                && std::holds_alternative<QueuedRenderControl>(
                    pacingControlQueue.WaitPop()->payload)
                && std::holds_alternative<FrameEnvelope>(
                    pacingControlQueue.WaitPop()->payload),
            "Frame-admission/configuration controls lost reliable queue order.");
        pacingControlQueue.Close();
        Expect(!pacingControlQueue.WaitPop().has_value(),
            "Frame-pacing control queue did not drain cleanly.");

        RenderFrameQueue protocolQueue(12);
        RenderControlCommand beforeFrame = MakeControl(1, 1);
        beforeFrame.boundary = RenderControlBoundary::BeforeFrame;
        Expect(protocolQueue.Push(beforeFrame) == 1,
            "Before-frame control was not accepted ahead of its frame.");
        Expect(protocolQueue.Push(MakeFrame(1)) == 2,
            "Frame did not follow its before-frame control.");
        Expect(protocolQueue.Push(MakeControl(2, 1)) == 3,
            "After-frame control was not accepted after its frame.");

        RenderControlCommand resize = MakeControl(3, 2);
        resize.boundary = RenderControlBoundary::BeforeFrame;
        resize.viewEpoch = RenderEpoch{2};
        resize.payload = ResizeRenderViewCommand{
            Prism::Scene::RenderViewId{1}, 1920, 1080};
        Expect(protocolQueue.Push(resize) == 4,
            "Resize did not advance the reliable view epoch.");

        RenderControlCommand sceneSwitch = MakeControl(4, 2);
        sceneSwitch.boundary = RenderControlBoundary::BeforeFrame;
        sceneSwitch.viewEpoch = RenderEpoch{2};
        sceneSwitch.payload = SwitchRenderSceneCommand{RenderEpoch{2}};
        Expect(protocolQueue.Push(sceneSwitch) == 5,
            "Scene switch did not advance the reliable scene epoch.");

        RenderControlCommand nextSceneControl = MakeControl(5, 2);
        nextSceneControl.boundary = RenderControlBoundary::BeforeFrame;
        nextSceneControl.sceneEpoch = RenderEpoch{2};
        nextSceneControl.viewEpoch = RenderEpoch{2};
        Expect(protocolQueue.Push(nextSceneControl) == 6,
            "Control for the announced scene epoch was rejected.");

        FrameEnvelope announcedFrame = MakeFrame(2);
        announcedFrame.sceneEpoch = RenderEpoch{2};
        announcedFrame.viewEpoch = RenderEpoch{2};
        Expect(protocolQueue.Push(std::move(announcedFrame)) == 7,
            "Frame did not consume the announced scene/view epochs.");

        RenderControlCommand quality = MakeControl(6, 3);
        quality.boundary = RenderControlBoundary::BeforeFrame;
        quality.sceneEpoch = RenderEpoch{2};
        quality.viewEpoch = RenderEpoch{2};
        quality.payload = ChangeRenderQualityCommand{2};
        Expect(protocolQueue.Push(quality) == 8,
            "Quality control did not announce a settings revision.");
        FrameEnvelope qualityFrame = MakeFrame(3, 0.0, true, 2);
        qualityFrame.sceneEpoch = RenderEpoch{2};
        qualityFrame.viewEpoch = RenderEpoch{2};
        Expect(protocolQueue.Push(std::move(qualityFrame)) == 9,
            "Frame did not consume the announced settings revision.");

        RenderControlCommand staleScene = nextSceneControl;
        staleScene.id = RenderControlCommandId{7};
        staleScene.targetFrame = Prism::Scene::LogicalFrameId{4};
        staleScene.sceneEpoch = RenderEpoch{1};
        Reject([&] { (void)protocolQueue.Push(staleScene); });
        RenderControlCommand staleView = nextSceneControl;
        staleView.id = RenderControlCommandId{7};
        staleView.targetFrame = Prism::Scene::LogicalFrameId{4};
        staleView.sceneEpoch = RenderEpoch{2};
        staleView.viewEpoch = RenderEpoch{1};
        Reject([&] { (void)protocolQueue.Push(staleView); });
        RenderControlCommand lateBefore = nextSceneControl;
        lateBefore.id = RenderControlCommandId{7};
        lateBefore.sceneEpoch = RenderEpoch{2};
        lateBefore.viewEpoch = RenderEpoch{2};
        lateBefore.targetFrame = Prism::Scene::LogicalFrameId{1};
        Reject([&] { (void)protocolQueue.Push(lateBefore); });

        RenderFrameQueue prematureAfterQueue;
        Reject([&]
        {
            (void)prematureAfterQueue.Push(MakeControl(1, 1));
        });
        protocolQueue.Close();
        while (protocolQueue.WaitPop())
        {
        }

        RenderFrameQueue backpressureQueue;
        (void)backpressureQueue.Push(MakeFrame(1));
        std::uint64_t blockedAcceptance = 0;
        std::thread blockedProducer([&]
        {
            blockedAcceptance =
                backpressureQueue.Push(MakeFrame(2));
        });
        for (std::size_t attempt = 0;
             attempt < 100000
             && backpressureQueue.GetStatistics()
                    .waitingProducerCount == 0;
             ++attempt)
        {
            std::this_thread::yield();
        }
        Expect(backpressureQueue.GetStatistics()
                    .waitingProducerCount == 1,
            "A full frame queue did not apply producer backpressure.");
        Expect(backpressureQueue.WaitPop()->acceptanceId == 1,
            "Backpressure changed the first accepted frame.");
        blockedProducer.join();
        Expect(blockedAcceptance == 2
                && backpressureQueue.WaitPop()->acceptanceId == 2,
            "Blocked producer did not resume in FIFO order.");
        backpressureQueue.Close();
        Expect(!backpressureQueue.WaitPop(),
            "Backpressure queue did not close after draining.");

        RenderFrameQueue closeQueue;
        (void)closeQueue.Push(MakeFrame(1));
        bool closeWokeProducer = false;
        std::thread closingProducer([&]
        {
            try
            {
                (void)closeQueue.Push(MakeFrame(2));
            }
            catch (const std::exception&)
            {
                closeWokeProducer = true;
            }
        });
        for (std::size_t attempt = 0;
             attempt < 100000
             && closeQueue.GetStatistics().waitingProducerCount == 0;
             ++attempt)
        {
            std::this_thread::yield();
        }
        closeQueue.Close();
        closingProducer.join();
        Expect(closeWokeProducer
                && closeQueue.GetStatistics().acceptedCount == 1,
            "Closing a full queue did not wake its blocked producer.");
        Expect(closeQueue.WaitPop()->acceptanceId == 1
                && !closeQueue.WaitPop(),
            "Normal close did not drain already accepted work.");

        RenderFrameQueue consumerWakeQueue;
        bool consumerClosed = false;
        std::thread blockedConsumer([&]
        {
            consumerClosed = !consumerWakeQueue.WaitPop().has_value();
        });
        consumerWakeQueue.Close();
        blockedConsumer.join();
        Expect(consumerClosed,
            "Closing an empty queue did not wake its consumer.");

        RenderFrameQueue failedQueue;
        (void)failedQueue.Push(MakeFrame(1));
        bool failureWokeProducer = false;
        std::thread failedProducer([&]
        {
            try
            {
                (void)failedQueue.Push(MakeFrame(2));
            }
            catch (const std::exception& exception)
            {
                failureWokeProducer =
                    std::string(exception.what()) == "queue failure";
            }
        });
        for (std::size_t attempt = 0;
             attempt < 100000
             && failedQueue.GetStatistics().waitingProducerCount == 0;
             ++attempt)
        {
            std::this_thread::yield();
        }
        failedQueue.Fail(std::make_exception_ptr(
            std::runtime_error("queue failure")));
        failedProducer.join();
        const auto failedStats = failedQueue.GetStatistics();
        Expect(failureWokeProducer
                && failedStats.state == RenderFrameQueueState::Failed
                && failedStats.acceptedCount == 1
                && failedStats.cancelledCount == 1
                && failedStats.waitingCount == 0,
            "Queue failure did not wake producers and cancel retained work.");
        Reject([&] { (void)failedQueue.WaitPop(); });

        VerifyService(RenderExecutionMode::Inline);
        VerifyService(RenderExecutionMode::Threaded);

        for (std::uint32_t mismatch = 1; mismatch <= 4; ++mismatch)
        {
            auto staleFeedbackState = std::make_shared<TargetState>();
            staleFeedbackState->visibilityMismatch = mismatch;
            RenderExecutionService staleFeedbackService(
                RenderExecutionMode::Inline,
                std::make_unique<RecordingTarget>(
                    staleFeedbackState));
            (void)staleFeedbackService.SubmitFrame(MakeFrame(1));
            const RenderControlTicket independentControl =
                staleFeedbackService.SubmitControl(
                    MakeControl(1, 1));
            const auto staleFeedback =
                staleFeedbackService.ConsumeLatestFrameFeedback();
            const RenderViewRuntimeFeedback* const gameFeedback =
                staleFeedback.has_value()
                ? staleFeedback->FindView(
                    Prism::Scene::GameRenderViewId)
                : nullptr;
            Expect(independentControl.Wait().succeeded
                    && staleFeedback.has_value()
                    && staleFeedback
                        ->rejectedVisibilityFeedbackCount == 1
                    && gameFeedback != nullptr
                    && gameFeedback->visibilityFeedback == nullptr,
                "Mismatched visibility feedback affected reliable control or escaped rejection.");
            staleFeedbackService.Shutdown();
        }

        for (const bool mismatch : {false, true})
        {
            auto controlState = std::make_shared<TargetState>();
            controlState->rejectFirstControl = !mismatch;
            controlState->mismatchFirstControl = mismatch;
            RenderExecutionService controlService(
                RenderExecutionMode::Inline,
                std::make_unique<RecordingTarget>(controlState));
            (void)controlService.SubmitFrame(MakeFrame(1));
            if (mismatch)
            {
                Reject([&]
                {
                    (void)controlService.SubmitControl(
                        MakeControl(1, 1));
                });
            }
            else
            {
                const RenderControlTicket ticket =
                    controlService.SubmitControl(MakeControl(1, 1));
                const RenderControlAcknowledgement rejected =
                    ticket.Wait();
                Expect(!rejected.succeeded,
                    "A rejected reliable control was reported successful.");
            }
            controlService.Shutdown();
        }

        // Deterministic producer/consumer overlap: frame N remains blocked on
        // the render lane while the main lane constructs immutable input for
        // N+1. No queued backlog or frame replacement is involved.
        auto pipelineState =
            std::make_shared<PipelineLatchState>();
        RenderExecutionService pipelineService(
            RenderExecutionMode::Threaded,
            std::make_unique<PipelineLatchTarget>(pipelineState));
        FrameEnvelope pipelineFirst = MakeFrame(1);
        pipelineFirst.producerInputTimestampNanoseconds =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()).count());
        const std::uint64_t pipelineFirstAcceptance =
            pipelineService.SubmitFrame(std::move(pipelineFirst));
        pipelineState->renderFrameEntered.Wait();
        FrameEnvelope pipelineSecond = MakeFrame(2);
        const bool preparedWhileRenderNBlocked =
            !pipelineState->firstRenderCompleted.load(
                std::memory_order_acquire);
        pipelineState->releaseRenderFrame.CountDown();
        const std::uint64_t pipelineSecondAcceptance =
            pipelineService.SubmitFrame(std::move(pipelineSecond));
        pipelineService.WaitUntilCompleted(
            pipelineSecondAcceptance);
        const std::vector<RenderFrameCompletion>
            pipelineCompletions =
                pipelineService.ConsumeFrameCompletions();
        const std::optional<RenderFrameFeedback>
            pipelineFeedback =
                pipelineService.ConsumeLatestFrameFeedback();
        const RenderFrameQueueStatistics pipelineStatistics =
            pipelineService.GetQueueStatistics();
        pipelineService.Shutdown();
        Expect(preparedWhileRenderNBlocked,
            "Main N+1 preparation did not overlap render N.");
        Expect(pipelineFirstAcceptance == 1
                && pipelineSecondAcceptance == 2
                && pipelineCompletions.size() == 2
                && pipelineCompletions[0].logicalFrameId.value == 1
                && pipelineCompletions[1].logicalFrameId.value == 2,
            "Pipelined frames were dropped, duplicated, or reordered.");
        Expect(pipelineStatistics.acceptedCount == 2
                && pipelineStatistics.consumedCount == 2
                && pipelineStatistics.cancelledCount == 0
                && pipelineStatistics.waitingCount == 0
                && pipelineStatistics.peakWaitingCount <= 1,
            "Pipelined overlap used backlog instead of bounded execution.");
        Expect(pipelineFeedback.has_value()
                && pipelineFeedback->logicalFrameId.value == 2
                && pipelineFeedback->pipeline
                    .waitingDepthAtAcceptance <= 1
                && pipelineFeedback->pipeline.peakWaitingDepth <= 1
                && pipelineFeedback->pipeline.inputToPresentCpuMs > 0.0,
            "Frame identity, queue depth, or input-to-present diagnostics were not published.");

        constexpr std::uint64_t LifecycleStressFrameCount = 128;
        auto lifecycleState = std::make_shared<TargetState>();
        RenderExecutionService lifecycleService(
            RenderExecutionMode::Threaded,
            std::make_unique<RecordingTarget>(lifecycleState));
        for (std::uint64_t frameId = 1;
             frameId <= LifecycleStressFrameCount;
             ++frameId)
        {
            FrameEnvelope frame = MakeFrame(frameId);
            std::weak_ptr<const Prism::Scene::RenderFramePacket>
                retainedFrame = frame.frame;
            const std::uint64_t acceptance =
                lifecycleService.SubmitFrame(std::move(frame));
            lifecycleService.WaitUntilCompleted(acceptance);
            const std::vector<RenderFrameCompletion> completed =
                lifecycleService.ConsumeFrameCompletions();
            std::optional<RenderFrameFeedback> feedback =
                lifecycleService.ConsumeLatestFrameFeedback();
            Expect(completed.size() == 1
                    && completed.front().logicalFrameId.value == frameId
                    && feedback.has_value()
                    && feedback->logicalFrameId.value == frameId,
                "Lifecycle stress lost or reordered frame feedback.");
            feedback.reset();
            Expect(retainedFrame.expired(),
                "Completed frame packet remained retained by the execution service.");

            RenderControlCommand control =
                MakeControl(frameId, frameId);
            if ((frameId % 2u) == 0u)
            {
                control.payload = ProcessAssetRenderWorkCommand{
                    frameId + 1u};
            }
            const RenderControlAcknowledgement acknowledgement =
                lifecycleService.SubmitControl(
                    std::move(control)).Wait();
            Expect(acknowledgement.succeeded,
                "Lifecycle stress reliable control was rejected.");
        }
        const RenderFrameQueueStatistics lifecycleStatistics =
            lifecycleService.GetQueueStatistics();
        lifecycleService.Shutdown();
        Expect(lifecycleStatistics.acceptedCount
                    == LifecycleStressFrameCount * 2u
                && lifecycleStatistics.consumedCount
                    == LifecycleStressFrameCount * 2u
                && lifecycleStatistics.cancelledCount == 0
                && lifecycleStatistics.waitingCount == 0
                && lifecycleStatistics.peakWaitingCount <= 1,
            "Lifecycle stress exceeded bounded storage or lost accepted work.");

        auto overlapState = std::make_shared<TargetState>();
        overlapState->blockFirstFrame = true;
        RenderExecutionService overlap(
            RenderExecutionMode::Threaded,
            std::make_unique<RecordingTarget>(overlapState));
        const std::uint64_t overlapFirst =
            overlap.SubmitFrame(MakeFrame(1));
        {
            std::unique_lock lock(overlapState->mutex);
            overlapState->condition.wait(lock, [&]
            {
                return overlapState->firstFrameEntered;
            });
        }
        const std::uint64_t overlapSecond =
            overlap.SubmitFrame(MakeFrame(2));
        std::uint64_t overlapThird = 0;
        std::thread thirdProducer([&]
        {
            overlapThird = overlap.SubmitFrame(MakeFrame(3));
        });
        for (std::size_t attempt = 0;
             attempt < 100000
             && overlap.GetQueueStatistics()
                    .waitingProducerCount == 0;
             ++attempt)
        {
            std::this_thread::yield();
        }
        Expect(overlap.GetQueueStatistics().waitingCount == 1
                && overlap.GetQueueStatistics()
                    .waitingProducerCount == 1,
            "One executing plus one waiting frame was not bounded.");
        {
            std::lock_guard lock(overlapState->mutex);
            overlapState->releaseFirstFrame = true;
        }
        overlapState->condition.notify_all();
        thirdProducer.join();
        overlap.WaitUntilCompleted(overlapThird);
        overlap.Shutdown();
        Expect(overlapFirst == 1 && overlapSecond == 2
                && overlapThird == 3,
            "Backpressured service acceptance order changed.");
        {
            std::lock_guard lock(overlapState->mutex);
            Expect(overlapState->order == std::vector<std::string>{
                    "initialize", "frame:1", "frame:2", "frame:3",
                    "shutdown"},
                "Threaded service dropped or duplicated an accepted frame.");
        }

        auto stopState = std::make_shared<TargetState>();
        stopState->blockFirstFrame = true;
        RenderExecutionService stopService(
            RenderExecutionMode::Threaded,
            std::make_unique<RecordingTarget>(stopState),
            2);
        (void)stopService.SubmitFrame(MakeFrame(1));
        {
            std::unique_lock lock(stopState->mutex);
            stopState->condition.wait(lock, [&]
            {
                return stopState->firstFrameEntered;
            });
        }
        const RenderControlTicket stopTicket =
            stopService.SubmitControl(MakeStopControl(1, 1));
        const std::uint64_t acceptedAfterStop =
            stopService.SubmitFrame(MakeFrame(2));
        {
            std::lock_guard lock(stopState->mutex);
            stopState->releaseFirstFrame = true;
        }
        stopState->condition.notify_all();
        stopService.WaitUntilCompleted(acceptedAfterStop);
        Expect(stopTicket.Wait().succeeded,
            "Reliable stop control was not acknowledged.");
        Reject([&]
        {
            (void)stopService.SubmitFrame(MakeFrame(3));
        });
        stopService.Shutdown();
        const RenderFrameQueueStatistics stopStatistics =
            stopService.GetQueueStatistics();
        Expect(stopStatistics.acceptedCount == 3
                && stopStatistics.consumedCount == 3
                && stopStatistics.cancelledCount == 0
                && stopStatistics.state
                    == RenderFrameQueueState::Closed,
            "Stop control did not drain already accepted work.");

        auto concurrentShutdownState =
            std::make_shared<TargetState>();
        concurrentShutdownState->blockFirstFrame = true;
        RenderExecutionService concurrentShutdown(
            RenderExecutionMode::Threaded,
            std::make_unique<RecordingTarget>(
                concurrentShutdownState));
        (void)concurrentShutdown.SubmitFrame(MakeFrame(1));
        {
            std::unique_lock lock(concurrentShutdownState->mutex);
            concurrentShutdownState->condition.wait(lock, [&]
            {
                return concurrentShutdownState->firstFrameEntered;
            });
        }
        (void)concurrentShutdown.SubmitFrame(MakeFrame(2));
        std::atomic_bool blockedProducerWoke = false;
        std::thread blockedShutdownProducer([&]
        {
            try
            {
                (void)concurrentShutdown.SubmitFrame(MakeFrame(3));
            }
            catch (const std::exception&)
            {
                blockedProducerWoke = true;
            }
        });
        for (std::size_t attempt = 0;
             attempt < 100000
             && concurrentShutdown.GetQueueStatistics()
                    .waitingProducerCount == 0;
             ++attempt)
        {
            std::this_thread::yield();
        }
        std::atomic_bool firstShutdownReturned = false;
        std::atomic_bool secondShutdownReturned = false;
        std::thread firstShutdown([&]
        {
            concurrentShutdown.Shutdown();
            firstShutdownReturned = true;
        });
        std::thread secondShutdown([&]
        {
            concurrentShutdown.Shutdown();
            secondShutdownReturned = true;
        });
        for (std::size_t attempt = 0;
             attempt < 100000
             && concurrentShutdown.GetQueueStatistics().state
                    == RenderFrameQueueState::Open;
             ++attempt)
        {
            std::this_thread::yield();
        }
        Expect(!firstShutdownReturned && !secondShutdownReturned,
            "Concurrent shutdown returned before executing work drained.");
        {
            std::lock_guard lock(concurrentShutdownState->mutex);
            concurrentShutdownState->releaseFirstFrame = true;
        }
        concurrentShutdownState->condition.notify_all();
        firstShutdown.join();
        secondShutdown.join();
        blockedShutdownProducer.join();
        const RenderFrameQueueStatistics shutdownStatistics =
            concurrentShutdown.GetQueueStatistics();
        Expect(firstShutdownReturned && secondShutdownReturned
                && blockedProducerWoke
                && shutdownStatistics.acceptedCount == 2
                && shutdownStatistics.consumedCount == 2
                && shutdownStatistics.cancelledCount == 0
                && shutdownStatistics.state
                    == RenderFrameQueueState::Closed,
            "Full-queue shutdown did not wake producers and drain once.");
        {
            std::lock_guard lock(concurrentShutdownState->mutex);
            Expect(concurrentShutdownState->shutdownCount == 1,
                "Concurrent shutdown destroyed the render target twice.");
        }

        auto failedState = std::make_shared<TargetState>();
        failedState->failFirstFrame = true;
        RenderExecutionService failed(
            RenderExecutionMode::Threaded,
            std::make_unique<RecordingTarget>(failedState));
        const std::uint64_t failedFrame =
            failed.SubmitFrame(MakeFrame(1));
        Reject([&] { failed.WaitUntilCompleted(failedFrame); });
        Reject([&] { (void)failed.SubmitFrame(MakeFrame(2)); });
        Expect(failed.ConsumeFrameCompletions().empty(),
            "A failed render frame reported successful completion.");
        failed.Shutdown();
        Expect(failed.GetQueueStatistics().state
                == RenderFrameQueueState::Failed,
            "Render failure did not close and wake the queue.");

        auto cancelledControlState = std::make_shared<TargetState>();
        cancelledControlState->blockFirstFrame = true;
        cancelledControlState->failFirstFrame = true;
        RenderExecutionService cancelledControlService(
            RenderExecutionMode::Threaded,
            std::make_unique<RecordingTarget>(cancelledControlState));
        (void)cancelledControlService.SubmitFrame(MakeFrame(1));
        {
            std::unique_lock lock(cancelledControlState->mutex);
            cancelledControlState->condition.wait(lock, [&]
            {
                return cancelledControlState->firstFrameEntered;
            });
        }
        const RenderControlTicket cancelledTicket =
            cancelledControlService.SubmitControl(MakeControl(1, 1));
        {
            std::lock_guard lock(cancelledControlState->mutex);
            cancelledControlState->releaseFirstFrame = true;
        }
        cancelledControlState->condition.notify_all();
        Reject([&] { (void)cancelledTicket.Wait(); });
        cancelledControlService.Shutdown();

        for (const RenderExecutionMode mode : {
                 RenderExecutionMode::Inline,
                 RenderExecutionMode::Threaded})
        {
            auto initializationState =
                std::make_shared<TargetState>();
            initializationState->failInitialize = true;
            Reject([&]
            {
                RenderExecutionService initializationFailure(
                    mode,
                    std::make_unique<RecordingTarget>(
                        initializationState));
            });
            std::lock_guard lock(initializationState->mutex);
            Expect(initializationState->initializeCalled
                    && initializationState->shutdownCount == 1,
                "Partial render initialization was not cleaned exactly once.");
        }

        std::cout << "Render frame queue tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
