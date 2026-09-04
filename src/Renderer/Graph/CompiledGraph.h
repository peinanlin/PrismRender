#pragma once

#include "Renderer/Graph/GraphDescription.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace Prism::Renderer
{
struct GraphResourceDescription
{
    std::string name;
    RHI::ResourceState state;
    bool texture = false;
    bool buffer = false;
    bool imported = false;
    bool transient = false;
    bool history = false;
    std::uint32_t currentVersion = 0;
    std::size_t textureSubresourceCount = 0;
    std::size_t bufferStateRangeCount = 0;
    bool active = false;
    std::size_t firstUse = std::numeric_limits<std::size_t>::max();
    std::size_t lastUse = std::numeric_limits<std::size_t>::max();
    std::size_t physicalAllocation =
        std::numeric_limits<std::size_t>::max();
    std::uint64_t estimatedBytes = 0;
    std::uint64_t nativePoolId = 0;
    std::size_t nativeAllocation =
        std::numeric_limits<std::size_t>::max();
    std::uint64_t nativeAllocationBytes = 0;
};

struct GraphPassDescription
{
    struct Access
    {
        std::string resource;
        RHI::ResourceState state;
        std::string kind = "logical";
        std::uint32_t version = 0;
        TextureSubresourceRange textureRange;
        BufferRange bufferRange;
    };

    std::string name;
    float cpuMilliseconds = 0.0f;
    std::size_t originalIndex = 0;
    std::size_t executionIndex = std::numeric_limits<std::size_t>::max();
    bool culled = false;
    std::string cullReason;
    bool sideEffect = false;
    bool parallelRecordable = false;
    bool parallelRecordingAudited = false;
    GraphQueueClass queue = GraphQueueClass::Graphics;
    std::vector<std::size_t> dependencies;
    std::vector<Access> reads;
    std::vector<Access> writes;
};

struct GraphQueueSyncDescription
{
    std::size_t producerPass = 0;
    std::size_t consumerPass = 0;
    GraphQueueClass producerQueue = GraphQueueClass::Graphics;
    GraphQueueClass consumerQueue = GraphQueueClass::Graphics;
    std::vector<std::string> resources;
};

struct GraphQueueBatchDescription
{
    std::size_t batchIndex = 0;
    GraphQueueClass queue = GraphQueueClass::Graphics;
    std::vector<std::size_t> passes;
    std::vector<std::size_t> dependencies;
    std::size_t firstExecutionIndex =
        std::numeric_limits<std::size_t>::max();
    std::size_t lastExecutionIndex =
        std::numeric_limits<std::size_t>::max();
};

struct GraphCompilationSummary
{
    bool compiled = false;
    bool compilationCacheHit = false;
    bool passCullingEnabled = false;
    bool serialQueueExecution = true;
    bool nativeComputeQueueAvailable = false;
    bool dedicatedComputeQueueAvailable = false;
    bool timelineQueueSynchronization = false;
    bool nativeMultiQueueSubmissionApplied = false;
    bool independentQueueBatchSubmissionAvailable = false;
    bool deferredQueueBatchSubmissionAvailable = false;
    bool dagQueueBatchExecutionApplied = false;
    bool deferredQueueBatchSubmissionApplied = false;
    bool parallelCommandRecordingApplied = false;
    bool nativeParallelCommandRecordingApplied = false;
    bool multiQueueGpuTimestamps = false;
    std::size_t parallelRecordedPassCount = 0;
    std::size_t nativeParallelRecordedPassCount = 0;
    std::size_t recordedCommandCount = 0;
    QueueSchedulingDecision automaticQueueDecision;
    GraphQueueExecutionMode requestedQueueExecutionMode =
        GraphQueueExecutionMode::Automatic;
    std::size_t nativeQueueSwitchCount = 0;
    std::size_t nativeQueueSegmentCount = 1;
    std::size_t queueBatchCount = 0;
    std::size_t crossQueueBatchDependencyCount = 0;
    std::size_t overlapOpportunityCount = 0;
    std::size_t declaredPassCount = 0;
    std::size_t activePassCount = 0;
    std::size_t culledPassCount = 0;
    std::size_t registeredTextureCount = 0;
    std::size_t registeredBufferCount = 0;
    std::size_t registeredTextureViewCount = 0;
    std::size_t historyResourceCount = 0;
    std::size_t versionedResourceCount = 0;
    std::size_t parameterPassCount = 0;
    std::size_t textureSubresourceCount = 0;
    std::size_t bufferStateRangeCount = 0;
    std::size_t transientResourceCount = 0;
    std::size_t transientAllocationCount = 0;
    std::uint64_t transientLogicalBytes = 0;
    std::uint64_t transientPhysicalBytes = 0;
    std::uint64_t transientAliasedBytes = 0;
    bool nativeTransientAliasingApplied = false;
    bool nativeTransientAliasingPlanValid = false;
    std::size_t nativeTransientAllocationCount = 0;
    std::uint64_t nativeTransientLogicalBytes = 0;
    std::uint64_t nativeTransientPhysicalBytes = 0;
    std::uint64_t nativeTransientAliasedBytes = 0;
    std::size_t expectedAliasingBarrierCount = 0;
    std::size_t executedAliasingBarrierCount = 0;
    std::size_t crossQueueSyncCount = 0;
};

struct GraphCompiledResource
{
    std::string name;
    bool texture = false;
    bool buffer = false;
    bool imported = false;
    bool transient = false;
    bool history = false;
    std::uint32_t currentVersion = 0;
    bool active = false;
    RHI::ResourceState state = RHI::ResourceState::Undefined;
    std::size_t firstUse = std::numeric_limits<std::size_t>::max();
    std::size_t lastUse = std::numeric_limits<std::size_t>::max();
    std::size_t physicalAllocation =
        std::numeric_limits<std::size_t>::max();
    std::uint64_t estimatedBytes = 0;
    std::uint64_t nativePoolId = 0;
    std::size_t nativeAllocation =
        std::numeric_limits<std::size_t>::max();
    std::uint64_t nativeAllocationBytes = 0;
};

// A compiled pass deliberately contains no callback or live RHI pointer.
struct GraphCompiledPass
{
    std::size_t executionIndex = std::numeric_limits<std::size_t>::max();
    bool active = true;
    std::string cullReason;
    std::vector<std::size_t> livenessDependencies;
    std::vector<std::size_t> executionDependencies;
};

struct CompiledGraph
{
    std::uint64_t declarationFingerprint = 0;
    std::vector<GraphCompiledPass> passes;
    std::unordered_map<std::string, GraphCompiledResource> compiledResources;
    std::vector<GraphQueueSyncDescription> queueSyncDescriptions;
    std::vector<GraphQueueBatchDescription> queueBatchDescriptions;
    std::vector<std::size_t> queueBatchSubmissionOrder;
    std::vector<std::size_t> passToQueueBatch;
    std::unordered_map<std::string, std::vector<std::string>>
        nativeAliasGroups;
    GraphCompilationSummary summary;
    std::string graphSignature;
    bool valid = false;

    void Clear()
    {
        declarationFingerprint = 0;
        passes.clear();
        compiledResources.clear();
        queueSyncDescriptions.clear();
        queueBatchDescriptions.clear();
        queueBatchSubmissionOrder.clear();
        passToQueueBatch.clear();
        nativeAliasGroups.clear();
        summary = {};
        graphSignature.clear();
        valid = false;
    }
};
} // namespace Prism::Renderer
