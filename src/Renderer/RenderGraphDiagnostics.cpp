#include "Renderer/RenderGraphDiagnostics.h"

#include "Renderer/RenderGraph.h"
#include "RHI/DeviceCapabilities.h"
#include "RHI/GraphicsApi.h"
#include "RHI/GraphicsTypes.h"

#include <fstream>
#include <limits>
#include <ranges>
#include <stdexcept>

namespace Prism::Renderer
{
namespace
{
std::string_view ToString(const RHI::ResourceState state)
{
    switch (state)
    {
    case RHI::ResourceState::Undefined: return "Undefined";
    case RHI::ResourceState::Common: return "Common";
    case RHI::ResourceState::Present: return "Present";
    case RHI::ResourceState::RenderTarget: return "RenderTarget";
    case RHI::ResourceState::DepthWrite: return "DepthWrite";
    case RHI::ResourceState::DepthRead: return "DepthRead";
    case RHI::ResourceState::ShaderResource: return "ShaderResource";
    case RHI::ResourceState::UnorderedAccess: return "UnorderedAccess";
    case RHI::ResourceState::CopySource: return "CopySource";
    case RHI::ResourceState::CopyDestination: return "CopyDestination";
    case RHI::ResourceState::VertexBuffer: return "VertexBuffer";
    case RHI::ResourceState::IndexBuffer: return "IndexBuffer";
    case RHI::ResourceState::ConstantBuffer: return "ConstantBuffer";
    case RHI::ResourceState::IndirectArgument: return "IndirectArgument";
    }
    return "Unknown";
}

std::string_view ToString(
    const RenderGraph::QueueClass queue)
{
    switch (queue)
    {
    case RenderGraph::QueueClass::Graphics:
        return "Graphics";
    case RenderGraph::QueueClass::Compute:
        return "Compute";
    }
    return "Unknown";
}

std::string_view ToString(
    const RenderGraph::QueueExecutionMode mode)
{
    switch (mode)
    {
    case RenderGraph::QueueExecutionMode::Automatic:
        return "auto";
    case RenderGraph::QueueExecutionMode::Serial:
        return "serial";
    case RenderGraph::QueueExecutionMode::Native:
        return "native";
    }
    return "unknown";
}

nlohmann::json SerializeIndex(
    const std::size_t index)
{
    return index == std::numeric_limits<std::size_t>::max()
        ? nlohmann::json(nullptr)
        : nlohmann::json(index);
}
} // namespace

nlohmann::json BuildRenderGraphReport(
    const RenderGraph& graph,
    const RHI::GraphicsApi graphicsApi,
    const RHI::GraphicsDeviceCapabilities*
        deviceCapabilities,
    const RHI::DescriptorAllocatorStatistics*
        descriptorStatistics,
    const RHI::UploadQueueStatistics*
        uploadStatistics,
    const RHI::ResourceRetirementStatistics*
        retirementStatistics)
{
    nlohmann::json resources = nlohmann::json::array();
    for (const RenderGraph::ResourceDescription& resource : graph.GetResourceDescriptions())
    {
        resources.push_back({
            {"name", resource.name},
            {"state", std::string(ToString(resource.state))},
            {"stateBits", static_cast<std::uint32_t>(resource.state)},
            {"kind",
             resource.texture
                 ? "texture"
                 : resource.buffer
                     ? "buffer"
                     : "logical"},
            {"imported", resource.imported},
            {"transient", resource.transient},
            {"history", resource.history},
            {"currentVersion",
             resource.currentVersion},
            {"textureSubresourceCount",
             resource.textureSubresourceCount},
            {"bufferStateRangeCount",
             resource.bufferStateRangeCount},
            {"active", resource.active},
            {"firstUse", SerializeIndex(resource.firstUse)},
            {"lastUse", SerializeIndex(resource.lastUse)},
            {"physicalAllocation",
             SerializeIndex(resource.physicalAllocation)},
            {"estimatedBytes", resource.estimatedBytes},
            {"nativePoolId",
             resource.nativePoolId == 0
                 ? nlohmann::json(nullptr)
                 : nlohmann::json(resource.nativePoolId)},
            {"nativeAllocation",
             SerializeIndex(resource.nativeAllocation)},
            {"nativeAllocationBytes",
             resource.nativeAllocationBytes}});
    }

    nlohmann::json passes = nlohmann::json::array();
    for (const RenderGraph::PassDescription& pass : graph.GetPassDescriptions())
    {
        const auto serializeAccesses = [](const std::vector<RenderGraph::PassDescription::Access>& accesses)
        {
            nlohmann::json result = nlohmann::json::array();
            for (const RenderGraph::PassDescription::Access& access : accesses)
            {
                result.push_back({
                    {"resource", access.resource},
                    {"state",
                     std::string(ToString(access.state))},
                    {"stateBits", static_cast<std::uint32_t>(access.state)},
                    {"kind", access.kind},
                    {"version", access.version},
                    {"textureRange", {
                        {"baseMipLevel",
                         access.textureRange.baseMipLevel},
                        {"mipLevelCount",
                         access.textureRange.mipLevelCount},
                        {"baseArrayLayer",
                         access.textureRange.baseArrayLayer},
                        {"arrayLayerCount",
                         access.textureRange.arrayLayerCount}}},
                    {"bufferRange", {
                        {"offset", access.bufferRange.offset},
                        {"size", access.bufferRange.size}}}});
            }
            return result;
        };
        passes.push_back({
            {"name", pass.name},
            {"cpuMilliseconds", pass.cpuMilliseconds},
            {"originalIndex", pass.originalIndex},
            {"executionIndex",
             SerializeIndex(pass.executionIndex)},
            {"culled", pass.culled},
            {"cullReason", pass.cullReason},
            {"sideEffect", pass.sideEffect},
            {"parallelRecordable",
             pass.parallelRecordable},
            {"parallelRecordingAudited",
             pass.parallelRecordingAudited},
            {"queue", std::string(ToString(pass.queue))},
            {"dependencies", pass.dependencies},
            {"reads", serializeAccesses(pass.reads)},
            {"writes", serializeAccesses(pass.writes)}});
    }

    nlohmann::json queueSync =
        nlohmann::json::array();
    for (const RenderGraph::QueueSyncDescription& sync :
         graph.GetQueueSyncDescriptions())
    {
        queueSync.push_back({
            {"producerPass", sync.producerPass},
            {"consumerPass", sync.consumerPass},
            {"producerQueue",
             std::string(ToString(sync.producerQueue))},
            {"consumerQueue",
             std::string(ToString(sync.consumerQueue))},
            {"resources", sync.resources}});
    }

    const std::vector<RenderGraph::PassDescription>
        passDescriptions =
            graph.GetPassDescriptions();
    nlohmann::json queueBatches =
        nlohmann::json::array();
    for (const RenderGraph::QueueBatchDescription& batch :
         graph.GetQueueBatchDescriptions())
    {
        nlohmann::json batchPasses =
            nlohmann::json::array();
        for (const std::size_t passIndex :
             batch.passes)
        {
            const auto pass = std::ranges::find(
                passDescriptions,
                passIndex,
                &RenderGraph::PassDescription::originalIndex);
            batchPasses.push_back(
                pass != passDescriptions.end()
                    ? nlohmann::json(pass->name)
                    : nlohmann::json(passIndex));
        }
        queueBatches.push_back({
            {"batchIndex", batch.batchIndex},
            {"queue",
             std::string(ToString(batch.queue))},
            {"passes", std::move(batchPasses)},
            {"dependencies", batch.dependencies},
            {"firstExecutionIndex",
             SerializeIndex(batch.firstExecutionIndex)},
            {"lastExecutionIndex",
             SerializeIndex(batch.lastExecutionIndex)}});
    }

    const RenderGraph::CompilationSummary& summary =
        graph.GetCompilationSummary();
    nlohmann::json rhi = {
        {"deviceCapabilities", nullptr},
        {"descriptorAllocator", nullptr},
        {"uploadQueue", nullptr},
        {"resourceRetirement", nullptr}};
    if (deviceCapabilities != nullptr)
    {
        const RHI::DeviceLimits& limits =
            deviceCapabilities->limits;
        const RHI::DeviceFeatures& features =
            deviceCapabilities->features;
        rhi["deviceCapabilities"] = {
            {"graphicsApi",
             std::string(RHI::ToString(
                 deviceCapabilities->graphicsApi))},
            {"adapterName",
             deviceCapabilities->adapterName},
            {"limits", {
                {"maxTextureDimension2D",
                 limits.maxTextureDimension2D},
                {"maxTextureArrayLayers",
                 limits.maxTextureArrayLayers},
                {"maxColorAttachments",
                 limits.maxColorAttachments},
                {"maxSamplerAnisotropy",
                 limits.maxSamplerAnisotropy},
                {"minConstantBufferOffsetAlignment",
                 limits.minConstantBufferOffsetAlignment},
                {"minStorageBufferOffsetAlignment",
                 limits.minStorageBufferOffsetAlignment},
                {"shaderVisibleResourceDescriptorCapacity",
                 limits.shaderVisibleResourceDescriptorCapacity},
                {"shaderVisibleSamplerDescriptorCapacity",
                 limits.shaderVisibleSamplerDescriptorCapacity},
                {"maxRayRecursionDepth",
                 limits.maxRayRecursionDepth},
                {"rayTracingShaderGroupHandleSize",
                 limits.rayTracingShaderGroupHandleSize},
                {"rayTracingShaderGroupBaseAlignment",
                 limits.rayTracingShaderGroupBaseAlignment},
                {"accelerationStructureScratchAlignment",
                 limits.accelerationStructureScratchAlignment}}},
            {"rayTracingTier",
             std::string(RHI::ToString(
                 deviceCapabilities->rayTracingTier))},
            {"features", {
                {"graphicsQueue", features.graphicsQueue},
                {"computeQueue", features.computeQueue},
                {"dedicatedComputeQueue",
                 features.dedicatedComputeQueue},
                {"copyQueue", features.copyQueue},
                {"dedicatedCopyQueue",
                 features.dedicatedCopyQueue},
                {"timelineSynchronization",
                 features.timelineSynchronization},
                {"dynamicRendering",
                 features.dynamicRendering},
                {"descriptorIndexing",
                 features.descriptorIndexing},
                {"bufferDeviceAddress",
                 features.bufferDeviceAddress},
                {"drawIndirectCount",
                 features.drawIndirectCount},
                {"samplerAnisotropy",
                 features.samplerAnisotropy},
                {"nativeParallelCommandRecording",
                 features.nativeParallelCommandRecording},
                {"rayTracingAccelerationStructure",
                 features.rayTracingAccelerationStructure},
                {"rayTracingPipeline",
                 features.rayTracingPipeline},
                {"rayQuery",
                 features.rayQuery}}}};
    }
    if (descriptorStatistics != nullptr)
    {
        rhi["descriptorAllocator"] = {
            {"poolCount",
             descriptorStatistics->poolCount},
            {"setCapacity",
             descriptorStatistics->setCapacity},
            {"allocatedSetCount",
             descriptorStatistics->allocatedSetCount},
            {"setHighWatermark",
             descriptorStatistics->setHighWatermark},
            {"pendingReleaseCount",
             descriptorStatistics->pendingReleaseCount},
            {"resourceDescriptorCapacity",
             descriptorStatistics
                 ->resourceDescriptorCapacity},
            {"allocatedResourceDescriptorCount",
             descriptorStatistics
                 ->allocatedResourceDescriptorCount},
            {"resourceDescriptorHighWatermark",
             descriptorStatistics
                 ->resourceDescriptorHighWatermark},
            {"samplerDescriptorCapacity",
             descriptorStatistics
                 ->samplerDescriptorCapacity},
            {"allocatedSamplerDescriptorCount",
             descriptorStatistics
                 ->allocatedSamplerDescriptorCount},
            {"samplerDescriptorHighWatermark",
             descriptorStatistics
                 ->samplerDescriptorHighWatermark}};
    }
    if (uploadStatistics != nullptr)
    {
        rhi["uploadQueue"] = {
            {"uploadedBytes",
             uploadStatistics->uploadedBytes},
            {"submittedBatchCount",
             uploadStatistics->submittedBatchCount},
            {"synchronousFlushCount",
             uploadStatistics->synchronousFlushCount},
            {"maximumBatchBytes",
             uploadStatistics->maximumBatchBytes},
            {"pendingOperationCount",
             uploadStatistics->pendingOperationCount},
            {"pendingBytes",
             uploadStatistics->pendingBytes},
            {"pendingTicket",
             uploadStatistics->pendingTicket},
            {"lastSubmittedTicket",
             uploadStatistics->lastSubmittedTicket},
            {"completedTicket",
             uploadStatistics->completedTicket},
            {"outstandingBatchCount",
             uploadStatistics->outstandingBatchCount},
            {"stagingPageCount",
             uploadStatistics->stagingPageCount},
            {"stagingCapacityBytes",
             uploadStatistics->stagingCapacityBytes},
            {"stagingHighWatermarkBytes",
             uploadStatistics->stagingHighWatermarkBytes}};
    }
    if (retirementStatistics != nullptr)
    {
        rhi["resourceRetirement"] = {
            {"totalRetiredObjectCount",
             retirementStatistics
                 ->totalRetiredObjectCount},
            {"totalReclaimedObjectCount",
             retirementStatistics
                 ->totalReclaimedObjectCount},
            {"pendingObjectCount",
             retirementStatistics
                 ->pendingObjectCount},
            {"pendingObjectHighWatermark",
             retirementStatistics
                 ->pendingObjectHighWatermark}};
    }
    return {
        {"format", "PrismRenderGraphReport"},
        {"version", 11},
        {"graphicsApi", std::string(RHI::ToString(graphicsApi))},
        {"rhi", std::move(rhi)},
        {"graphSignature", graph.GetGraphSignature()},
        {"compilation", {
            {"compiled", summary.compiled},
            {"compilationCacheHit",
             summary.compilationCacheHit},
            {"passCullingEnabled",
             summary.passCullingEnabled},
            {"declaredPassCount",
             summary.declaredPassCount},
            {"activePassCount",
             summary.activePassCount},
            {"culledPassCount",
             summary.culledPassCount},
            {"resourceModel", {
                {"registeredTextureCount",
                 summary.registeredTextureCount},
                {"registeredBufferCount",
                 summary.registeredBufferCount},
                {"registeredTextureViewCount",
                 summary.registeredTextureViewCount},
                {"historyResourceCount",
                 summary.historyResourceCount},
                {"versionedResourceCount",
                 summary.versionedResourceCount},
                {"parameterPassCount",
                 summary.parameterPassCount},
                {"textureSubresourceCount",
                 summary.textureSubresourceCount},
                {"bufferStateRangeCount",
                 summary.bufferStateRangeCount}}},
            {"queueExecutionMode",
             summary.dagQueueBatchExecutionApplied
                 ? "dag_multi_queue"
                 : summary.serialQueueExecution
                     ? "serial_fallback"
                     : "native_multi_queue"},
            {"requestedQueueExecutionMode",
             std::string(ToString(
                 summary.requestedQueueExecutionMode))},
            {"queueInfrastructure", {
                {"nativeComputeQueueAvailable",
                 summary.nativeComputeQueueAvailable},
                {"dedicatedComputeQueueAvailable",
                 summary.dedicatedComputeQueueAvailable},
                {"timelineSynchronization",
                 summary.timelineQueueSynchronization},
                {"nativeQueueSwitching",
                 summary.nativeMultiQueueSubmissionApplied
                     || summary.nativeQueueSwitchCount > 0},
                {"independentBatchSubmission",
                 summary
                     .independentQueueBatchSubmissionAvailable},
                {"deferredBatchSubmission",
                 summary
                     .deferredQueueBatchSubmissionAvailable},
                {"dagQueueBatchExecutionApplied",
                 summary.dagQueueBatchExecutionApplied},
                {"deferredBatchSubmissionApplied",
                 summary
                     .deferredQueueBatchSubmissionApplied},
                {"parallelCommandRecordingApplied",
                 summary.parallelCommandRecordingApplied},
                {"nativeParallelCommandRecordingApplied",
                 summary
                     .nativeParallelCommandRecordingApplied},
                {"parallelRecordedPassCount",
                 summary.parallelRecordedPassCount},
                {"nativeParallelRecordedPassCount",
                 summary
                     .nativeParallelRecordedPassCount},
                {"recordedCommandCount",
                 summary.recordedCommandCount},
                {"nativeMultiQueueSubmissionApplied",
                 summary.nativeMultiQueueSubmissionApplied},
                {"multiQueueGpuTimestamps",
                 summary.multiQueueGpuTimestamps},
                {"queueSwitchCount",
                 summary.nativeQueueSwitchCount},
                {"queueSegmentCount",
                 summary.nativeQueueSegmentCount}}},
            {"queueBatchPlan", {
                {"batchCount",
                 summary.queueBatchCount},
                {"crossQueueDependencyCount",
                 summary
                     .crossQueueBatchDependencyCount},
                {"overlapOpportunityCount",
                 summary.overlapOpportunityCount}}},
            {"automaticQueueDecision", {
                {"historyAvailable",
                 summary.automaticQueueDecision
                     .historyAvailable},
                {"exactHistoryAvailable",
                 summary.automaticQueueDecision
                     .exactHistoryAvailable},
                {"predictionAvailable",
                 summary.automaticQueueDecision
                     .predictionAvailable},
                {"predictedFromPassHistory",
                 summary.automaticQueueDecision
                     .predictedFromPassHistory},
                {"controlledExploration",
                 summary.automaticQueueDecision
                     .controlledExploration},
                {"historyExpired",
                 summary.automaticQueueDecision
                     .historyExpired},
                {"selectNative",
                 summary.automaticQueueDecision
                     .selectNative},
                {"reason",
                 summary.automaticQueueDecision.reason},
                {"predictionSource",
                 summary.automaticQueueDecision
                     .predictionSource},
                {"serialSamples",
                 summary.automaticQueueDecision
                     .serialSamples},
                {"nativeSamples",
                 summary.automaticQueueDecision
                     .nativeSamples},
                {"estimatedSerialMilliseconds",
                 summary.automaticQueueDecision
                     .estimatedSerialMilliseconds},
                {"estimatedNativeMilliseconds",
                 summary.automaticQueueDecision
                     .estimatedNativeMilliseconds},
                {"serialP50Milliseconds",
                 summary.automaticQueueDecision
                     .serialP50Milliseconds},
                {"serialP95Milliseconds",
                 summary.automaticQueueDecision
                     .serialP95Milliseconds},
                {"nativeP50Milliseconds",
                 summary.automaticQueueDecision
                     .nativeP50Milliseconds},
                {"nativeP95Milliseconds",
                 summary.automaticQueueDecision
                     .nativeP95Milliseconds},
                {"estimatedNetBenefitMilliseconds",
                 summary.automaticQueueDecision
                     .estimatedNetBenefitMilliseconds},
                {"estimatedSubmissionCostMilliseconds",
                 summary.automaticQueueDecision
                     .estimatedSubmissionCostMilliseconds},
                {"estimatedOverlapMilliseconds",
                 summary.automaticQueueDecision
                     .estimatedOverlapMilliseconds},
                {"requiredBenefitMilliseconds",
                 summary.automaticQueueDecision
                     .requiredBenefitMilliseconds},
                {"confidence",
                 summary.automaticQueueDecision
                     .confidence}}},
            {"crossQueueSyncCount",
             summary.crossQueueSyncCount},
            {"transient", {
                {"resourceCount",
                 summary.transientResourceCount},
                {"allocationCount",
                 summary.transientAllocationCount},
                {"logicalBytes",
                 summary.transientLogicalBytes},
                {"physicalBytes",
                 summary.transientPhysicalBytes},
                {"aliasedBytes",
                 summary.transientAliasedBytes},
                {"native", {
                    {"planValid",
                     summary.nativeTransientAliasingPlanValid},
                    {"physicalAliasingApplied",
                     summary.nativeTransientAliasingApplied},
                    {"allocationCount",
                     summary.nativeTransientAllocationCount},
                    {"logicalBytes",
                     summary.nativeTransientLogicalBytes},
                    {"physicalBytes",
                     summary.nativeTransientPhysicalBytes},
                    {"aliasedBytes",
                     summary.nativeTransientAliasedBytes},
                    {"expectedAliasingBarrierCount",
                     summary.expectedAliasingBarrierCount},
                    {"executedAliasingBarrierCount",
                     summary.executedAliasingBarrierCount}}}}}}},
        {"resources", std::move(resources)},
        {"passes", std::move(passes)},
        {"queueBatches", std::move(queueBatches)},
        {"queueSync", std::move(queueSync)}};
}

bool WriteRenderGraphReport(
    const std::filesystem::path& path,
    const RenderGraph& graph,
    const RHI::GraphicsApi graphicsApi,
    const RHI::GraphicsDeviceCapabilities*
        deviceCapabilities,
    const RHI::DescriptorAllocatorStatistics*
        descriptorStatistics,
    const RHI::UploadQueueStatistics*
        uploadStatistics,
    const RHI::ResourceRetirementStatistics*
        retirementStatistics,
    std::string* outError)
{
    try
    {
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not open the RenderGraph report for writing.");
        output << BuildRenderGraphReport(
            graph,
            graphicsApi,
            deviceCapabilities,
            descriptorStatistics,
            uploadStatistics,
            retirementStatistics).dump(2) << '\n';
        if (!output) throw std::runtime_error("Failed while writing the RenderGraph report.");
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr) *outError = exception.what();
        return false;
    }
}

bool WriteRenderGraphReport(
    const std::filesystem::path& path,
    const nlohmann::json& report,
    std::string* outError)
{
    try
    {
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not open the RenderGraph report for writing.");
        }
        output << report.dump(2) << '\n';
        if (!output)
        {
            throw std::runtime_error(
                "Failed while writing the RenderGraph report.");
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr)
        {
            *outError = exception.what();
        }
        return false;
    }
}
} // namespace Prism::Renderer
