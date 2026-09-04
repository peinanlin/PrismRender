#include "Renderer/Graph/GraphExecutor.h"

#include "Core/Assert.h"
#include "Core/CpuTrace.h"
#include "Core/Threading/ITaskExecutor.h"
#include "Core/Threading/TaskGroup.h"
#include "RHI/DeferredCommandContext.h"
#include "RHI/ICommandContext.h"
#include "Renderer/Graph/ResourceStateTracker.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <ranges>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Prism::Renderer
{
namespace
{
constexpr std::size_t InvalidIndex =
    std::numeric_limits<std::size_t>::max();

class ExecutionLaneAffinity
{
public:
    ExecutionLaneAffinity()
        : m_thread(std::this_thread::get_id())
    {
    }

    void Check() const
    {
        Core::Check(
            std::this_thread::get_id() == m_thread,
            "RenderGraph execution-lane work crossed thread affinity.");
    }

private:
    std::thread::id m_thread;
};

class GraphExecutionScope
{
public:
    GraphExecutionScope(
        GraphExecutionState& state,
        GraphCompilationSummary& summary)
        : m_state(state),
          m_summary(summary),
          m_uncaughtExceptions(std::uncaught_exceptions())
    {
        m_state.BeginExecution(summary);
    }

    ~GraphExecutionScope()
    {
        m_state.EndExecution(
            m_summary,
            std::uncaught_exceptions() == m_uncaughtExceptions);
    }

    GraphExecutionScope(const GraphExecutionScope&) = delete;
    GraphExecutionScope& operator=(const GraphExecutionScope&) = delete;

private:
    GraphExecutionState& m_state;
    GraphCompilationSummary& m_summary;
    int m_uncaughtExceptions = 0;
};

RHI::CommandQueueType ToCommandQueueType(const GraphQueueClass queue)
{
    return queue == GraphQueueClass::Compute
        ? RHI::CommandQueueType::Compute
        : RHI::CommandQueueType::Graphics;
}

std::size_t QueueIndex(const GraphQueueClass queue)
{
    return queue == GraphQueueClass::Compute ? 1u : 0u;
}
} // namespace

void GraphExecutor::Execute(
    GraphDescription& description,
    GraphExecutionState& executionState,
    CompiledGraph& compiledGraph,
    const bool detailedProfilingEnabled,
    const GraphMarkerCallback& beginMarker,
    const GraphMarkerCallback& endMarker)
{
    const GraphExecutionScope executionScope(
        executionState,
        compiledGraph.summary);
    using Clock = std::chrono::steady_clock;
    std::unordered_set<std::string> availableResources = description.importedResources;
    executionState.passInfos.reserve(description.passes.size());

    for (GraphPassDeclaration& pass : description.passes)
    {
        if (!pass.active)
        {
            continue;
        }
        ValidateReads(pass, availableResources);
        Core::Check(static_cast<bool>(pass.execute), "A context RenderGraph pass requires Execute(ICommandContext&).");

        if (beginMarker)
        {
            beginMarker(pass.info.name);
        }

        const Clock::time_point start =
            detailedProfilingEnabled
                ? Clock::now() : Clock::time_point{};
        pass.execute();
        RecordPassResult(pass, detailedProfilingEnabled, start);

        if (endMarker)
        {
            endMarker(pass.info.name);
        }

        for (const std::string& resource : pass.writes)
        {
            availableResources.emplace(resource);
        }
        executionState.passInfos.push_back(pass.info);
    }
}

void GraphExecutor::ExecuteQueueBatches(
    GraphDescription& description,
    GraphExecutionState& executionState,
    CompiledGraph& compiledGraph,
    const bool detailedProfilingEnabled,
    RHI::ICommandContext& commandContext,
    Core::ITaskExecutor& taskExecutor,
    const GraphMarkerCallback& beginMarker,
    const GraphMarkerCallback& endMarker)
{
    using Clock = std::chrono::steady_clock;
    const ExecutionLaneAffinity executionLane;
    struct RecordedPass
    {
        std::size_t passIndex = InvalidIndex;
        std::unique_ptr<
            RHI::DeferredCommandContext> prefix;
        std::unique_ptr<
            RHI::DeferredCommandContext> body;
        std::unique_ptr<
            RHI::IParallelCommandRecording>
            nativeBody;
        bool recordedInParallel = false;
        bool recordedNatively = false;
    };
    struct RecordedBatch
    {
        std::vector<RecordedPass> passes;
        std::unique_ptr<
            RHI::DeferredCommandContext> tail;
    };

    const RHI::GraphicsApi graphicsApi =
        commandContext.GetGraphicsApi();
    const RHI::CommandQueueCapabilities
        capabilities =
            commandContext.GetQueueCapabilities();
    std::vector<RecordedBatch> recordedBatches(
        compiledGraph.queueBatchDescriptions.size());
    std::unordered_set<std::string>
        availableResources = description.importedResources;
    Core::TaskGroup recordingTasks;

    // Reused textures can still carry the previous graphics frame's pixel
    // shader state. A compute list cannot release that state itself. Prepare
    // them on the graphics prologue before planning their first compute use.
    bool importedGraphicsHandoff = false;
    for (const auto& [name, resource] : compiledGraph.compiledResources)
    {
        if (!resource.active || !resource.texture)
            continue;
        const auto first = std::ranges::find(description.passes, resource.firstUse, &GraphPassDeclaration::executionIndex);
        if (first == description.passes.end() || first->options.queue != GraphQueueClass::Compute)
            continue;
        const auto texture = executionState.textureResources.find(name);
        if (texture == executionState.textureResources.end()) continue;
        const bool initialized = std::ranges::any_of(texture->second.subresourceStates,
            [](RHI::ResourceState state) { return state != RHI::ResourceState::Undefined
                && state != RHI::ResourceState::Common; });
        if (!initialized) continue;
        ResourceStateTracker::TransitionResourceToCommon(
            name,
            executionState,
            compiledGraph,
            commandContext);
        importedGraphicsHandoff = true;
    }

    // Resource-state planning remains deterministic on the main thread.
    // Opted-in pass bodies record into isolated command streams in parallel.
    for (const std::size_t batchIndex :
         compiledGraph.queueBatchSubmissionOrder)
    {
        const GraphQueueBatchDescription& batch =
            compiledGraph.queueBatchDescriptions[batchIndex];
        RecordedBatch& recordedBatch =
            recordedBatches[batchIndex];
        for (const std::size_t passIndex :
             batch.passes)
        {
            executionLane.Check();
            GraphPassDeclaration& pass = description.passes[passIndex];
            ValidateReads(
                pass,
                availableResources);
            RecordedPass recordedPass{};
            recordedPass.passIndex = passIndex;
            recordedPass.prefix =
                std::make_unique<
                    RHI::DeferredCommandContext>(
                    graphicsApi,
                    ToCommandQueueType(batch.queue),
                    commandContext
                        .SupportsBufferRangeBarriers());
            ResourceStateTracker::PrepareAliasingBarriers(
                pass,
                executionState,
                compiledGraph,
                *recordedPass.prefix);
            ResourceStateTracker::PreparePassBarriers(
                pass,
                executionState,
                compiledGraph,
                *recordedPass.prefix);
            if (pass.options.parallelRecordable
                && pass.options.parallelRecordingContract.IsSatisfied()
                && pass.contextExecute)
            {
                recordedPass.recordedInParallel =
                    true;
                if (capabilities
                        .nativeParallelCommandRecording)
                {
                    recordedPass.nativeBody =
                        commandContext
                            .CreateParallelCommandRecording(
                                ToCommandQueueType(
                                    batch.queue));
                    Core::Check(
                        recordedPass.nativeBody
                            != nullptr,
                        "The RHI advertised native parallel command recording but did not create a recording.");
                    RHI::IParallelCommandRecording*
                        nativeBody =
                            recordedPass
                                .nativeBody.get();
                    recordedPass.recordedNatively =
                        true;
                    recordingTasks.Submit(
                        taskExecutor,
                            [detailedProfilingEnabled,
                             &pass,
                             nativeBody]()
                             {
                                 Core::CpuTraceSpan span(
                                     "NativeCommandRecord",
                                     "rdg.record");
                                 const Clock::time_point start =
                                     detailedProfilingEnabled
                                         ? Clock::now()
                                         : Clock::time_point{};
                                 RHI::ICommandContext&
                                     recordingContext =
                                         nativeBody
                                             ->GetCommandContext();
                                 {
                                     RHI::ScopedDebugLabel label(
                                         recordingContext,
                                         pass.info.name);
                                     pass.contextExecute(
                                         recordingContext);
                                 }
                                 Core::Check(
                                     nativeBody->Close(),
                                    "The RHI failed to close a native parallel command recording.");
                                RecordPassResult(pass, detailedProfilingEnabled, start);
                            });
                }
                else
                {
                    recordedPass.body =
                        std::make_unique<
                            RHI::DeferredCommandContext>(
                            graphicsApi,
                            ToCommandQueueType(
                                batch.queue),
                            commandContext
                                .SupportsBufferRangeBarriers());
                    RHI::DeferredCommandContext*
                        deferredBody =
                            recordedPass.body.get();
                    recordingTasks.Submit(
                        taskExecutor,
                            [detailedProfilingEnabled,
                             &pass,
                             deferredBody]()
                             {
                                 Core::CpuTraceSpan span(
                                     "DeferredCommandRecord",
                                     "rdg.record");
                                 const Clock::time_point start =
                                     detailedProfilingEnabled
                                         ? Clock::now()
                                         : Clock::time_point{};
                                 {
                                     RHI::ScopedDebugLabel label(
                                         *deferredBody,
                                         pass.info.name);
                                     pass.contextExecute(
                                         *deferredBody);
                                 }
                                RecordPassResult(pass, detailedProfilingEnabled, start);
                            });
                }
            }
            for (const std::string& resource :
                 pass.writes)
            {
                availableResources.emplace(resource);
            }
            recordedBatch.passes.push_back(
                std::move(recordedPass));
        }
        recordedBatch.tail =
            std::make_unique<
                RHI::DeferredCommandContext>(
                graphicsApi,
                ToCommandQueueType(batch.queue),
                commandContext
                    .SupportsBufferRangeBarriers());
        ResourceStateTracker::PrepareBatchQueueHandoffs(
            batch,
            description,
            executionState,
            compiledGraph,
            *recordedBatch.tail);
    }

    executionLane.Check();
    const Clock::time_point joinStart = Clock::now();
    try
    {
        recordingTasks.Wait();
    }
    catch (...)
    {
        taskExecutor.RecordJoinDuration(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - joinStart));
        throw;
    }
    taskExecutor.RecordJoinDuration(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - joinStart));

    std::vector<RHI::QueueSyncPoint> syncPoints(
        compiledGraph.queueBatchDescriptions.size());
    std::array<RHI::QueueSyncPoint, 2>
        latestQueueSignals{};
    GraphQueueClass previousQueue =
        GraphQueueClass::Graphics;
    bool hasPreviousQueue = false;
    std::size_t queueSwitches = 0;

    for (const std::size_t batchIndex :
         compiledGraph.queueBatchSubmissionOrder)
    {
        executionLane.Check();
        Core::CpuTraceSpan batchSpan(
            "QueueBatchRecord",
            "rdg.submit");
        const GraphQueueBatchDescription& batch =
            compiledGraph.queueBatchDescriptions[batchIndex];
        std::array<RHI::QueueSyncPoint, 2>
            waitByQueue{};
        if (importedGraphicsHandoff && batch.queue == GraphQueueClass::Compute)
        {
            // The first graphics batch owns the prologue, including imports
            // with no producer pass in this frame's dependency DAG.
            const auto firstBatch = compiledGraph.queueBatchSubmissionOrder.front();
            const auto signal = syncPoints[firstBatch];
            Core::Check(signal.IsValid() && signal.queue == RHI::CommandQueueType::Graphics,
                "Imported compute resources require a submitted graphics prologue.");
            waitByQueue[QueueIndex(GraphQueueClass::Graphics)] = signal;
        }
        for (const std::size_t dependency :
             batch.dependencies)
        {
            const GraphQueueBatchDescription& producer =
                compiledGraph.queueBatchDescriptions[dependency];
            if (producer.queue == batch.queue)
            {
                continue;
            }
            const RHI::QueueSyncPoint signal =
                syncPoints[dependency];
            Core::Check(
                signal.IsValid(),
                "RenderGraph queue-batch dependency was not submitted.");
            RHI::QueueSyncPoint& wait =
                waitByQueue[
                    QueueIndex(producer.queue)];
            if (!wait.IsValid()
                || signal.value > wait.value)
            {
                wait = signal;
            }
        }
        std::vector<RHI::QueueSyncPoint> waits;
        for (const RHI::QueueSyncPoint wait :
             waitByQueue)
        {
            if (wait.IsValid())
            {
                waits.push_back(wait);
            }
        }

        Core::Check(
            commandContext.BeginQueueBatch(
                ToCommandQueueType(batch.queue),
                waits),
            "The RHI failed to begin a RenderGraph queue batch.");
        if (hasPreviousQueue
            && previousQueue != batch.queue)
        {
            ++queueSwitches;
        }
        previousQueue = batch.queue;
        hasPreviousQueue = true;

        RecordedBatch& recordedBatch =
            recordedBatches[batchIndex];
        for (RecordedPass& recordedPass :
             recordedBatch.passes)
        {
            GraphPassDeclaration& pass =
                description.passes[recordedPass.passIndex];
            recordedPass.prefix->Replay(
                commandContext);
            if (beginMarker)
            {
                beginMarker(pass.info.name);
            }
             if (recordedPass.recordedInParallel)
             {
                if (recordedPass.recordedNatively)
                {
                    Core::Check(
                        commandContext
                            .AppendParallelCommandRecording(
                                std::move(
                                    recordedPass
                                        .nativeBody)),
                        "The RHI failed to append a native parallel command recording.");
                    ++compiledGraph.summary
                          .nativeParallelRecordedPassCount;
                }
                else
                {
                    recordedPass.body->Replay(
                        commandContext);
                    compiledGraph.summary
                        .recordedCommandCount +=
                        recordedPass.body
                            ->GetCommandCount();
                }
                ++compiledGraph.summary
                      .parallelRecordedPassCount;
            }
             else
             {
                 executionLane.Check();
                 const Clock::time_point start =
                     detailedProfilingEnabled
                         ? Clock::now()
                         : Clock::time_point{};
                 {
                     RHI::ScopedDebugLabel label(
                         commandContext,
                         pass.info.name);
                     if (pass.contextExecute)
                     {
                         pass.contextExecute(
                             commandContext);
                     }
                     else
                     {
                         pass.execute();
                     }
                 }
                 RecordPassResult(pass, detailedProfilingEnabled, start);
            }
            if (endMarker)
            {
                endMarker(pass.info.name);
            }
            executionState.passInfos.push_back(pass.info);
        }

        recordedBatch.tail->Replay(
            commandContext);
        const RHI::QueueSyncPoint signal =
            commandContext.EndQueueBatch();
        Core::Check(
            signal.IsValid()
                && signal.queue
                    == ToCommandQueueType(batch.queue),
            "The RHI returned an invalid RenderGraph queue-batch signal.");
        syncPoints[batchIndex] = signal;
        latestQueueSignals[
            QueueIndex(batch.queue)] = signal;
    }

    if (capabilities.deferredBatchSubmission)
    {
        Core::CpuTraceSpan submitSpan(
            "QueueBatchSubmit",
            "rdg.submit");
        Core::Check(
            commandContext.FlushQueueBatches(),
            "The RHI failed to submit deferred RenderGraph queue batches.");
        compiledGraph.summary
            .deferredQueueBatchSubmissionApplied =
            true;
    }

    std::vector<RHI::QueueSyncPoint>
        continuationWaits;
    for (const RHI::QueueSyncPoint signal :
         latestQueueSignals)
    {
        if (signal.IsValid())
        {
            continuationWaits.push_back(signal);
        }
    }
    Core::Check(
        commandContext.ResumeGraphicsQueue(
            continuationWaits),
        "The RHI failed to resume graphics after RenderGraph queue batches.");

    for (auto& [name, texture] :
         executionState.textureResources)
    {
        const auto compiled =
            compiledGraph.compiledResources.find(name);
        if (compiled == compiledGraph.compiledResources.end()
            || !compiled->second.active
            || compiled->second.lastUse
                == InvalidIndex
            || texture.texture == nullptr
            || texture.state
                != RHI::ResourceState::Common)
        {
            continue;
        }
        const auto lastPass =
            std::ranges::find(
                description.passes,
                compiled->second.lastUse,
                &GraphPassDeclaration::executionIndex);
        if (lastPass == description.passes.end()
            || lastPass->options.queue
                != GraphQueueClass::Compute)
        {
            continue;
        }
        RHI::ResourceState restoredState =
            RHI::ResourceState::Undefined;
        const auto write =
            std::ranges::find(
                lastPass->resourceWrites,
                name,
                &GraphTrackedResourceAccess::name);
        if (write != lastPass->resourceWrites.end())
        {
            restoredState = write->state;
        }
        else
        {
            const auto read =
                std::ranges::find(
                    lastPass->resourceReads,
                    name,
                    &GraphTrackedResourceAccess::name);
            if (read != lastPass->resourceReads.end())
            {
                restoredState = read->state;
            }
        }
        if (restoredState
                == RHI::ResourceState::Undefined
            || restoredState
                == RHI::ResourceState::Common)
        {
            continue;
        }
        commandContext.TextureBarrier({
            texture.texture,
            RHI::ResourceState::Common,
            restoredState});
        texture.state = restoredState;
        std::ranges::fill(texture.subresourceStates, restoredState);
        compiled->second.state =
            restoredState;
    }

    compiledGraph.summary
        .nativeMultiQueueSubmissionApplied = true;
    compiledGraph.summary
        .dagQueueBatchExecutionApplied = true;
    compiledGraph.summary
        .parallelCommandRecordingApplied =
        compiledGraph.summary
            .parallelRecordedPassCount > 0;
    compiledGraph.summary
        .nativeParallelCommandRecordingApplied =
        compiledGraph.summary
            .nativeParallelRecordedPassCount > 0;
    compiledGraph.summary.multiQueueGpuTimestamps =
        true;
    compiledGraph.summary.serialQueueExecution =
        false;
    compiledGraph.summary.nativeQueueSwitchCount =
        queueSwitches;
    compiledGraph.summary.nativeQueueSegmentCount =
        compiledGraph.queueBatchDescriptions.size();
}

void GraphExecutor::Execute(
    GraphDescription& description,
    GraphExecutionState& executionState,
    CompiledGraph& compiledGraph,
    const bool detailedProfilingEnabled,
    RHI::ICommandContext& commandContext,
    const GraphMarkerCallback& beginMarker,
    const GraphMarkerCallback& endMarker)
{
    Core::InlineTaskExecutor inlineExecutor;
    Execute(
        description,
        executionState,
        compiledGraph,
        detailedProfilingEnabled,
        commandContext,
        inlineExecutor,
        beginMarker,
        endMarker);
}

void GraphExecutor::Execute(
    GraphDescription& description,
    GraphExecutionState& executionState,
    CompiledGraph& compiledGraph,
    const bool detailedProfilingEnabled,
    RHI::ICommandContext& commandContext,
    Core::ITaskExecutor& taskExecutor,
    const GraphMarkerCallback& beginMarker,
    const GraphMarkerCallback& endMarker)
{
    const GraphExecutionScope executionScope(
        executionState,
        compiledGraph.summary);
    using Clock = std::chrono::steady_clock;
    std::unordered_set<std::string> availableResources = description.importedResources;
    executionState.passInfos.reserve(description.passes.size());
    compiledGraph.summary.executedAliasingBarrierCount = 0;
    const RHI::CommandQueueCapabilities queueCapabilities =
        commandContext.GetQueueCapabilities();
    compiledGraph.summary.nativeComputeQueueAvailable =
        queueCapabilities.compute;
    compiledGraph.summary
        .dedicatedComputeQueueAvailable =
        queueCapabilities.dedicatedCompute;
    compiledGraph.summary.timelineQueueSynchronization =
        queueCapabilities.timelineSynchronization;
    compiledGraph.summary
        .nativeMultiQueueSubmissionApplied = false;
    compiledGraph.summary
        .independentQueueBatchSubmissionAvailable =
        queueCapabilities.independentBatchSubmission;
    compiledGraph.summary
        .deferredQueueBatchSubmissionAvailable =
        queueCapabilities.deferredBatchSubmission;
    compiledGraph.summary
        .dagQueueBatchExecutionApplied = false;
    compiledGraph.summary
        .deferredQueueBatchSubmissionApplied = false;
    compiledGraph.summary
        .parallelCommandRecordingApplied = false;
    compiledGraph.summary
        .nativeParallelCommandRecordingApplied = false;
    compiledGraph.summary.parallelRecordedPassCount = 0;
    compiledGraph.summary
        .nativeParallelRecordedPassCount = 0;
    compiledGraph.summary.recordedCommandCount = 0;
    compiledGraph.summary.automaticQueueDecision =
        description.automaticQueueDecision;
    compiledGraph.summary.multiQueueGpuTimestamps = false;
    compiledGraph.summary.requestedQueueExecutionMode =
        description.queueExecutionMode;
    compiledGraph.summary.nativeQueueSwitchCount = 0;
    compiledGraph.summary.nativeQueueSegmentCount = 1;
    const bool automaticNativeSelection =
        description.queueExecutionMode
            == GraphQueueExecutionMode::Automatic
        && description.automaticQueueDecision.selectNative;
    const bool dagQueueBatchExecution =
        (description.queueExecutionMode
             == GraphQueueExecutionMode::Native
         || automaticNativeSelection)
        && queueCapabilities.compute
        && queueCapabilities.timelineSynchronization
        && queueCapabilities.independentBatchSubmission
        && compiledGraph.summary.crossQueueSyncCount > 0;
    if (dagQueueBatchExecution)
    {
        ExecuteQueueBatches(
            description,
            executionState,
            compiledGraph,
            detailedProfilingEnabled,
            commandContext,
            taskExecutor,
            beginMarker,
            endMarker);
        return;
    }
    const bool nativeQueueExecution =
        (description.queueExecutionMode
             == GraphQueueExecutionMode::Native
         || automaticNativeSelection)
        && !queueCapabilities.independentBatchSubmission
        && queueCapabilities.compute
        && queueCapabilities.timelineSynchronization
        && queueCapabilities.nativeQueueSwitching
        && compiledGraph.summary.crossQueueSyncCount > 0;
    GraphQueueClass activeQueue = GraphQueueClass::Graphics;
    Core::Check(
        commandContext.GetActiveCommandQueue()
            == RHI::CommandQueueType::Graphics,
        "RenderGraph native multi-queue execution must begin on the graphics queue.");

    for (GraphPassDeclaration& pass : description.passes)
    {
        if (!pass.active)
        {
            continue;
        }
        if (nativeQueueExecution
            && pass.options.queue != activeQueue)
        {
            ResourceStateTracker::PrepareQueueHandoff(
                activeQueue,
                pass.options.queue,
                pass.executionIndex,
                description,
                executionState,
                compiledGraph,
                commandContext);
            Core::Check(
                commandContext.SwitchCommandQueue(
                    ToCommandQueueType(pass.options.queue)),
                "The RHI failed to switch RenderGraph command queues.");
            activeQueue = pass.options.queue;
            ++compiledGraph.summary
                  .nativeQueueSwitchCount;
            ++compiledGraph.summary
                  .nativeQueueSegmentCount;
        }
        ValidateReads(pass, availableResources);
        ResourceStateTracker::PrepareAliasingBarriers(
            pass,
            executionState,
            compiledGraph,
            commandContext);
        ResourceStateTracker::PreparePassBarriers(
            pass,
            executionState,
            compiledGraph,
            commandContext);
        if (beginMarker)
        {
            beginMarker(pass.info.name);
        }

        {
            RHI::ScopedDebugLabel label(
                commandContext,
                pass.info.name);
            const Clock::time_point start =
                detailedProfilingEnabled
                    ? Clock::now()
                    : Clock::time_point{};
            if (pass.contextExecute)
            {
                pass.contextExecute(commandContext);
            }
            else
            {
                pass.execute();
            }
            RecordPassResult(pass, detailedProfilingEnabled, start);
        }

        if (endMarker)
        {
            endMarker(pass.info.name);
        }
        for (const std::string& resource : pass.writes)
        {
            availableResources.emplace(resource);
        }
        executionState.passInfos.push_back(pass.info);
    }

    if (nativeQueueExecution
        && activeQueue != GraphQueueClass::Graphics)
    {
        ResourceStateTracker::PrepareQueueHandoff(
            activeQueue,
            GraphQueueClass::Graphics,
            compiledGraph.summary.activePassCount,
            description,
            executionState,
            compiledGraph,
            commandContext);
        Core::Check(
            commandContext.SwitchCommandQueue(
                RHI::CommandQueueType::Graphics),
            "The RHI failed to return RenderGraph execution to the graphics queue.");
        ++compiledGraph.summary.nativeQueueSwitchCount;
        ++compiledGraph.summary.nativeQueueSegmentCount;
        activeQueue = GraphQueueClass::Graphics;
    }
    compiledGraph.summary.nativeMultiQueueSubmissionApplied =
        nativeQueueExecution
        && compiledGraph.summary.nativeQueueSwitchCount > 0;
    compiledGraph.summary.multiQueueGpuTimestamps =
        compiledGraph.summary.nativeMultiQueueSubmissionApplied;
    compiledGraph.summary.serialQueueExecution =
        !compiledGraph.summary.nativeMultiQueueSubmissionApplied;
}

void GraphExecutor::ValidateReads(
    const GraphPassDeclaration& pass,
    const std::unordered_set<std::string>& availableResources)
{
    for (const std::string& resource : pass.reads)
    {
        Core::Check(
            availableResources.contains(resource),
            "Render graph pass reads a resource before it is imported or produced.");
    }
}

void GraphExecutor::RecordPassResult(
    GraphPassDeclaration& pass,
    const bool detailedProfilingEnabled,
    const std::chrono::steady_clock::time_point& start)
{
    if (!detailedProfilingEnabled)
    {
        pass.info.cpuMilliseconds = 0.0f;
        return;
    }
    pass.info.cpuMilliseconds =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
}
} // namespace Prism::Renderer
