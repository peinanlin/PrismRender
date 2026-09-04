#include "Core/Threading/TaskScheduler.h"
#include "Renderer/RenderGraph.h"
#include "Renderer/RenderGraphDiagnostics.h"
#include "Renderer/DemoSceneSettings.h"
#include "Renderer/Features/ClusteredLighting.h"
#include "Renderer/Features/FftOcean.h"
#include "Renderer/Features/GpuDrivenVisibility.h"
#include "Renderer/Features/InteractiveTerrain.h"
#include "Renderer/Features/LocalLightShadows.h"
#include "Renderer/Features/PlanarReflections.h"
#include "Renderer/Features/ScreenSpaceEffects.h"
#include "Renderer/Features/SkyAtmosphere.h"
#include "Renderer/Features/TemporalAntiAliasing.h"
#include "Renderer/Features/VarianceShadowMaps.h"
#include "Renderer/Pipeline/ScenePipelinePlan.h"
#include "Renderer/DeferredTransientLayout.h"
#include "Renderer/RhiPasses.h"
#include "Renderer/SharedRenderGraphFrontend.h"
#include "Renderer/Features/Ocean/SpectralOceanSimulation.h"
#include "Renderer/Features/Ocean/WaterOpticsFeature.h"
#include "Renderer/Features/Ocean/WaterOpticsTransientLayout.h"
#include "RHI/DeferredCommandContext.h"
#include "RHI/DeviceCapabilities.h"
#include "RHI/ICommandContext.h"
#include "Scene/RenderSceneView.h"
#include "Scene/DemoSceneCatalog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
class MockParallelCommandRecording;

void Expect(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class TestEvent
{
public:
    void Signal()
    {
        {
            std::lock_guard lock(m_mutex);
            m_signaled = true;
        }
        m_condition.notify_all();
    }

    void Wait()
    {
        std::unique_lock lock(m_mutex);
        m_condition.wait(lock, [this] { return m_signaled; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_signaled = false;
};

class MockCommandContext final : public Prism::RHI::ICommandContext
{
public:
    Prism::RHI::GraphicsApi GetGraphicsApi() const override
    {
        return Prism::RHI::GraphicsApi::Vulkan;
    }
    Prism::RHI::CommandQueueCapabilities
        GetQueueCapabilities() const override
    {
        return {
            true,
            nativeQueueSwitching,
            nativeQueueSwitching,
            nativeQueueSwitching,
            nativeQueueSwitching,
            independentBatchSubmission,
            deferredBatchSubmission,
            nativeParallelRecording};
    }
    Prism::RHI::CommandQueueType
        GetActiveCommandQueue() const override
    {
        return activeQueue;
    }
    bool SwitchCommandQueue(
        const Prism::RHI::CommandQueueType queue) override
    {
        if (!nativeQueueSwitching)
        {
            return queue
                == Prism::RHI::CommandQueueType::Graphics;
        }
        activeQueue = queue;
        queueSwitches.push_back(queue);
        return true;
    }
    bool BeginQueueBatch(
        const Prism::RHI::CommandQueueType queue,
        const std::span<
            const Prism::RHI::QueueSyncPoint> waits) override
    {
        if (!independentBatchSubmission)
        {
            return false;
        }
        activeQueue = queue;
        batchQueues.push_back(queue);
        batchWaits.emplace_back(
            waits.begin(),
            waits.end());
        batchOpen = true;
        return true;
    }
    Prism::RHI::QueueSyncPoint
        EndQueueBatch() override
    {
        Expect(
            batchOpen,
            "The mock queue batch was not open.");
        batchOpen = false;
        const std::size_t queueIndex =
            activeQueue
                    == Prism::RHI::CommandQueueType::Compute
                ? 1u
                : 0u;
        Prism::RHI::QueueSyncPoint signal{
            activeQueue,
            nextBatchValues[queueIndex]++};
        batchSignals.push_back(signal);
        return signal;
    }
    bool FlushQueueBatches() override
    {
        if (!deferredBatchSubmission
            || batchOpen)
        {
            return false;
        }
        batchesFlushed = true;
        return true;
    }
    bool ResumeGraphicsQueue(
        const std::span<
            const Prism::RHI::QueueSyncPoint> waits) override
    {
        if (!independentBatchSubmission
            || batchOpen)
        {
            return false;
        }
        activeQueue =
            Prism::RHI::CommandQueueType::Graphics;
        continuationWaits.assign(
            waits.begin(),
            waits.end());
        graphicsResumed = true;
        return true;
    }
    std::unique_ptr<
        Prism::RHI::IParallelCommandRecording>
        CreateParallelCommandRecording(
            Prism::RHI::CommandQueueType queue) override;
    bool AppendParallelCommandRecording(
        std::unique_ptr<
            Prism::RHI::IParallelCommandRecording>
            recording) override;

    void BeginRendering(const Prism::RHI::RenderingInfo&) override { ++renderingScopes; }
    void EndRendering() override { ++renderingEnds; }
    void BindGraphicsPipeline(const Prism::RHI::IGraphicsPipeline&) override { ++graphicsPipelineBinds; }
    void BindComputePipeline(const Prism::RHI::IComputePipeline&) override { ++computePipelineBinds; }
    void BindVertexBuffer(const Prism::RHI::IBuffer&, std::uint32_t) override { ++vertexBufferBinds; }
    void BindIndexBuffer(const Prism::RHI::IBuffer&, Prism::RHI::IndexFormat) override { ++indexBufferBinds; }
    void BindDescriptorSet(
        const Prism::RHI::IDescriptorSet&,
        const std::span<const Prism::RHI::DynamicBufferOffset> dynamicOffsets) override
    {
        ++descriptorSetBinds;
        lastDynamicOffsets.assign(dynamicOffsets.begin(), dynamicOffsets.end());
    }
    void DrawIndexed(std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t, std::uint32_t) override
    {
        ++indexedDraws;
    }
    void DrawIndexedIndirect(
        const Prism::RHI::IBuffer& argumentBuffer,
        std::size_t argumentOffset,
        std::uint32_t maxDrawCount,
        std::uint32_t stride,
        const Prism::RHI::IBuffer* countBuffer,
        std::size_t countOffset) override
    {
        ++indexedIndirectDraws;
        lastIndirectArgumentBuffer =
            &argumentBuffer;
        lastIndirectArgumentOffset =
            argumentOffset;
        lastIndirectMaxDrawCount =
            maxDrawCount;
        lastIndirectStride = stride;
        lastIndirectCountBuffer =
            countBuffer;
        lastIndirectCountOffset =
            countOffset;
    }
    void Draw(std::uint32_t vertexCount, std::uint32_t, std::uint32_t, std::uint32_t) override
    {
        ++draws;
        lastVertexCount = vertexCount;
    }
    void Dispatch(std::uint32_t, std::uint32_t, std::uint32_t) override { ++dispatches; }
    void CopyBuffer(
        const Prism::RHI::IBuffer&,
        Prism::RHI::IBuffer&,
        std::size_t,
        std::size_t,
        std::size_t) override
    {
        ++bufferCopies;
    }
    void TextureBarrier(const Prism::RHI::TextureBarrier& barrier) override
    {
        ++textureBarriers;
        barriers.push_back(barrier);
        barrierQueues.push_back(activeQueue);
    }
    void BufferBarrier(
        const Prism::RHI::BufferBarrier& barrier) override
    {
        ++bufferBarriers;
        lastBufferBarrier = barrier;
        bufferBarrierHistory.push_back(barrier);
    }
    void GlobalBarrier(
        const Prism::RHI::GlobalBarrier& barrier) override
    {
        ++memoryBarriers;
        lastMemoryBarrier = barrier;
    }
    void TextureViewBarrier(
        const Prism::RHI::ITextureView&,
        Prism::RHI::ResourceState,
        Prism::RHI::ResourceState) override
    {
        ++textureViewBarriers;
    }
    void TextureAliasingBarrier(
        const Prism::RHI::ITexture* before,
        Prism::RHI::ITexture& after) override
    {
        ++aliasingBarriers;
        aliasingBarrierSources.push_back(before);
        aliasingBarrierDestinations.push_back(&after);
    }
    void BufferAliasingBarrier(
        const Prism::RHI::IBuffer* before,
        Prism::RHI::IBuffer& after) override
    {
        ++bufferAliasingBarriers;
        bufferAliasingBarrierSources.push_back(before);
        bufferAliasingBarrierDestinations.push_back(&after);
    }

    std::uint32_t graphicsPipelineBinds = 0;
    std::uint32_t renderingScopes = 0;
    std::uint32_t renderingEnds = 0;
    std::uint32_t computePipelineBinds = 0;
    std::uint32_t vertexBufferBinds = 0;
    std::uint32_t indexBufferBinds = 0;
    std::uint32_t descriptorSetBinds = 0;
    std::uint32_t indexedDraws = 0;
    std::uint32_t indexedIndirectDraws = 0;
    std::uint32_t bufferCopies = 0;
    std::uint32_t draws = 0;
    std::uint32_t dispatches = 0;
    const Prism::RHI::IBuffer*
        lastIndirectArgumentBuffer = nullptr;
    std::size_t lastIndirectArgumentOffset = 0;
    std::uint32_t lastIndirectMaxDrawCount = 0;
    std::uint32_t lastIndirectStride = 0;
    const Prism::RHI::IBuffer*
        lastIndirectCountBuffer = nullptr;
    std::size_t lastIndirectCountOffset = 0;
    std::uint32_t textureBarriers = 0;
    std::uint32_t bufferBarriers = 0;
    std::uint32_t memoryBarriers = 0;
    std::uint32_t textureViewBarriers = 0;
    std::uint32_t aliasingBarriers = 0;
    std::uint32_t bufferAliasingBarriers = 0;
    std::uint32_t lastVertexCount = 0;
    std::vector<Prism::RHI::DynamicBufferOffset> lastDynamicOffsets;
    std::vector<Prism::RHI::TextureBarrier> barriers;
    std::vector<Prism::RHI::CommandQueueType> barrierQueues;
    Prism::RHI::BufferBarrier lastBufferBarrier{};
    std::vector<Prism::RHI::BufferBarrier>
        bufferBarrierHistory;
    Prism::RHI::GlobalBarrier lastMemoryBarrier{};
    std::vector<const Prism::RHI::ITexture*>
        aliasingBarrierSources;
    std::vector<const Prism::RHI::ITexture*>
        aliasingBarrierDestinations;
    std::vector<const Prism::RHI::IBuffer*>
        bufferAliasingBarrierSources;
    std::vector<const Prism::RHI::IBuffer*>
        bufferAliasingBarrierDestinations;
    bool nativeQueueSwitching = false;
    bool independentBatchSubmission = false;
    bool deferredBatchSubmission = false;
    bool nativeParallelRecording = false;
    bool batchOpen = false;
    bool batchesFlushed = false;
    bool graphicsResumed = false;
    Prism::RHI::CommandQueueType activeQueue =
        Prism::RHI::CommandQueueType::Graphics;
    std::vector<Prism::RHI::CommandQueueType>
        queueSwitches;
    std::vector<Prism::RHI::CommandQueueType>
        batchQueues;
    std::vector<std::vector<
        Prism::RHI::QueueSyncPoint>> batchWaits;
    std::vector<Prism::RHI::QueueSyncPoint>
        batchSignals;
    std::vector<Prism::RHI::QueueSyncPoint>
        continuationWaits;
    std::array<std::uint64_t, 2>
        nextBatchValues{1, 1};
    std::size_t appendedNativeRecordings = 0;
};

class MockParallelCommandRecording final
    : public Prism::RHI::IParallelCommandRecording
{
public:
    explicit MockParallelCommandRecording(
        const Prism::RHI::CommandQueueType queue)
        : m_queue(queue)
    {
        m_context.activeQueue = queue;
    }

    Prism::RHI::ICommandContext&
        GetCommandContext() override
    {
        Expect(
            !m_closed,
            "A closed mock native recording was modified.");
        return m_context;
    }

    bool Close() override
    {
        m_closed = true;
        return true;
    }

    Prism::RHI::CommandQueueType GetQueue() const
    {
        return m_queue;
    }

    bool IsClosed() const
    {
        return m_closed;
    }

private:
    MockCommandContext m_context;
    Prism::RHI::CommandQueueType m_queue =
        Prism::RHI::CommandQueueType::Graphics;
    bool m_closed = false;
};

std::unique_ptr<
    Prism::RHI::IParallelCommandRecording>
MockCommandContext::CreateParallelCommandRecording(
    const Prism::RHI::CommandQueueType queue)
{
    if (!nativeParallelRecording)
    {
        return {};
    }
    return std::make_unique<
        MockParallelCommandRecording>(queue);
}

bool MockCommandContext::
    AppendParallelCommandRecording(
        std::unique_ptr<
            Prism::RHI::IParallelCommandRecording>
            recording)
{
    auto* native =
        dynamic_cast<
            MockParallelCommandRecording*>(
            recording.get());
    if (!batchOpen
        || native == nullptr
        || !native->IsClosed()
        || native->GetQueue() != activeQueue)
    {
        return false;
    }
    ++appendedNativeRecordings;
    return true;
}

class MockGraphicsPipeline final : public Prism::RHI::IGraphicsPipeline
{
public:
    Prism::RHI::GraphicsApi GetGraphicsApi() const override { return Prism::RHI::GraphicsApi::Vulkan; }
};

class MockBuffer final : public Prism::RHI::IBuffer
{
public:
    MockBuffer() = default;
    explicit MockBuffer(
        const Prism::RHI::BufferDescription& value)
        : description(value)
    {
    }
    MockBuffer(
        const Prism::RHI::BufferDescription& value,
        Prism::RHI::TransientBufferAllocationInfo allocation)
        : description(value)
        , allocationInfo(allocation)
        , transient(true)
    {
    }

    Prism::RHI::GraphicsApi GetGraphicsApi() const override { return Prism::RHI::GraphicsApi::Vulkan; }
    const Prism::RHI::BufferDescription& GetDescription() const override { return description; }
    void Update(const void*, std::size_t, std::size_t) override {}
    void Read(void*, std::size_t, std::size_t) const override {}
    const Prism::RHI::TransientBufferAllocationInfo*
        GetTransientAllocationInfo() const override
    {
        return transient ? &allocationInfo : nullptr;
    }

private:
    Prism::RHI::BufferDescription description{64, 16, Prism::RHI::BufferUsage::Vertex,
                                               Prism::RHI::MemoryAccess::CpuToGpu};
    Prism::RHI::TransientBufferAllocationInfo
        allocationInfo{};
    bool transient = false;
};

class MockDescriptorSet final : public Prism::RHI::IDescriptorSet
{
public:
    Prism::RHI::GraphicsApi GetGraphicsApi() const override { return Prism::RHI::GraphicsApi::Vulkan; }
    void WriteBuffer(std::uint32_t, std::shared_ptr<Prism::RHI::IBuffer>, std::size_t, std::size_t) override {}
    void WriteTexture(std::uint32_t, std::shared_ptr<Prism::RHI::ITexture>) override {}
    void WriteTextureView(std::uint32_t, std::shared_ptr<Prism::RHI::ITextureView>) override {}
    void WriteSampler(std::uint32_t, std::shared_ptr<Prism::RHI::ISampler>) override {}
    void WriteAccelerationStructure(
        std::uint32_t,
        std::shared_ptr<
            Prism::RHI::IRayTracingAccelerationStructure>)
        override
    {
    }
};

class MockTextureView final : public Prism::RHI::ITextureView
{
public:
    explicit MockTextureView(
        const Prism::RHI::TextureViewType type = Prism::RHI::TextureViewType::DepthStencil)
    {
        description.type = type;
    }
    MockTextureView(
        const Prism::RHI::TextureViewDescription& value,
        const Prism::RHI::ITexture* texture)
        : description(value)
        , sourceTexture(texture)
    {
    }

    Prism::RHI::GraphicsApi GetGraphicsApi() const override { return Prism::RHI::GraphicsApi::Vulkan; }
    const Prism::RHI::TextureViewDescription& GetDescription() const override { return description; }
    const Prism::RHI::ITexture* GetTexture() const override { return sourceTexture; }

private:
    Prism::RHI::TextureViewDescription description{};
    const Prism::RHI::ITexture* sourceTexture = nullptr;
};

class MockTexture final : public Prism::RHI::ITexture
{
public:
    MockTexture() = default;
    explicit MockTexture(
        Prism::RHI::TextureDescription value)
        : description(value)
    {
    }
    MockTexture(
        Prism::RHI::TextureDescription value,
        Prism::RHI::TransientTextureAllocationInfo allocation)
        : description(value)
        , allocationInfo(allocation)
        , transient(true)
    {
    }

    Prism::RHI::GraphicsApi GetGraphicsApi() const override { return Prism::RHI::GraphicsApi::Vulkan; }
    const Prism::RHI::TextureDescription& GetDescription() const override { return description; }
    const Prism::RHI::TransientTextureAllocationInfo*
        GetTransientAllocationInfo() const override
    {
        return transient ? &allocationInfo : nullptr;
    }

private:
    Prism::RHI::TextureDescription description{
        Prism::RHI::TextureDimension::Texture2D,
        64,
        64,
        1,
        1,
        1,
        Prism::RHI::Format::Rgba16Float,
        Prism::RHI::TextureUsage::RenderTarget | Prism::RHI::TextureUsage::ShaderResource,
        Prism::RHI::MemoryAccess::GpuOnly};
    Prism::RHI::TransientTextureAllocationInfo
        allocationInfo{};
    bool transient = false;
};
} // namespace

int main()
{
    try
    {
        {
            Prism::Renderer::RenderGraph profilingGraph;
            profilingGraph.ImportResource("Input");
            profilingGraph.AddPass(
                "Profiled",
                {"Input"},
                {"Output"},
                []
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                });
            profilingGraph.MarkOutput("Output");
            profilingGraph.SetDetailedProfilingEnabled(false);
            profilingGraph.Execute();
            Expect(profilingGraph.GetCpuMilliseconds("Profiled") == 0.0f,
                "Basic RenderGraph sampling retained per-pass CPU timing.");
            profilingGraph.SetDetailedProfilingEnabled(true);
            profilingGraph.Execute();
            Expect(profilingGraph.GetCpuMilliseconds("Profiled") > 0.0f,
                "Detailed RenderGraph sampling did not record per-pass CPU timing.");
        }
        {
            Prism::Renderer::TemporalAntiAliasing temporal;
            temporal.ResetHistory();
            temporal.ResetHistory();
            Expect(temporal.GetHistoryResetCallCount() == 2 && !temporal.IsHistoryValid()
                && temporal.GetHistoryReadIndex() == 0, "TAA diagnostics lost reset calls or changed reset semantics.");
            Prism::Renderer::RenderGraph stateGraph;
            MockTexture texture;
            const auto combined = Prism::RHI::ResourceState::DepthRead
                | Prism::RHI::ResourceState::ShaderResource;
            (void)stateGraph.ImportTexture("CombinedState", texture, combined);
            stateGraph.Compile();
            const auto report = Prism::Renderer::BuildRenderGraphReport(stateGraph, Prism::RHI::GraphicsApi::Vulkan);
            Expect(report.at("resources").at(0).at("stateBits") == static_cast<std::uint32_t>(combined),
                "Graph diagnostics lost combined resource state bits.");
        }
        MockCommandContext barrierTarget;
        MockBuffer barrierBuffer;
        Prism::RHI::DeferredCommandContext deferredBarriers(
            Prism::RHI::GraphicsApi::Vulkan,
            Prism::RHI::CommandQueueType::Graphics);
        Prism::RHI::BufferBarrier bufferBarrier{};
        bufferBarrier.buffer = &barrierBuffer;
        bufferBarrier.before =
            Prism::RHI::ResourceState::CopyDestination;
        bufferBarrier.after =
            Prism::RHI::ResourceState::UnorderedAccess;
        bufferBarrier.offset = 16;
        bufferBarrier.size = 32;
        deferredBarriers.BufferBarrier(bufferBarrier);
        const Prism::RHI::GlobalBarrier globalBarrier{
            Prism::RHI::ResourceState::UnorderedAccess,
            Prism::RHI::ResourceState::ShaderResource};
        deferredBarriers.GlobalBarrier(globalBarrier);
        Prism::RHI::BufferDescription indirectDescription{};
        indirectDescription.size = 128;
        indirectDescription.stride =
            sizeof(
                Prism::RHI::
                    DrawIndexedIndirectArguments);
        indirectDescription.usage =
            Prism::RHI::BufferUsage::Indirect;
        indirectDescription.memoryAccess =
            Prism::RHI::MemoryAccess::CpuToGpu;
        MockBuffer indirectArguments(
            indirectDescription);
        deferredBarriers.DrawIndexedIndirect(
            indirectArguments,
            20,
            3,
            20,
            nullptr,
            0);
        deferredBarriers.Replay(barrierTarget);
        Expect(
            barrierTarget.bufferBarriers == 1
                && barrierTarget.memoryBarriers == 1
                && barrierTarget.lastBufferBarrier.buffer
                    == &barrierBuffer
                && barrierTarget.lastBufferBarrier.offset
                    == 16
                && barrierTarget.lastBufferBarrier.size
                    == 32
                && barrierTarget.lastMemoryBarrier.after
                    == Prism::RHI::ResourceState::
                        ShaderResource
                && barrierTarget
                       .indexedIndirectDraws
                    == 1
                && barrierTarget
                       .lastIndirectArgumentBuffer
                    == &indirectArguments
                && barrierTarget
                       .lastIndirectArgumentOffset
                    == 20
                && barrierTarget
                       .lastIndirectMaxDrawCount
                    == 3
                && barrierTarget
                       .lastIndirectStride
                    == sizeof(
                        Prism::RHI::
                            DrawIndexedIndirectArguments),
            "Deferred command recording lost public buffer/global barriers.");

        Prism::Renderer::RenderGraph graph;
        MockCommandContext commandContext;

        {
            auto sceneData =
                std::make_shared<const Prism::Scene::RenderSceneData>(
                    Prism::Scene::SceneGeneration{1},
                    Prism::Scene::RenderSceneDataRevision{1},
                    std::vector<Prism::Scene::RenderObject>{},
                    std::vector<Prism::Scene::RenderSceneObjectMetadata>{},
                    std::vector<Prism::Scene::RenderSceneObjectAssetBindings>{});
            Prism::Scene::RenderView renderView{};
            renderView.id = Prism::Scene::GameRenderViewId;
            renderView.width = 1;
            renderView.height = 1;
            auto packet =
                std::make_shared<const Prism::Scene::RenderFramePacket>(
                    Prism::Scene::LogicalFrameId{1},
                    0.0,
                    std::move(sceneData),
                    Prism::Scene::RenderFrameDynamicData{},
                    std::vector<Prism::Scene::RenderView>{renderView});
            std::weak_ptr<const Prism::Scene::RenderFramePacket> packetWeak =
                packet;
            auto sceneLifetime =
                std::make_shared<const Prism::Scene::RenderSceneView>(
                    packet,
                    Prism::Scene::GameRenderViewId);
            packet.reset();

            Prism::Renderer::RenderGraph lifetimeGraph;
            std::atomic<std::uint32_t> packetObservations{0};
            lifetimeGraph.ImportResource("RetainedPacketInput");
            lifetimeGraph.AddContextPass(
                "RetainedScenePacket.Graphics",
                {"RetainedPacketInput"},
                {"RetainedPacketIntermediate"},
                [sceneLifetime, &packetObservations](
                    Prism::RHI::ICommandContext&)
                {
                    if (sceneLifetime->GetPacket() != nullptr)
                    {
                        packetObservations.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                },
                Prism::Renderer::RenderGraph::PassOptions{
                    Prism::Renderer::RenderGraph::QueueClass::Graphics,
                    false,
                    true,
                    true,
                    Prism::Renderer::RenderGraph::ParallelRecordingContract::
                        AuditedIndependent()});
            lifetimeGraph.AddContextPass(
                "RetainedScenePacket.Compute",
                {"RetainedPacketIntermediate"},
                {"RetainedPacketOutput"},
                [sceneLifetime, &packetObservations](
                    Prism::RHI::ICommandContext&)
                {
                    if (sceneLifetime->GetPacket() != nullptr)
                    {
                        packetObservations.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                },
                Prism::Renderer::RenderGraph::PassOptions{
                    Prism::Renderer::RenderGraph::QueueClass::Compute,
                    false,
                    true,
                    true,
                    Prism::Renderer::RenderGraph::ParallelRecordingContract::
                        AuditedIndependent()});
            lifetimeGraph.MarkOutput("RetainedPacketOutput");
            lifetimeGraph.SetQueueExecutionMode(
                Prism::Renderer::RenderGraph::QueueExecutionMode::Native);
            sceneLifetime.reset();
            Expect(
                !packetWeak.expired(),
                "A graph callback did not retain its immutable scene packet.");

            commandContext.nativeQueueSwitching = true;
            commandContext.independentBatchSubmission = true;
            commandContext.deferredBatchSubmission = true;
            commandContext.nativeParallelRecording = true;
            commandContext.appendedNativeRecordings = 0;
            lifetimeGraph.Execute(commandContext);
            Expect(
                packetObservations.load(std::memory_order_relaxed) == 2
                    && commandContext.appendedNativeRecordings == 2
                    && lifetimeGraph.GetCompilationSummary()
                        .nativeParallelCommandRecordingApplied,
                "Parallel graph recording lost its immutable scene packet lease.");
            lifetimeGraph.Reset();
            Expect(
                packetWeak.expired(),
                "A reset graph retained an obsolete scene packet.");

            Prism::Renderer::RenderGraph unauditedGraph;
            std::thread::id unauditedExecutionThread;
            const std::thread::id executionLaneThread =
                std::this_thread::get_id();
            unauditedGraph.ImportResource("Input");
            unauditedGraph.AddContextPass(
                "UnauditedSharedWrite",
                {"Input"},
                {"Output"},
                [&](Prism::RHI::ICommandContext& receivedContext)
                {
                    Expect(&receivedContext == &commandContext,
                        "An unaudited pass received an isolated context.");
                    unauditedExecutionThread =
                        std::this_thread::get_id();
                },
                Prism::Renderer::RenderGraph::PassOptions{
                    Prism::Renderer::RenderGraph::QueueClass::Graphics,
                    false,
                    true,
                    true});
            unauditedGraph.MarkOutput("Output");
            unauditedGraph.SetQueueExecutionMode(
                Prism::Renderer::RenderGraph::QueueExecutionMode::Native);
            commandContext.appendedNativeRecordings = 0;
            unauditedGraph.Execute(commandContext);
            Expect(
                unauditedExecutionThread == executionLaneThread
                    && commandContext.appendedNativeRecordings == 0
                    && !unauditedGraph.GetCompilationSummary()
                        .parallelCommandRecordingApplied,
                "An unaudited callback escaped the execution lane.");

            Prism::Core::TaskScheduler recordingPool({2, 4});
            Prism::Renderer::RenderGraph reorderedGraph;
            TestEvent secondRecordingFinished;
            std::mutex completionMutex;
            std::vector<std::string> completionOrder;
            std::vector<std::string> replayOrder;
            reorderedGraph.ImportResource("Input");
            reorderedGraph.AddContextPass(
                "FirstRecord",
                {"Input"},
                {"Intermediate"},
                [&](Prism::RHI::ICommandContext&)
                {
                    secondRecordingFinished.Wait();
                    std::lock_guard lock(completionMutex);
                    completionOrder.emplace_back("FirstRecord");
                },
                Prism::Renderer::RenderGraph::PassOptions{
                    Prism::Renderer::RenderGraph::QueueClass::Graphics,
                    false,
                    true,
                    true,
                    Prism::Renderer::RenderGraph::ParallelRecordingContract::
                        AuditedIndependent()});
            reorderedGraph.AddContextPass(
                "SecondRecord",
                {"Intermediate"},
                {"Output"},
                [&](Prism::RHI::ICommandContext&)
                {
                    {
                        std::lock_guard lock(completionMutex);
                        completionOrder.emplace_back("SecondRecord");
                    }
                    secondRecordingFinished.Signal();
                },
                Prism::Renderer::RenderGraph::PassOptions{
                    Prism::Renderer::RenderGraph::QueueClass::Compute,
                    false,
                    true,
                    true,
                    Prism::Renderer::RenderGraph::ParallelRecordingContract::
                        AuditedIndependent()});
            reorderedGraph.MarkOutput("Output");
            reorderedGraph.SetQueueExecutionMode(
                Prism::Renderer::RenderGraph::QueueExecutionMode::Native);
            commandContext.batchQueues.clear();
            reorderedGraph.Execute(
                commandContext,
                recordingPool,
                [&](const std::string_view name)
                {
                    replayOrder.emplace_back(name);
                });
            const Prism::Core::TaskExecutorStatistics recordingStatistics =
                recordingPool.GetStatistics();
            Expect(
                completionOrder
                        == std::vector<std::string>{
                            "SecondRecord", "FirstRecord"}
                    && replayOrder
                        == std::vector<std::string>{
                            "FirstRecord", "SecondRecord"}
                    && commandContext.batchQueues
                        == std::vector<Prism::RHI::CommandQueueType>{
                            Prism::RHI::CommandQueueType::Graphics,
                            Prism::RHI::CommandQueueType::Compute}
                    && recordingStatistics.submittedTaskCount == 2
                    && recordingStatistics.completedTaskCount == 2
                    && recordingStatistics
                        .cumulativeExecutionNanoseconds > 0
                    && recordingStatistics.cumulativeJoinNanoseconds > 0
                    && recordingStatistics.peakActiveWorkerCount <= 2
                    && recordingStatistics.peakQueuedTaskCount <= 4,
                "Out-of-order task completion changed graph replay order.");

            Prism::Renderer::RenderGraph failedPoolGraph;
            failedPoolGraph.ImportResource("Input");
            failedPoolGraph.AddContextPass(
                "FailureRoot", {"Input"}, {"Root"},
                [](Prism::RHI::ICommandContext&) {});
            failedPoolGraph.AddContextPass(
                "FailureRecord", {"Root"}, {"Failed"},
                [](Prism::RHI::ICommandContext&)
                {
                    throw std::runtime_error("pooled recording failed");
                },
                Prism::Renderer::RenderGraph::PassOptions{
                    Prism::Renderer::RenderGraph::QueueClass::Compute,
                    false,
                    true,
                    true,
                    Prism::Renderer::RenderGraph::ParallelRecordingContract::
                        AuditedIndependent()});
            failedPoolGraph.AddContextPass(
                "FailureJoin", {"Failed"}, {"Output"},
                [](Prism::RHI::ICommandContext&) {});
            failedPoolGraph.MarkOutput("Output");
            failedPoolGraph.SetQueueExecutionMode(
                Prism::Renderer::RenderGraph::QueueExecutionMode::Native);
            commandContext.batchQueues.clear();
            bool pooledFailureReported = false;
            try
            {
                failedPoolGraph.Execute(commandContext, recordingPool);
            }
            catch (const std::runtime_error& exception)
            {
                pooledFailureReported =
                    std::string(exception.what()) ==
                    "pooled recording failed";
            }
            Expect(
                pooledFailureReported
                    && commandContext.batchQueues.empty()
                    && failedPoolGraph.GetPassInfos().empty(),
                "A failed pooled recording submitted a partial frame.");
            commandContext.nativeQueueSwitching = false;
            commandContext.independentBatchSubmission = false;
            commandContext.deferredBatchSubmission = false;
            commandContext.nativeParallelRecording = false;
        }

        std::vector<std::string> executionOrder;
        graph.ImportResource("ImportedColor");
        graph.AddContextPass(
            "Geometry",
            {"ImportedColor"},
            {"SceneColor", "Depth"},
            [&](Prism::RHI::ICommandContext& receivedContext)
            {
                Expect(&receivedContext == &commandContext, "RenderGraph passed the wrong command context.");
                executionOrder.emplace_back("Geometry");
            });
        graph.AddContextPass(
            "PostProcess",
            {"SceneColor"},
            {"BackBuffer"},
            [&](Prism::RHI::ICommandContext&)
            {
                executionOrder.emplace_back("PostProcess");
            });
        graph.Execute(commandContext);

        Expect(executionOrder.size() == 2, "RenderGraph did not execute every context pass.");
        Expect(executionOrder[0] == "Geometry" && executionOrder[1] == "PostProcess",
               "RenderGraph context pass order changed.");
        Expect(graph.GetPassInfos().size() == 2, "RenderGraph pass statistics were not recorded.");
        const std::vector<Prism::Renderer::RenderGraph::PassDescription> passDescriptions =
            graph.GetPassDescriptions();
        Expect(passDescriptions.size() == 2
                   && passDescriptions[0].name == "Geometry"
                   && passDescriptions[0].reads.size() == 1
                   && passDescriptions[0].writes.size() == 2,
               "RenderGraph pass introspection lost pass dependencies.");
        const std::vector<Prism::Renderer::RenderGraph::ResourceDescription> resourceDescriptions =
            graph.GetResourceDescriptions();
        const auto importedColor = std::ranges::find(
            resourceDescriptions,
            std::string("ImportedColor"),
            &Prism::Renderer::RenderGraph::ResourceDescription::name);
        Expect(importedColor != resourceDescriptions.end()
                   && importedColor->imported,
               "RenderGraph resource introspection lost imported resources.");

        bool invalidDependencyRejected = false;
        graph.Reset();
        graph.AddContextPass("Invalid", {"Missing"}, {"Output"}, [](Prism::RHI::ICommandContext&) {});
        try
        {
            graph.Execute(commandContext);
        }
        catch (const std::runtime_error&)
        {
            invalidDependencyRejected = true;
        }
        Expect(invalidDependencyRejected, "RenderGraph accepted a read-before-produce dependency.");

        MockTexture graphTexture;
        graph.Reset();
        commandContext.barriers.clear();
        commandContext.textureBarriers = 0;
        graph.DeclareTexture("SceneColor", graphTexture, Prism::RHI::ResourceState::Undefined);
        graph.AddResourceContextPass(
            "WriteColor",
            {},
            {{"SceneColor", Prism::RHI::ResourceState::RenderTarget}},
            [](Prism::RHI::ICommandContext&) {});
        graph.AddResourceContextPass(
            "ReadColor",
            {{"SceneColor", Prism::RHI::ResourceState::ShaderResource}},
            {{"Output", Prism::RHI::ResourceState::Undefined}},
            [](Prism::RHI::ICommandContext&) {});
        graph.Execute(commandContext);
        Expect(commandContext.textureBarriers == 2,
               "RenderGraph did not generate write and read texture transitions.");
        Expect(commandContext.barriers[0].before == Prism::RHI::ResourceState::Undefined
                   && commandContext.barriers[0].after == Prism::RHI::ResourceState::RenderTarget
                   && commandContext.barriers[1].before == Prism::RHI::ResourceState::RenderTarget
                   && commandContext.barriers[1].after == Prism::RHI::ResourceState::ShaderResource,
               "RenderGraph generated an incorrect automatic barrier sequence.");
        const auto trackedResources = graph.GetResourceDescriptions();
        const auto trackedPasses = graph.GetPassDescriptions();
        const auto trackedSceneColor =
            std::ranges::find(
                trackedResources,
                std::string("SceneColor"),
                &Prism::Renderer::RenderGraph::ResourceDescription::name);
        Expect(trackedSceneColor != trackedResources.end()
                   && trackedSceneColor->state == Prism::RHI::ResourceState::ShaderResource,
               "RenderGraph resource introspection did not expose the final tracked state.");
        Expect(trackedPasses.size() == 2
                   && trackedPasses[0].writes[0].state == Prism::RHI::ResourceState::RenderTarget
                   && trackedPasses[1].reads[0].state == Prism::RHI::ResourceState::ShaderResource,
               "RenderGraph pass introspection did not expose declared access states.");

        Prism::RHI::TextureDescription
            subresourceTextureDescription{
                Prism::RHI::TextureDimension::
                    Texture2D,
                128,
                128,
                2,
                4,
                1,
                Prism::RHI::Format::Rgba16Float,
                Prism::RHI::TextureUsage::
                        ShaderResource
                    | Prism::RHI::TextureUsage::
                          UnorderedAccess,
                Prism::RHI::MemoryAccess::GpuOnly};
        Prism::RHI::BufferDescription
            rangeBufferDescription{
                128,
                16,
                Prism::RHI::BufferUsage::Storage
                    | Prism::RHI::BufferUsage::
                          CopySource
                    | Prism::RHI::BufferUsage::
                          CopyDestination,
                Prism::RHI::MemoryAccess::GpuOnly};
        MockTexture subresourceTexture(
            subresourceTextureDescription);
        MockBuffer rangeBuffer(
            rangeBufferDescription);
        Prism::RHI::TextureViewDescription
            mipViewDescription{};
        mipViewDescription.type =
            Prism::RHI::TextureViewType::Storage;
        mipViewDescription.baseMipLevel = 1;
        mipViewDescription.mipLevelCount = 1;
        mipViewDescription.baseArrayLayer = 1;
        mipViewDescription.arrayLayerCount = 1;
        MockTextureView mipView(
            mipViewDescription,
            &subresourceTexture);

        graph.Reset();
        commandContext.barriers.clear();
        commandContext.bufferBarrierHistory.clear();
        commandContext.textureBarriers = 0;
        commandContext.bufferBarriers = 0;
        Prism::Renderer::TextureHandle
            versionedTexture = graph.DeclareTexture(
                "VersionedTexture",
                subresourceTexture,
                Prism::RHI::ResourceState::
                    ShaderResource);
        Prism::Renderer::BufferHandle
            versionedBuffer = graph.DeclareBuffer(
                "VersionedBuffer",
                rangeBuffer,
                Prism::RHI::ResourceState::
                    CopySource);
        Prism::Renderer::TextureViewHandle
            mipViewHandle = graph.DeclareTextureView(
                "VersionedTexture.Mip1Layer1",
                versionedTexture,
                mipView);
        graph.GetBlackboard().Set<std::string>(
            "subresource-test");
        Expect(
            graph.GetBlackboard()
                    .Get<std::string>()
                == "subresource-test",
            "RenderGraph blackboard lost a typed value.");

        auto writeParameters =
            graph.CreatePassParameters();
        const Prism::Renderer::TextureHandle
            textureVersion1 =
                writeParameters.WriteTexture(
                    versionedTexture,
                    Prism::RHI::ResourceState::
                        UnorderedAccess,
                    {1, 1, 1, 1});
        const Prism::Renderer::BufferHandle
            bufferVersion1 =
                writeParameters.WriteBuffer(
                    versionedBuffer,
                    Prism::RHI::ResourceState::
                        CopyDestination,
                    {16, 32});
        Prism::Renderer::TextureViewHandle
            undeclaredMipView = mipViewHandle;
        undeclaredMipView.texture = textureVersion1;
        bool parameterResourcesResolved = false;
        bool undeclaredParameterRejected = false;
        graph.AddParameterPass(
            "WriteSubresources",
            std::move(writeParameters),
            [&](Prism::RHI::ICommandContext&,
                const Prism::Renderer::
                    RenderGraphPassResources&
                        resources)
            {
                parameterResourcesResolved =
                    &resources.GetTexture(
                        textureVersion1)
                        == &subresourceTexture
                    && &resources.GetBuffer(
                           bufferVersion1)
                        == &rangeBuffer;
                try
                {
                    (void)resources.GetTextureView(
                        undeclaredMipView);
                }
                catch (const std::runtime_error&)
                {
                    undeclaredParameterRejected = true;
                }
            });

        bool staleVersionRejected = false;
        try
        {
            auto staleParameters =
                graph.CreatePassParameters();
            staleParameters.ReadTexture(
                versionedTexture,
                Prism::RHI::ResourceState::
                    ShaderResource);
            graph.AddParameterPass(
                "StaleRead",
                std::move(staleParameters),
                [](Prism::RHI::ICommandContext&,
                   const Prism::Renderer::
                       RenderGraphPassResources&) {});
        }
        catch (const std::runtime_error&)
        {
            staleVersionRejected = true;
        }
        Expect(
            staleVersionRejected,
            "RenderGraph accepted a stale texture version.");

        mipViewHandle.texture = textureVersion1;
        auto readParameters =
            graph.CreatePassParameters();
        readParameters.ReadTextureView(
            mipViewHandle,
            Prism::RHI::ResourceState::
                ShaderResource);
        readParameters.ReadBuffer(
            bufferVersion1,
            Prism::RHI::ResourceState::
                ShaderResource,
            {16, 32});
        bool viewResolved = false;
        graph.AddParameterPass(
            "ReadSubresources",
            std::move(readParameters),
            [&](Prism::RHI::ICommandContext&,
                const Prism::Renderer::
                    RenderGraphPassResources&
                        resources)
            {
                viewResolved =
                    &resources.GetTextureView(
                        mipViewHandle)
                    == &mipView;
            },
            Prism::Renderer::RenderGraph::
                PassOptions{
                    Prism::Renderer::RenderGraph::
                        QueueClass::Graphics,
                    true,
                    false});
        graph.Execute(commandContext);
        Expect(
            parameterResourcesResolved
                && viewResolved
                && undeclaredParameterRejected,
            "RenderGraph parameter-pass resources did not resolve their RHI objects.");
        Expect(
            commandContext.textureBarriers == 2
                && commandContext.barriers[0]
                           .baseMipLevel == 1
                && commandContext.barriers[0]
                           .mipLevelCount == 1
                && commandContext.barriers[0]
                           .baseArrayLayer == 1
                && commandContext.barriers[0]
                           .arrayLayerCount == 1
                && commandContext.barriers[1]
                           .before
                    == Prism::RHI::ResourceState::
                        UnorderedAccess
                && commandContext.barriers[1]
                           .after
                    == Prism::RHI::ResourceState::
                        ShaderResource,
            "RenderGraph did not generate subresource texture barriers.");
        Expect(
            commandContext.bufferBarriers == 2
                && commandContext
                       .bufferBarrierHistory[0]
                       .offset == 16
                && commandContext
                       .bufferBarrierHistory[0]
                       .size == 32
                && commandContext
                       .bufferBarrierHistory[1]
                       .before
                    == Prism::RHI::ResourceState::
                        CopyDestination
                && commandContext
                       .bufferBarrierHistory[1]
                       .after
                    == Prism::RHI::ResourceState::
                        ShaderResource,
            "RenderGraph did not generate ranged buffer barriers.");
        const auto versionedResources =
            graph.GetResourceDescriptions();
        const auto describedVersionedTexture =
            std::ranges::find(
                versionedResources,
                std::string("VersionedTexture"),
                &Prism::Renderer::RenderGraph::
                    ResourceDescription::name);
        const auto versionedPasses =
            graph.GetPassDescriptions();
        const auto versionedSummary =
            graph.GetCompilationSummary();
        const auto describedTextureRead =
            std::ranges::find(
                versionedPasses[1].reads,
                std::string("texture"),
                &Prism::Renderer::RenderGraph::
                    PassDescription::Access::kind);
        Expect(
            describedVersionedTexture
                    != versionedResources.end()
                && describedVersionedTexture->texture
                && describedVersionedTexture
                       ->currentVersion == 1
                && versionedPasses[0].writes[0]
                       .kind == "texture"
                && versionedPasses[0].writes[0]
                       .version == 1
                && versionedPasses[0].writes[1]
                       .kind == "buffer"
                && describedTextureRead
                    != versionedPasses[1]
                           .reads.end()
                && describedTextureRead
                       ->textureRange.baseMipLevel == 1,
            "RenderGraph diagnostics lost handle versions or access ranges.");
        Expect(
            versionedSummary.registeredTextureCount == 1
                && versionedSummary
                       .registeredBufferCount == 1
                && versionedSummary
                       .registeredTextureViewCount == 1
                && versionedSummary
                       .versionedResourceCount == 2
                && versionedSummary
                       .parameterPassCount == 2
                && versionedSummary
                       .textureSubresourceCount == 8
                && describedVersionedTexture
                       ->textureSubresourceCount == 8,
            "RenderGraph resource-model compilation statistics are invalid.");

        const Prism::Renderer::TextureHandle
            invalidatedTexture = textureVersion1;
        graph.Reset();
        bool generationRejected = false;
        try
        {
            (void)graph.ResolveTexture(
                invalidatedTexture);
        }
        catch (const std::runtime_error&)
        {
            generationRejected = true;
        }
        Expect(
            generationRejected,
            "RenderGraph accepted a handle from an older graph generation.");

        MockTexture previousHistoryTexture;
        MockTexture currentHistoryTexture;
        const Prism::Renderer::TextureHistoryHandle
            history = graph.ImportTextureHistory(
                "TemporalColor",
                previousHistoryTexture,
                Prism::RHI::ResourceState::
                    ShaderResource,
                currentHistoryTexture,
                Prism::RHI::ResourceState::
                    ShaderResource);
        auto historyParameters =
            graph.CreatePassParameters();
        historyParameters.ReadTexture(
            history.previous,
            Prism::RHI::ResourceState::
                ShaderResource);
        const Prism::Renderer::TextureHandle
            currentHistoryVersion =
                historyParameters.WriteTexture(
                    history.current,
                    Prism::RHI::ResourceState::
                        RenderTarget);
        graph.AddParameterPass(
            "TemporalResolve",
            std::move(historyParameters),
            [](Prism::RHI::ICommandContext&,
               const Prism::Renderer::
                   RenderGraphPassResources&) {},
            Prism::Renderer::RenderGraph::
                PassOptions{
                    Prism::Renderer::RenderGraph::
                        QueueClass::Graphics,
                    true,
                    false});
        graph.MarkOutput(currentHistoryVersion);
        graph.Compile();
        const auto historyResources =
            graph.GetResourceDescriptions();
        const std::size_t historyResourceCount =
            std::ranges::count(
                historyResources,
                true,
                &Prism::Renderer::RenderGraph::
                    ResourceDescription::history);
        Expect(
            historyResourceCount == 2,
            "RenderGraph history imports were not identified in diagnostics.");

        graph.Reset();
        Prism::RHI::BufferDescription
            transientBufferDescription{
                4096,
                16,
                Prism::RHI::BufferUsage::Storage,
                Prism::RHI::MemoryAccess::GpuOnly};
        graph.DeclareTransientBuffer(
            "TransientBufferA",
            transientBufferDescription);
        graph.DeclareTransientBuffer(
            "TransientBufferB",
            transientBufferDescription);
        graph.AddContextPass(
            "WriteTransientBufferA",
            {},
            {"TransientBufferA"},
            [](Prism::RHI::ICommandContext&) {});
        graph.AddContextPass(
            "ConsumeTransientBufferA",
            {"TransientBufferA"},
            {},
            [](Prism::RHI::ICommandContext&) {},
            Prism::Renderer::RenderGraph::
                PassOptions{
                    Prism::Renderer::RenderGraph::
                        QueueClass::Graphics,
                    true,
                    false});
        graph.AddContextPass(
            "WriteTransientBufferB",
            {},
            {"TransientBufferB"},
            [](Prism::RHI::ICommandContext&) {});
        graph.AddContextPass(
            "ConsumeTransientBufferB",
            {"TransientBufferB"},
            {"TransientBufferOutput"},
            [](Prism::RHI::ICommandContext&) {});
        graph.MarkOutput("TransientBufferOutput");
        graph.Compile();
        const auto transientBufferSummary =
            graph.GetCompilationSummary();
        Expect(
            transientBufferSummary
                    .transientResourceCount == 2
                && transientBufferSummary
                       .transientAllocationCount == 1
                && transientBufferSummary
                       .transientAliasedBytes == 4096,
            "RenderGraph did not plan compatible transient buffer aliasing.");

        graph.Reset();
        MockBuffer sharedFrameConstants;
        MockBuffer sharedClusterConstants;
        MockBuffer sharedPointLights;
        MockBuffer sharedClusterLightCounts;
        MockBuffer sharedClusterLightIndices;
        MockBuffer matrixGpuObjectRecords;
        MockBuffer matrixIndirectArguments;
        MockBuffer matrixIndirectDrawCounts;
        MockBuffer sharedLocalLightConstants;
        MockBuffer
            sharedLocalShadowRenderConstants;
        MockTexture sharedShadow;
        MockTexture sharedSpotShadow;
        MockTexture sharedPointShadow;
        MockTexture sharedPlanarReflection;
        MockTexture sharedPlanarReflectionDepth;
        MockTexture sharedShadowMoments;
        MockTexture sharedShadowMomentsScratch;
        MockTexture sharedDepth;
        MockTexture sharedHiZ;
        std::array<MockTexture, 3> sharedWaterGBuffer;
        MockTexture sharedWaterMotion;
        MockTexture sharedWaterCompositeDepth;
        MockTexture sharedWaterRefraction;
        MockTexture sharedWaterCompositeScratch;
        std::array<MockTexture, 4> sharedWaterVolumes;
        MockTexture sharedAmbientOcclusion;
        MockTexture sharedScreenSpaceColor;
        MockTexture sharedMotionVectors;
        MockTexture sharedTemporalResolved;
        MockTexture sharedTemporalHistoryRead;
        MockTexture sharedTemporalHistoryWrite;
        MockTexture sharedAtmosphereTransmittance;
        MockTexture sharedAtmosphereSkyView;
        std::array<MockTexture, 2> sharedOceanSpectrumA;
        std::array<MockTexture, 2> sharedOceanSpectrumB;
        MockTexture sharedOceanDisplacement;
        MockTexture sharedOceanNormalFoam;
        MockTexture sharedOceanSlopeMoments;
        MockTexture sharedOceanInitialSpectrum;
        std::array<MockTexture, 2> sharedOceanFoamHistory;
        MockTexture sharedTerrainRawHeight;
        MockTexture sharedTerrainErodedHeight;
        std::array<MockTexture, 4> sharedGBuffer;
        MockTexture sharedHdr;
        MockTexture sharedBloomA;
        MockTexture sharedBloomB;
        MockTexture sharedOutput;
        Prism::Renderer::SharedRenderGraphResources
            sharedResources{};
        sharedResources.frameConstants =
            &sharedFrameConstants;
        sharedResources.shadowMap = &sharedShadow;
        sharedResources.depthBuffer = &sharedDepth;
        sharedResources.hiZ = &sharedHiZ;
        sharedResources.hdrColor = &sharedHdr;
        sharedResources.bloomA = &sharedBloomA;
        sharedResources.bloomB = &sharedBloomB;
        sharedResources.outputColor = &sharedOutput;
        for (std::size_t index = 0;
             index < sharedGBuffer.size();
             ++index)
        {
            sharedResources.gbuffer[index] =
                &sharedGBuffer[index];
        }
        std::size_t sharedCallbackCount = 0;
        std::size_t waterOpticsBoundaryCount = 0;
        const auto sharedCallback =
            [&](Prism::RHI::ICommandContext&,
                const Prism::Renderer::
                    RenderGraphPassResources&)
            {
                ++sharedCallbackCount;
            };
        Prism::Renderer::SharedRenderGraphCallbacks
            sharedCallbacks{};
        sharedCallbacks.shadow = sharedCallback;
        sharedCallbacks.gbuffer = sharedCallback;
        sharedCallbacks.deferredLighting =
            sharedCallback;
        sharedCallbacks.forwardGeometry =
            sharedCallback;
        Prism::Renderer::WaterOpticsPassCallbacks sharedWaterCallbacks{};
        sharedWaterCallbacks.depthCopy = sharedCallback;
        sharedWaterCallbacks.visibility = sharedCallback;
        sharedWaterCallbacks.refraction = sharedCallback;
        sharedWaterCallbacks.caustics = sharedCallback;
        sharedWaterCallbacks.volumetricAccumulate = sharedCallback;
        sharedWaterCallbacks.volumetricTemporal = sharedCallback;
        sharedWaterCallbacks.volumetricReconstruct = sharedCallback;
        sharedWaterCallbacks.composite = sharedCallback;
        sharedWaterCallbacks.publish = sharedCallback;
        Prism::Renderer::WaterOpticsPassOptions sharedWaterOptions{};
        sharedWaterOptions.compositeEnabled = true;
        const Prism::Renderer::WaterOpticsGraphCallback sharedWaterBuild =
            [&](Prism::Renderer::RenderGraph& waterGraph,
                const Prism::Renderer::WaterOpticsGraphInputs& inputs)
            {
                ++waterOpticsBoundaryCount;
                const auto refraction = waterGraph.ImportTexture(
                    "WaterRefraction", sharedWaterRefraction,
                    Prism::RHI::ResourceState::Undefined);
                const auto composite = waterGraph.ImportTexture(
                    "WaterCompositeScratch", sharedWaterCompositeScratch,
                    Prism::RHI::ResourceState::Undefined);
                std::array<Prism::Renderer::TextureHandle, 4> volumes{};
                if (sharedWaterOptions.volumetricsEnabled)
                    for (std::size_t i = 0; i < volumes.size(); ++i)
                        volumes[i] = waterGraph.ImportTexture("WaterVolume" + std::to_string(i),
                            sharedWaterVolumes[i], Prism::RHI::ResourceState::ShaderResource);
                return Prism::Renderer::WaterOpticsFeature::AddPasses(
                    waterGraph, inputs, refraction, composite, {}, volumes,
                    sharedWaterCallbacks, sharedWaterOptions);
            };
        const auto publishWaterContribution = [&]()
        {
            Prism::Renderer::WaterOpticsGraphContribution contribution{};
            for (std::size_t index = 0;
                 index < sharedWaterGBuffer.size();
                 ++index)
            {
                contribution.waterGBuffer[index] =
                    &sharedWaterGBuffer[index];
            }
            contribution.waterMotion = &sharedWaterMotion;
            contribution.compositeDepth = &sharedWaterCompositeDepth;
            contribution.build = sharedWaterBuild;
            graph.GetBlackboard().Publish<
                Prism::Renderer::WaterOpticsFeatureSlot>(
                    std::move(contribution),
                    {"test.water-optics",
                     Prism::Renderer::RenderGraphBlackboardValueScope::ViewLocal,
                     0u,
                     graph.GetResourceGeneration(),
                     1u});
        };
        sharedCallbacks.transparentGeometry =
            sharedCallback;
        sharedCallbacks.hiZ =
            [&](Prism::RHI::ICommandContext&,
                const Prism::Renderer::
                    RenderGraphPassResources&,
                const std::uint32_t)
            {
                ++sharedCallbackCount;
            };
        sharedCallbacks.bloomExtract =
            sharedCallback;
        sharedCallbacks.bloomHorizontal =
            sharedCallback;
        sharedCallbacks.bloomVertical =
            sharedCallback;
        sharedCallbacks.tonemap = sharedCallback;
        sharedCallbacks.outputReady =
            sharedCallback;
        Prism::Renderer::SpectralOceanGraphCallbacks
            sharedSpectralCallbacks{};
        bool sharedSpectralRebuild = false;
        const auto publishLightingAndShadowContributions = [&]()
        {
            const auto metadata = [&](const char* producer)
            {
                return Prism::Renderer::RenderGraphBlackboardPublication{
                    producer,
                    Prism::Renderer::RenderGraphBlackboardValueScope::ViewLocal,
                    0u,
                    graph.GetResourceGeneration(),
                    1u};
            };
            graph.GetBlackboard().Publish<
                Prism::Renderer::ClusteredLightingFeatureSlot>(
                    Prism::Renderer::ClusteredLightingGraphContribution{
                        &sharedClusterConstants,
                        &sharedPointLights,
                        &sharedClusterLightCounts,
                        &sharedClusterLightIndices,
                        Prism::RHI::ResourceState::UnorderedAccess,
                        Prism::RHI::ResourceState::UnorderedAccess,
                        sharedCallback},
                    metadata("test.clustered-lighting"));
            graph.GetBlackboard().Publish<
                Prism::Renderer::LocalLightShadowsFeatureSlot>(
                    Prism::Renderer::LocalLightShadowsGraphContribution{
                        &sharedLocalLightConstants,
                        &sharedLocalShadowRenderConstants,
                        &sharedSpotShadow,
                        &sharedPointShadow,
                        Prism::RHI::ResourceState::ShaderResource,
                        Prism::RHI::ResourceState::ShaderResource,
                        sharedCallback,
                        sharedCallback},
                    metadata("test.local-light-shadows"));
            graph.GetBlackboard().Publish<
                Prism::Renderer::VarianceShadowMapsFeatureSlot>(
                    Prism::Renderer::VarianceShadowMapsGraphContribution{
                        &sharedShadowMoments,
                        &sharedShadowMomentsScratch,
                        Prism::RHI::ResourceState::ShaderResource,
                        Prism::RHI::ResourceState::ShaderResource,
                        sharedCallback,
                        sharedCallback,
                        sharedCallback},
                    metadata("test.variance-shadows"));
            graph.GetBlackboard().Publish<
                Prism::Renderer::PlanarReflectionsFeatureSlot>(
                    Prism::Renderer::PlanarReflectionsGraphContribution{
                        &sharedPlanarReflection,
                        &sharedPlanarReflectionDepth,
                        Prism::RHI::ResourceState::ShaderResource,
                        Prism::RHI::ResourceState::DepthWrite,
                        sharedCallback},
                    metadata("test.planar-reflections"));
            graph.GetBlackboard().Publish<
                Prism::Renderer::ScreenSpaceEffectsFeatureSlot>(
                    Prism::Renderer::ScreenSpaceEffectsGraphContribution{
                        &sharedAmbientOcclusion,
                        &sharedScreenSpaceColor,
                        Prism::RHI::ResourceState::Undefined,
                        Prism::RHI::ResourceState::Undefined,
                        sharedCallback,
                        sharedCallback},
                    metadata("test.screen-space-effects"));
            graph.GetBlackboard().Publish<
                Prism::Renderer::TemporalAntiAliasingFeatureSlot>(
                    Prism::Renderer::TemporalAntiAliasingGraphContribution{
                        &sharedMotionVectors,
                        &sharedTemporalResolved,
                        &sharedTemporalHistoryRead,
                        &sharedTemporalHistoryWrite,
                        Prism::RHI::ResourceState::Undefined,
                        Prism::RHI::ResourceState::Undefined,
                        Prism::RHI::ResourceState::Undefined,
                        Prism::RHI::ResourceState::Undefined,
                        sharedCallback},
                    metadata("test.temporal-aa"));
            graph.GetBlackboard().Publish<
                Prism::Renderer::FftOceanFeatureSlot>(
                    Prism::Renderer::FftOceanGraphContribution{
                        {&sharedOceanSpectrumA[0], &sharedOceanSpectrumA[1]},
                        {&sharedOceanSpectrumB[0], &sharedOceanSpectrumB[1]},
                        &sharedOceanDisplacement,
                        &sharedOceanNormalFoam,
                        {},
                        {},
                        Prism::RHI::ResourceState::Undefined,
                        Prism::RHI::ResourceState::Undefined,
                        sharedCallback},
                    metadata("test.fft-ocean"));
            graph.GetBlackboard().Publish<
                Prism::Renderer::SpectralOceanFeatureSlot>(
                    Prism::Renderer::SpectralOceanGraphContribution{
                        &sharedOceanInitialSpectrum,
                        {&sharedOceanSpectrumA[0], &sharedOceanSpectrumA[1]},
                        {&sharedOceanSpectrumB[0], &sharedOceanSpectrumB[1]},
                        &sharedOceanDisplacement,
                        &sharedOceanNormalFoam,
                        &sharedOceanSlopeMoments,
                        {&sharedOceanFoamHistory[0], &sharedOceanFoamHistory[1]},
                        Prism::RHI::ResourceState::Undefined,
                        {},
                        {},
                        Prism::RHI::ResourceState::Undefined,
                        Prism::RHI::ResourceState::Undefined,
                        Prism::RHI::ResourceState::Undefined,
                        {},
                        0u,
                        1u,
                        sharedSpectralRebuild,
                        sharedSpectralCallbacks},
                    {"test.spectral-ocean",
                     Prism::Renderer::RenderGraphBlackboardValueScope::DeviceShared,
                     0u,
                     graph.GetResourceGeneration(),
                     1u});
        };
        const auto buildSharedGraph = [&] (
            const Prism::Renderer::SharedRenderGraphResources& resources,
            const Prism::Renderer::SharedRenderGraphOptions& options,
            const Prism::Renderer::SharedRenderGraphCallbacks& callbacks)
        {
            publishLightingAndShadowContributions();
            return Prism::Renderer::SharedRenderGraphFrontend::Build(
                graph, resources, options, callbacks);
        };
        const auto sharedHandles =
            buildSharedGraph(sharedResources, {}, sharedCallbacks);
        graph.Execute(commandContext);
        const auto sharedSummary =
            graph.GetCompilationSummary();
        const auto sharedPasses =
            graph.GetPassDescriptions();
        const auto screenSpacePass = std::ranges::find(
            sharedPasses,
            "ScreenSpaceReflections",
            &Prism::Renderer::RenderGraph::PassDescription::name);
        const auto temporalPass = std::ranges::find(
            sharedPasses,
            "TemporalResolve",
            &Prism::Renderer::RenderGraph::PassDescription::name);
        const auto bloomPass = std::ranges::find(
            sharedPasses,
            "BloomExtract",
            &Prism::Renderer::RenderGraph::PassDescription::name);
        const auto tonemapPass = std::ranges::find(
            sharedPasses,
            "Tonemap",
            &Prism::Renderer::RenderGraph::PassDescription::name);
        Expect(
            sharedCallbackCount == 16
                && sharedSummary.declaredPassCount == 17
                && sharedSummary.activePassCount == 17
                && sharedSummary.parameterPassCount == 17
                && sharedSummary.registeredTextureCount
                    == 21
                && sharedSummary.registeredBufferCount
                    == 7
                && sharedSummary
                       .versionedResourceCount == 20
                && sharedPasses.front().name
                    == "Shadow"
                && sharedPasses.back().name
                    == "OutputReady"
                && sharedHandles.outputColor.version
                    == 1
                && graph.GetBlackboard()
                       .Contains<Prism::Renderer::
                           SharedSceneGraphHandles>(),
            "The shared D3D12/Vulkan render frontend produced an invalid parameter graph.");
        Expect(
            screenSpacePass != sharedPasses.end()
                && temporalPass != sharedPasses.end()
                && bloomPass != sharedPasses.end()
                && tonemapPass != sharedPasses.end()
                && screenSpacePass < temporalPass
                && temporalPass < bloomPass
                && bloomPass < tonemapPass,
            "Screen-space, temporal, bloom, and tonemap order changed.");

        // Static descriptor consumers must share Fluid's resource identity.
        // Otherwise native execution releases the cubemap in the prologue,
        // before the sky/material graphics readers have used it.
        {
            MockTexture environment;
            MockTexture fluidComposite;
            auto fluidResources = sharedResources;
            fluidResources.environment = &environment;
            auto fluidCallbacks = sharedCallbacks;
            const Prism::Renderer::FluidGraphCallback fluidBuild =
                [&](Prism::Renderer::RenderGraph& fluidGraph,
                const Prism::Renderer::FluidGraphInputs& inputs)
            {
                Expect(inputs.environment.IsValid(), "Fluid must receive the shared cubemap handle.");
                auto result = fluidGraph.ImportTexture("Fluid.Composite", fluidComposite,
                    Prism::RHI::ResourceState::Undefined);
                auto parameters = fluidGraph.CreatePassParameters();
                parameters.ReadTexture(inputs.sceneColor, Prism::RHI::ResourceState::ShaderResource);
                parameters.ReadTexture(inputs.sceneDepth, Prism::RHI::ResourceState::ShaderResource);
                parameters.ReadTexture(inputs.environment, Prism::RHI::ResourceState::ShaderResource);
                result = parameters.WriteTexture(result, Prism::RHI::ResourceState::UnorderedAccess);
                fluidGraph.AddParameterPass("Fluid.Composite", std::move(parameters), sharedCallback,
                    Prism::Renderer::RenderGraph::PassOptions{
                        Prism::Renderer::RenderGraph::QueueClass::Compute,
                        false, true, true,
                        Prism::Renderer::RenderGraph::ParallelRecordingContract::
                            AuditedIndependent()});
                return Prism::Renderer::FluidGraphResult{result};
            };
            for (const bool deferred : {false, true})
            {
                graph.Reset();
                graph.GetBlackboard().Publish<
                    Prism::Renderer::FluidFeatureSlot>(
                        Prism::Renderer::FluidGraphContribution{fluidBuild},
                        {"test.fluid",
                         Prism::Renderer::RenderGraphBlackboardValueScope::ViewLocal,
                         0u,
                         graph.GetResourceGeneration(),
                         1u});
                Prism::Renderer::SharedRenderGraphOptions options{};
                options.fluidEnabled = true;
                options.deferredRenderingEnabled = deferred;
                options.planarReflectionsEnabled = deferred;
                (void)buildSharedGraph(
                    fluidResources, options, fluidCallbacks);
                graph.Compile();
                const auto passes = graph.GetPassDescriptions();
                for (const std::string_view name : {
                    deferred ? "DeferredLighting" : "ForwardGeometry",
                    "TransparentGeometry", "Fluid.Composite"})
                {
                    const auto pass = std::ranges::find(passes, name, &Prism::Renderer::RenderGraph::PassDescription::name);
                    Expect(pass != passes.end() && std::ranges::any_of(pass->reads,
                        [](const auto& read) { return read.resource == "Environment"
                            && read.state == Prism::RHI::ResourceState::ShaderResource; }),
                        "Every sky/material/Fluid consumer must declare the same cubemap read.");
                }
                const auto resources = graph.GetResourceDescriptions();
                Expect(std::ranges::count_if(resources, [](const auto& resource) {
                    return resource.name == "Environment" || resource.name == "Fluid.Environment";
                }) == 1, "The shared cubemap must not have a private Fluid graph alias.");
            }
            graph.Reset();
        }

        graph.Reset();
        sharedCallbackCount = 0;
        Prism::Renderer::SharedRenderGraphOptions
            atmosphereOptions{};
        atmosphereOptions.physicalAtmosphereEnabled =
            true;
        graph.GetBlackboard().Publish<
            Prism::Renderer::SkyAtmosphereFeatureSlot>(
                Prism::Renderer::SkyAtmosphereGraphContribution{
                    &sharedAtmosphereTransmittance,
                    &sharedAtmosphereSkyView,
                    Prism::RHI::ResourceState::Undefined,
                    Prism::RHI::ResourceState::Undefined,
                    sharedCallback,
                    sharedCallback},
                {"test.sky-atmosphere",
                 Prism::Renderer::RenderGraphBlackboardValueScope::ViewLocal,
                 0u,
                 graph.GetResourceGeneration(),
                 1u});
        const auto atmosphereHandles =
            buildSharedGraph(
                sharedResources,
                atmosphereOptions,
                sharedCallbacks);
        graph.Execute(commandContext);
        const auto atmospherePasses =
            graph.GetPassDescriptions();
        Expect(
            sharedCallbackCount == 18
                && atmospherePasses.size() == 19
                && atmospherePasses[0].name
                    == "AtmosphereTransmittance"
                && atmospherePasses[1].name
                    == "AtmosphereSkyView"
                && atmosphereHandles
                       .atmosphereTransmittance
                       .version == 1
                && atmosphereHandles
                       .atmosphereSkyView
                       .version == 1,
            "Physical atmosphere LUT passes must execute before scene lighting and publish sampled versions.");

        // Logical-frame scheduling suppresses mutation on a secondary view,
        // but the view must retain explicit GPU dependencies on the shared
        // terrain textures. A CPU producer flag alone cannot represent those
        // ordering and state-transition requirements.
        graph.Reset();
        Prism::Renderer::SharedRenderGraphOptions
            terrainConsumerOptions{};
        terrainConsumerOptions.interactiveTerrainEnabled = true;
        terrainConsumerOptions.interactiveTerrainUpdateEnabled = false;
        const auto publishTerrainContribution = [&]()
        {
            graph.GetBlackboard().Publish<
                Prism::Renderer::InteractiveTerrainFeatureSlot>(
                    Prism::Renderer::InteractiveTerrainGraphContribution{
                        &sharedTerrainRawHeight,
                        &sharedTerrainErodedHeight,
                        Prism::RHI::ResourceState::Undefined,
                        Prism::RHI::ResourceState::Undefined,
                        sharedCallback},
                    {"test.interactive-terrain",
                     Prism::Renderer::RenderGraphBlackboardValueScope::DeviceShared,
                     0u,
                     graph.GetResourceGeneration(),
                    1u});
        };
        const auto publishGpuDrivenContribution = [&]()
        {
            graph.GetBlackboard().Publish<
                Prism::Renderer::GpuDrivenVisibilityFeatureSlot>(
                    Prism::Renderer::GpuDrivenVisibilityGraphContribution{
                        &matrixGpuObjectRecords,
                        &matrixIndirectArguments,
                        &matrixIndirectDrawCounts,
                        Prism::RHI::ResourceState::UnorderedAccess,
                        Prism::RHI::ResourceState::UnorderedAccess,
                        Prism::RHI::ResourceState::UnorderedAccess,
                        sharedCallback},
                    {"test.gpu-driven-visibility",
                     Prism::Renderer::RenderGraphBlackboardValueScope::ViewLocal,
                     0u,
                     graph.GetResourceGeneration(),
                     1u});
        };
        publishTerrainContribution();
        const auto terrainConsumerHandles =
            buildSharedGraph(
                sharedResources, terrainConsumerOptions,
                sharedCallbacks);
        graph.Compile();
        const auto terrainConsumerPasses = graph.GetPassDescriptions();
        const bool consumerMutatesTerrain = std::ranges::any_of(
            terrainConsumerPasses, [](const auto& pass)
            {
                return pass.name == "InteractiveTerrain.BrushAndErosion";
            });
        const auto consumerGBuffer = std::ranges::find(
            terrainConsumerPasses, "GBuffer",
            &Prism::Renderer::RenderGraph::PassDescription::name);
        Expect(terrainConsumerHandles.terrainRawHeight.IsValid()
                && terrainConsumerHandles.terrainErodedHeight.IsValid()
                && !consumerMutatesTerrain
                && consumerGBuffer != terrainConsumerPasses.end()
                && std::ranges::any_of(consumerGBuffer->reads,
                    [](const auto& read)
                    {
                        return read.resource == "TerrainErodedHeight"
                            && read.state
                                == Prism::RHI::ResourceState::ShaderResource;
                    }),
            "A secondary terrain view must keep GPU read dependencies without recording the shared mutation pass.");

        graph.Reset();
        terrainConsumerOptions.interactiveTerrainUpdateEnabled = true;
        publishTerrainContribution();
        (void)buildSharedGraph(
            sharedResources, terrainConsumerOptions,
            sharedCallbacks);
        graph.Compile();
        const auto terrainProducerPasses = graph.GetPassDescriptions();
        Expect(std::ranges::count_if(terrainProducerPasses,
                   [](const auto& pass)
                   {
                       return pass.name
                           == "InteractiveTerrain.BrushAndErosion";
                   }) == 1,
            "The selected logical-frame producer must record exactly one terrain mutation pass.");

        graph.Reset();
        sharedCallbackCount = 0;
        Prism::Renderer::SharedRenderGraphOptions
            oceanOptions{};
        oceanOptions.fftOceanEnabled = true;
        const auto oceanHandles =
            buildSharedGraph(
                sharedResources,
                oceanOptions,
                sharedCallbacks);
        graph.Execute(commandContext);
        const auto oceanPasses =
            graph.GetPassDescriptions();
        Expect(
            sharedCallbackCount == 17
                && oceanPasses.size() == 18
                && oceanPasses[0].name
                    == "FftOcean.SpectrumAndIfft"
                && oceanHandles.oceanDisplacement.version == 1
                && oceanHandles.oceanNormalFoam.version == 1,
            "FFT ocean must publish displacement and normal/foam textures before scene geometry.");

        graph.Reset();
        sharedCallbackCount = 0;
        Prism::Renderer::SharedRenderGraphOptions spectralOceanOptions{};
        spectralOceanOptions.spectralOceanEnabled = true;
        spectralOceanOptions.oceanImplementation =
            Prism::Renderer::OceanImplementation::SpectralOcean;
        sharedSpectralCallbacks.initialSpectrum = sharedCallback;
        sharedSpectralCallbacks.evolution = sharedCallback;
        sharedSpectralCallbacks.horizontalFft = sharedCallback;
        sharedSpectralCallbacks.verticalFft = sharedCallback;
        sharedSpectralCallbacks.outputMaps = sharedCallback;
        sharedSpectralCallbacks.foam = sharedCallback;
        sharedSpectralCallbacks.mips = sharedCallback;
        sharedSpectralRebuild = true;
        const auto sharedSpectralHandles =
            buildSharedGraph(
                sharedResources, spectralOceanOptions,
                sharedCallbacks);
        graph.Execute(commandContext);
        const auto sharedSpectralPasses = graph.GetPassDescriptions();
        const bool hasLegacyOceanPass = std::ranges::any_of(
            sharedSpectralPasses, [](const auto& pass)
            {
                return pass.name == "FftOcean.SpectrumAndIfft";
            });
        const std::size_t spectralPassCount = std::ranges::count_if(
            sharedSpectralPasses, [](const auto& pass)
            {
                return pass.name.starts_with("SpectralOcean.");
            });
        Expect(!hasLegacyOceanPass && spectralPassCount == 7u
                && sharedSpectralHandles.oceanSlopeMoments.IsValid(),
            "Spectral selection must schedule only the seven-stage spectral path.");

        graph.Reset();
        sharedCallbackCount = 0;
        waterOpticsBoundaryCount = 0;
        Prism::Renderer::SharedRenderGraphOptions hpWaterOptions =
            spectralOceanOptions;
        hpWaterOptions.oceanOpticsModel =
            Prism::Renderer::OceanOpticsModel::HpWater;
        publishWaterContribution();
        const auto hpWaterHandles =
            buildSharedGraph(
                sharedResources, hpWaterOptions,
                sharedCallbacks);
        graph.SetPassCullingEnabled(true);
        graph.Execute(commandContext);
        const auto hpWaterPasses = graph.GetPassDescriptions();
        const auto passIndex = [&](const std::string_view name)
        {
            const auto found = std::ranges::find_if(
                hpWaterPasses,
                [name](const auto& pass)
                {
                    return pass.name == name;
                });
            return static_cast<std::size_t>(
                std::distance(hpWaterPasses.begin(), found));
        };
        const std::size_t gbufferIndex = passIndex("GBuffer");
        const std::size_t hiZReadyIndex = passIndex("HiZReady");
        const std::size_t depthCopyIndex =
            passIndex("WaterOptics.DepthCopy");
        const std::size_t visibilityIndex =
            passIndex("WaterOptics.Visibility");
        const std::size_t approximateRefractionIndex =
            passIndex("WaterOptics.Refraction.Approximate");
        const std::size_t rayMarchRefractionIndex =
            passIndex("WaterOptics.Refraction.RayMarch");
        const std::size_t compositeIndex =
            passIndex("WaterOptics.Composite");
        const std::size_t publishIndex =
            passIndex("WaterOptics.Publish");
        const std::size_t transparentIndex = passIndex("TransparentGeometry");
        const std::size_t hpWaterSpectralPassCount =
            std::ranges::count_if(hpWaterPasses, [](const auto& pass)
            {
                return pass.name.starts_with("SpectralOcean.");
            });
        const std::size_t waterOpticsPassCount =
            std::ranges::count_if(hpWaterPasses, [](const auto& pass)
            {
                return pass.name.starts_with("WaterOptics.");
            });
        const auto& transparentPass = hpWaterPasses.at(transparentIndex);
        const auto& compositePass = hpWaterPasses.at(compositeIndex);
        const auto& publishPass = hpWaterPasses.at(publishIndex);
        const auto compositeReads = [&](const std::string_view resource)
        {
            return std::ranges::any_of(compositePass.reads,
                [resource](const auto& access)
                {
                    return access.resource == resource;
                });
        };
        const bool compositeWritesDedicatedHdr =
            std::ranges::any_of(compositePass.writes,
                [](const auto& access)
                {
                    return access.resource == "WaterCompositeScratch";
                });
        const bool publishReadsDedicatedHdr =
            std::ranges::any_of(publishPass.reads,
                [](const auto& access)
                {
                    return access.resource == "WaterCompositeScratch";
                });
        const bool publishWritesAuthoritativeOutputs =
            std::ranges::any_of(publishPass.writes,
                [](const auto& access)
                {
                    return access.resource == "HdrColor";
                })
            && std::ranges::any_of(publishPass.writes,
                [](const auto& access)
                {
                    return access.resource == "MotionVectors";
                });
        const bool transparencyReadsCompositeDepth =
            std::ranges::any_of(transparentPass.reads,
                [](const auto& access)
                {
                    return access.resource == "WaterCompositeDepth"
                        && access.version == 2u;
                });
        const bool transparencyTouchesOpaqueDepth =
            std::ranges::any_of(transparentPass.reads,
                [](const auto& access)
                {
                    return access.resource == "DepthBuffer";
                })
            || std::ranges::any_of(transparentPass.writes,
                [](const auto& access)
                {
                    return access.resource == "DepthBuffer";
                });
        Expect(gbufferIndex < hiZReadyIndex
                && hiZReadyIndex < depthCopyIndex
                && depthCopyIndex < visibilityIndex
                && visibilityIndex < approximateRefractionIndex
                && approximateRefractionIndex < compositeIndex
                && compositeIndex < publishIndex
                && publishIndex < transparentIndex
                && rayMarchRefractionIndex == hpWaterPasses.size()
                && waterOpticsBoundaryCount == 1u
                && hpWaterSpectralPassCount == 7u
                && waterOpticsPassCount == 5u
                && compositeReads("HdrColor")
                && compositeReads("DepthBuffer")
                && compositeReads("WaterRefraction")
                && compositeReads("WaterGBuffer0")
                && compositeReads("WaterGBuffer1")
                && compositeReads("WaterGBuffer2")
                && compositeReads("OceanSlopeMoments")
                && compositeReads("ShadowMap")
                && compositeWritesDedicatedHdr
                && publishReadsDedicatedHdr
                && publishWritesAuthoritativeOutputs
                && !hpWaterPasses[depthCopyIndex].culled
                && !hpWaterPasses[visibilityIndex].culled
                && hpWaterHandles.depthBuffer.version == 1u
                && hpWaterHandles.waterCompositeDepth.version == 3u
                && hpWaterHandles.waterMotion.version == 1u
                && std::ranges::all_of(
                    hpWaterHandles.waterGBuffer,
                    [](const auto handle)
                    {
                        return handle.version == 1u;
                    })
                && transparencyReadsCompositeDepth
                && !transparencyTouchesOpaqueDepth,
            "HPWater must preserve opaque depth/Hi-Z, update composite depth after visibility, and route transparency to that updated version.");
        graph.Reset();
        sharedWaterOptions.rayMarchEnabled = true;
        sharedWaterOptions.effectiveRayMarchSamples = 8u;
        publishWaterContribution();
        const auto highQualityHandles =
            buildSharedGraph(
                sharedResources, hpWaterOptions,
                sharedCallbacks);
        (void)highQualityHandles;
        graph.Execute(commandContext);
        const auto highQualityPasses = graph.GetPassDescriptions();
        const bool highHasRayMarch = std::ranges::any_of(
            highQualityPasses, [](const auto& pass)
            {
                return pass.name == "WaterOptics.Refraction.RayMarch";
            });
        const bool highHasApproximation = std::ranges::any_of(
            highQualityPasses, [](const auto& pass)
            {
                return pass.name == "WaterOptics.Refraction.Approximate";
            });
        Expect(highHasRayMarch && !highHasApproximation
                && sharedWaterOptions.effectiveRayMarchSamples == 8u,
            "High-quality water optics must schedule only the effective ray-march refraction pass.");
        for (std::uint32_t readIndex = 0u; readIndex < 2u; ++readIndex)
        {
            graph.Reset();
            sharedWaterOptions.volumetricsEnabled = true;
            sharedWaterOptions.volumetricReadIndex = readIndex;
            publishWaterContribution();
            (void)buildSharedGraph(
                sharedResources, hpWaterOptions, sharedCallbacks);
            graph.Execute(commandContext);
            const auto passes = graph.GetPassDescriptions();
            const auto findPass = [&](std::string_view name) {
                return std::ranges::find_if(passes, [name](const auto& pass) { return pass.name == name; });
            };
            const auto accumulate = findPass("WaterOptics.Volume.Accumulate");
            const auto temporal = findPass("WaterOptics.Volume.Temporal");
            const auto reconstruct = findPass("WaterOptics.Volume.Reconstruct");
            const auto composite = findPass("WaterOptics.Composite");
            Expect(accumulate != passes.end() && temporal != passes.end()
                    && reconstruct != passes.end() && composite != passes.end()
                    && accumulate < temporal && temporal < reconstruct && reconstruct < composite,
                "Volumetrics must accumulate, reproject and reconstruct before HDR composition.");
            const auto reads = [&](std::string_view resource) {
                return std::ranges::any_of(temporal->reads, [resource](const auto& a) { return a.resource == resource; });
            };
            Expect(reads("WaterVolume" + std::to_string(1u + readIndex))
                    && reads("WaterGBuffer2") && reads("WaterMotion")
                    && std::ranges::any_of(temporal->writes, [readIndex](const auto& a) {
                        return a.resource == "WaterVolume" + std::to_string(2u - readIndex);
                    }), "Temporal volume pass must use distinct ping-pong histories and coherent water motion/mask.");
        }
        sharedWaterOptions.volumetricsEnabled = false;
        Prism::Renderer::SpectralOceanSimulation spectralSimulation;
        Prism::Renderer::OceanSettings spectralSettings =
            Prism::Renderer::OceanSettings::WaveWorksReference();
        Expect(spectralSimulation.ConfigureSpectrum(spectralSettings),
            "The first spectral configuration must dirty the initial cache.");
        Expect(spectralSimulation.PendingSpectrumVersion() == 1u
                && spectralSimulation.ActiveSpectrumVersion() == 0u,
            "The first spectrum edit was not staged as a pending version.");
        MockTexture initialSpectrumTexture;
        const auto buildSpectralHandles = [&]()
        {
            Prism::Renderer::SpectralOceanGraphHandles handles{};
            handles.initialSpectrum = graph.ImportTexture(
                "SpectralInitialSpectrum", initialSpectrumTexture,
                Prism::RHI::ResourceState::Undefined);
            for (std::size_t index = 0; index < 2; ++index)
            {
                handles.spectrumA[index] = graph.ImportTexture(
                    "SpectralWorkingA" + std::to_string(index),
                    sharedOceanSpectrumA[index],
                    Prism::RHI::ResourceState::Undefined);
                handles.spectrumB[index] = graph.ImportTexture(
                    "SpectralWorkingB" + std::to_string(index),
                    sharedOceanSpectrumB[index],
                    Prism::RHI::ResourceState::Undefined);
            }
            handles.displacement = graph.ImportTexture(
                "SpectralDisplacement", sharedOceanDisplacement,
                Prism::RHI::ResourceState::Undefined);
            handles.gradientFoam = graph.ImportTexture(
                "SpectralGradientFoam", sharedOceanNormalFoam,
                Prism::RHI::ResourceState::Undefined);
            handles.slopeMoments = graph.ImportTexture(
                "SpectralSlopeMoments", sharedOceanSlopeMoments,
                Prism::RHI::ResourceState::Undefined);
            for (std::size_t index = 0; index < 2; ++index)
            {
                handles.foamHistory[index] = graph.ImportTexture(
                    "SpectralFoamHistory" + std::to_string(index),
                    sharedOceanFoamHistory[index],
                    Prism::RHI::ResourceState::Undefined);
            }
            return handles;
        };
        Prism::Renderer::SpectralOceanGraphCallbacks spectralCallbacks{};
        spectralCallbacks.initialSpectrum = sharedCallback;
        spectralCallbacks.evolution = sharedCallback;
        spectralCallbacks.horizontalFft = sharedCallback;
        spectralCallbacks.verticalFft = sharedCallback;
        spectralCallbacks.outputMaps = sharedCallback;
        spectralCallbacks.foam = sharedCallback;
        spectralCallbacks.mips = sharedCallback;

        graph.Reset();
        sharedCallbackCount = 0;
        auto spectralHandles = buildSpectralHandles();
        Prism::Renderer::SpectralOceanSimulation::AddPasses(
            graph, spectralHandles,
            spectralSimulation.IsInitialSpectrumRebuildPending(),
            spectralCallbacks);
        graph.Execute(commandContext);
        const auto initialBuildPasses = graph.GetPassDescriptions();
        const std::array<std::string_view, 7> expectedSpectralPasses{
            "SpectralOcean.InitialSpectrum",
            "SpectralOcean.Evolution",
            "SpectralOcean.Fft.Horizontal",
            "SpectralOcean.Fft.Vertical",
            "SpectralOcean.OutputMaps",
            "SpectralOcean.Foam",
            "SpectralOcean.Mips"};
        bool expectedEdges = initialBuildPasses.size()
            == expectedSpectralPasses.size();
        for (std::size_t index = 0;
             expectedEdges && index < initialBuildPasses.size(); ++index)
        {
            expectedEdges = initialBuildPasses[index].name
                == expectedSpectralPasses[index];
            if (index > 0u)
            {
                expectedEdges = expectedEdges
                    && std::ranges::find(
                        initialBuildPasses[index].dependencies,
                        index - 1u)
                        != initialBuildPasses[index].dependencies.end();
            }
        }
        Expect(sharedCallbackCount == 7 && expectedEdges,
            "Spectral graph stages or declared dependency edges are invalid.");

        graph.Reset();
        sharedCallbackCount = 0;
        spectralHandles = buildSpectralHandles();
        spectralCallbacks.asyncCompute = true;
        Prism::Renderer::SpectralOceanSimulation::AddPasses(
            graph, spectralHandles, false, spectralCallbacks);
        const auto asyncSpectralPasses = graph.GetPassDescriptions();
        Expect(!asyncSpectralPasses.empty()
                && std::ranges::all_of(asyncSpectralPasses,
                    [](const auto& pass)
                    {
                        return pass.queue
                            == Prism::Renderer::RenderGraph::QueueClass::Compute;
                    }),
            "Async spectral selection did not move every eligible stage to the compute queue.");

        // Exercise the same lifecycle used by runtime quality/pause/scene
        // changes while async work is selected.  Rebuilding the graph and
        // rotating the cascade mask repeatedly catches stale pass handles,
        // retained callbacks, and queue metadata that survives Reset().
        for (std::uint32_t stressIteration = 0u;
             stressIteration < 96u; ++stressIteration)
        {
            graph.Reset();
            spectralCallbacks.asyncCompute = (stressIteration & 1u) != 0u;
            spectralHandles = buildSpectralHandles();
            Prism::Renderer::SpectralOceanSimulation::AddPasses(
                graph, spectralHandles, (stressIteration % 7u) == 0u,
                spectralCallbacks);
            const auto stressPasses = graph.GetPassDescriptions();
            Expect(stressPasses.size() == ((stressIteration % 7u) == 0u ? 7u : 6u),
                "Async ocean lifecycle stress produced an incomplete pass set.");
            if (spectralCallbacks.asyncCompute)
            {
                Expect(std::ranges::all_of(stressPasses,
                        [](const auto& pass)
                        {
                            return pass.queue
                                == Prism::Renderer::RenderGraph::QueueClass::Compute;
                        }),
                    "Async ocean lifecycle stress lost compute queue ownership.");
            }
        }
        spectralCallbacks.asyncCompute = false;

        spectralSimulation.MarkInitialSpectrumCommitted();
        Expect(!spectralSimulation.ConfigureSpectrum(spectralSettings)
                && !spectralSimulation.IsInitialSpectrumRebuildPending(),
            "Advancing time with unchanged spectrum settings must reuse h0.");
        graph.Reset();
        sharedCallbackCount = 0;
        spectralHandles = buildSpectralHandles();
        Prism::Renderer::SpectralOceanSimulation::AddPasses(
            graph, spectralHandles, false, spectralCallbacks);
        graph.Execute(commandContext);
        const auto cachedPasses = graph.GetPassDescriptions();
        Expect(sharedCallbackCount == 6 && cachedPasses.size() == 6
                && cachedPasses.front().name == "SpectralOcean.Evolution"
                && std::ranges::none_of(cachedPasses,
                    [](const Prism::Renderer::RenderGraph::PassDescription& pass)
                    {
                        return pass.name == "SpectralOcean.InitialSpectrum";
                    }),
            "Time-only evolution scheduled an initial-spectrum rebuild.");

        spectralSettings.baseWind.direction = {0.6f, 0.8f};
        Expect(spectralSimulation.ConfigureSpectrum(spectralSettings),
            "Changing wind direction must dirty the initial spectrum once.");
        const std::uint64_t windVersion =
            spectralSimulation.PendingSpectrumVersion();
        spectralSettings.baseWind.fetchKilometers += 10.0f;
        Expect(spectralSimulation.ConfigureSpectrum(spectralSettings)
                && spectralSimulation.PendingSpectrumVersion()
                    == windVersion + 1u
                && spectralSimulation.ActiveSpectrumVersion() <
                    spectralSimulation.PendingSpectrumVersion(),
            "Rapid spectrum edits did not retain a newer pending version over the active version.");
        // The graph below represents the latest coherent settings snapshot.
        spectralSimulation.MarkInitialSpectrumCommitted();
        Expect(!spectralSimulation.IsInitialSpectrumRebuildPending()
                && spectralSimulation.ActiveSpectrumVersion()
                    == spectralSimulation.PendingSpectrumVersion(),
            "A coherent spectrum commit did not atomically publish the latest version.");
        graph.Reset();
        sharedCallbackCount = 0;
        spectralHandles = buildSpectralHandles();
        Prism::Renderer::SpectralOceanSimulation::AddPasses(
            graph, spectralHandles, true, spectralCallbacks);
        graph.Execute(commandContext);
        const auto windRebuildPasses = graph.GetPassDescriptions();
        Expect(sharedCallbackCount == 7
                && std::ranges::count_if(windRebuildPasses,
                    [](const Prism::Renderer::RenderGraph::PassDescription& pass)
                    {
                        return pass.name == "SpectralOcean.InitialSpectrum";
                    }) == 1,
            "A wind change did not schedule exactly one h0 rebuild pass.");

        graph.Reset();
        sharedCallbackCount = 0;
        Prism::Renderer::SharedRenderGraphOptions
            minimalDeferredOptions{};
        minimalDeferredOptions.shadowsEnabled = false;
        minimalDeferredOptions.clusteredLightingEnabled = false;
        minimalDeferredOptions.localLightShadowsEnabled = false;
        minimalDeferredOptions.planarReflectionsEnabled = false;
        minimalDeferredOptions.varianceShadowsEnabled = false;
        minimalDeferredOptions.gtaoEnabled = false;
        minimalDeferredOptions.screenSpaceReflectionsEnabled = false;
        minimalDeferredOptions.temporalAntiAliasingEnabled = false;
        minimalDeferredOptions.bloomEnabled = false;
        (void)buildSharedGraph(
            sharedResources,
            minimalDeferredOptions,
            sharedCallbacks);
        graph.Execute(commandContext);
        const auto minimalDeferredPasses =
            graph.GetPassDescriptions();
        Expect(
            sharedCallbackCount == 5
                && minimalDeferredPasses.size() == 5
                && minimalDeferredPasses[0].name == "GBuffer"
                && minimalDeferredPasses[1].name == "DeferredLighting"
                && minimalDeferredPasses[2].name == "TransparentGeometry"
                && minimalDeferredPasses[3].name == "Tonemap"
                && minimalDeferredPasses[4].name == "OutputReady",
            "Disabled deferred features must be removed from the render graph.");

        graph.Reset();
        sharedCallbackCount = 0;
        Prism::Renderer::SharedRenderGraphOptions
            forwardOptions = minimalDeferredOptions;
        forwardOptions.deferredRenderingEnabled = false;
        (void)buildSharedGraph(
            sharedResources,
            forwardOptions,
            sharedCallbacks);
        graph.Execute(commandContext);
        const auto forwardPasses =
            graph.GetPassDescriptions();
        Expect(
            sharedCallbackCount == 4
                && forwardPasses.size() == 4
                && forwardPasses[0].name == "ForwardGeometry"
                && forwardPasses[1].name == "TransparentGeometry"
                && forwardPasses[2].name == "Tonemap"
                && forwardPasses[3].name == "OutputReady",
            "The forward render path must not schedule deferred-only passes.");

        Prism::Renderer::RenderSettings forwardSettings{};
        forwardSettings.deferredRenderingEnabled = false;
        forwardSettings.forwardPlusEnabled = false;
        forwardSettings.screenSpaceReflectionsEnabled = true;
        forwardSettings.gtaoEnabled = true;
        forwardSettings.temporalAntiAliasingEnabled = true;
        const auto forwardPlan =
            Prism::Renderer::BuildScenePipelinePlan(
                forwardSettings,
                Prism::RHI::ResourceState::Present);
        Expect(
            forwardPlan.execution.forwardGeometry
                && !forwardPlan.execution.deferredGeometry
                && !forwardPlan.execution.clusteredLighting
                && !forwardPlan.execution.gtao
                && !forwardPlan.execution.screenSpaceComposite
                && !forwardPlan.execution.temporalAntiAliasing,
            "The scene pipeline plan did not remove deferred-only features from the forward path.");

        Prism::Renderer::RenderSettings reflectionSettings{};
        reflectionSettings.gpuDrivenEnabled = false;
        reflectionSettings.screenSpaceReflectionsEnabled = true;
        const auto reflectionPlan =
            Prism::Renderer::BuildScenePipelinePlan(
                reflectionSettings,
                Prism::RHI::ResourceState::Present);
        Expect(
            reflectionPlan.execution.screenSpaceComposite
                && reflectionPlan.execution.hiZ,
            "Screen-space reflections must make Hi-Z part of the resolved pipeline plan.");

        Prism::Renderer::RenderSettings fluidSettings{};
        fluidSettings.fluid.enabled = true;
        fluidSettings.temporalAntiAliasingEnabled = true;
        const auto fluidPlan =
            Prism::Renderer::BuildScenePipelinePlan(
                fluidSettings,
                Prism::RHI::ResourceState::Present);
        Expect(
            fluidPlan.execution.fluid
                && !fluidPlan.execution.temporalAntiAliasing,
            "Fluid rendering must bypass TAA until fluid motion vectors and a reactive mask are available.");

        Prism::Renderer::RenderSettings
            atmosphereSettings{};
        atmosphereSettings.physicalAtmosphereEnabled =
            true;
        const auto atmospherePlan =
            Prism::Renderer::BuildScenePipelinePlan(
                atmosphereSettings,
                Prism::RHI::ResourceState::Present);
        Expect(
            atmospherePlan.execution.skyAtmosphere
                && atmospherePlan.graphOptions
                       .physicalAtmosphereEnabled,
            "The scene pipeline plan must preserve an enabled physical atmosphere.");

        Prism::Renderer::ScenePipelineFrameState emptyFrameState{};
        emptyFrameState.usage = {};
        emptyFrameState.terrainPendingWork = false;
        Prism::Renderer::RenderSettings emptyFrameSettings{};
        emptyFrameSettings.interactiveTerrainEnabled = true;
        const auto emptyFramePlan =
            Prism::Renderer::BuildScenePipelinePlan(
                emptyFrameSettings,
                emptyFrameState,
                Prism::RHI::ResourceState::Present);
        Expect(
            !emptyFramePlan.execution.shadows
                && !emptyFramePlan.execution.clusteredLighting
                && !emptyFramePlan.execution.localLightShadows
                && !emptyFramePlan.execution.gtao
                && !emptyFramePlan.execution.screenSpaceComposite
                && !emptyFramePlan.execution.gpuDriven
                && !emptyFramePlan.execution.hiZ
                && !emptyFramePlan.graphOptions.interactiveTerrainEnabled
                && !emptyFramePlan.graphOptions
                        .interactiveTerrainUpdateEnabled
                && emptyFramePlan.execution.temporalAntiAliasing
                && emptyFramePlan.execution.bloom,
            "Empty frame content did not remove content-dependent work or preserve fullscreen work.");
        Expect(
            emptyFramePlan.shadowsDecision.rejectionReason
                    == Prism::Renderer::ScenePipelineRejectionReason::
                        StaticContentMissing
                && emptyFramePlan.clusteredLightingDecision.rejectionReason
                    == Prism::Renderer::ScenePipelineRejectionReason::
                        DynamicContentMissing
                && emptyFramePlan.terrainSamplingDecision.rejectionReason
                    == Prism::Renderer::ScenePipelineRejectionReason::
                        StaticContentMissing,
            "Empty frame activation decisions lost their rejection reasons.");

        Prism::Renderer::ScenePipelineFrameState pointLightFrame{};
        pointLightFrame.usage.hasDirectionalLight = false;
        pointLightFrame.usage.hasSpotLights = false;
        pointLightFrame.usage.hasShadowedSpotLights = false;
        pointLightFrame.usage.hasPointLights = true;
        pointLightFrame.usage.hasShadowedPointLights = true;
        const auto pointLightPlan =
            Prism::Renderer::BuildScenePipelinePlan(
                Prism::Renderer::RenderSettings{},
                pointLightFrame,
                Prism::RHI::ResourceState::Present);
        Expect(
            !pointLightPlan.execution.shadows
                && pointLightPlan.execution.clusteredLighting
                && pointLightPlan.execution.localLightShadows
                && !pointLightPlan.execution.spotShadows
                && pointLightPlan.execution.pointShadows,
            "Point-only frame did not resolve clustered and local-shadow families independently.");

        Prism::Renderer::ScenePipelineFrameState stableTerrainFrame{};
        stableTerrainFrame.terrainPendingWork = false;
        Prism::Renderer::RenderSettings terrainFrameSettings{};
        terrainFrameSettings.interactiveTerrainEnabled = true;
        const auto stableTerrainPlan =
            Prism::Renderer::BuildScenePipelinePlan(
                terrainFrameSettings,
                stableTerrainFrame,
                Prism::RHI::ResourceState::Present);
        Expect(
            stableTerrainPlan.graphOptions.interactiveTerrainEnabled
                && !stableTerrainPlan.graphOptions
                        .interactiveTerrainUpdateEnabled
                && stableTerrainPlan.terrainUpdateDecision.rejectionReason
                    == Prism::Renderer::ScenePipelineRejectionReason::
                        PendingWorkMissing,
            "Stable terrain sampling incorrectly retained mutation work.");

        Prism::Renderer::ScenePipelineFrameState unsupportedGpuFrame{};
        unsupportedGpuFrame.gpuDrivenSupported = false;
        Prism::Renderer::RenderSettings unsupportedGpuSettings{};
        unsupportedGpuSettings.gpuDrivenEnabled = true;
        const auto unsupportedGpuPlan =
            Prism::Renderer::BuildScenePipelinePlan(
                unsupportedGpuSettings,
                unsupportedGpuFrame,
                Prism::RHI::ResourceState::Present);
        Expect(
            !unsupportedGpuPlan.execution.gpuDriven
                && unsupportedGpuPlan.gpuDrivenDecision.rejectionReason
                    == Prism::Renderer::ScenePipelineRejectionReason::
                        CapabilityUnsupported,
            "Unsupported GPU-driven drawing did not report an explicit capability rejection.");

        struct SceneExpectation
        {
            Prism::Scene::DemoSceneId id;
            bool localLights;
            bool shadowedPointLights;
            bool shadowedSpotLights;
            bool terrain;
            bool gpuCandidate;
        };
        constexpr std::array sceneExpectations{
            SceneExpectation{Prism::Scene::DemoSceneId::EditorPreview, true, true, true, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::Showcase, true, true, true, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::ReflectionLab, true, true, true, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::ShadowLab, true, true, true, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::LightingLab, true, true, false, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::GpuDrivenLab, true, false, false, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::PostProcessLab, true, false, false, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::RenderGraphLab, true, true, false, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::MaterialLab, true, true, false, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::AssetStreamingLab, true, false, false, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::AtmosphereLab, false, false, false, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::LargeWorldLab, false, false, false, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::TerrainVirtualTextureLab, false, false, false, true, false},
            SceneExpectation{Prism::Scene::DemoSceneId::OceanLab, false, false, false, false, false},
            SceneExpectation{Prism::Scene::DemoSceneId::WaveWorksLab, false, false, false, false, false},
            SceneExpectation{Prism::Scene::DemoSceneId::HpWaterOceanLab, false, false, false, false, false},
            SceneExpectation{Prism::Scene::DemoSceneId::PbfLab, true, false, false, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::FluidRenderLab, true, false, false, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::FluidCausticsLab, true, false, false, false, true},
            SceneExpectation{Prism::Scene::DemoSceneId::FluidToonLab, true, false, false, false, true}};
        static_assert(sceneExpectations.size()
            == static_cast<std::size_t>(
                Prism::Scene::DemoSceneId::Count));
        for (std::size_t index = 0;
             index < sceneExpectations.size();
             ++index)
        {
            const SceneExpectation& expected =
                sceneExpectations[index];
            Expect(static_cast<std::size_t>(expected.id) == index,
                "The feature expectation matrix is not catalog-complete and ordered.");
            Prism::Renderer::RenderSettings settings{};
            Prism::Renderer::ApplyDemoSceneSettings(
                expected.id,
                settings);
            Prism::Renderer::ScenePipelineFrameState state{};
            state.usage.scene.hasTerrainSurface = expected.terrain;
            state.usage.scene.hasGpuDrivenCandidate =
                expected.gpuCandidate;
            state.usage.hasPointLights = expected.localLights;
            state.usage.hasSpotLights = false;
            state.usage.hasShadowedPointLights =
                expected.shadowedPointLights;
            state.usage.hasShadowedSpotLights =
                expected.shadowedSpotLights;
            const auto plan =
                Prism::Renderer::BuildScenePipelinePlan(
                    settings,
                    state,
                    Prism::RHI::ResourceState::Present);
            const bool clusteredExpected = expected.localLights
                && settings.clusteredLightingEnabled
                && (settings.deferredRenderingEnabled
                    || settings.forwardPlusEnabled);
            Expect(plan.execution.clusteredLighting
                    == clusteredExpected,
                "A catalog scene resolved an incorrect clustered-light family.");
            Expect(plan.execution.spotShadows
                    == (expected.shadowedSpotLights
                        && settings.shadowsEnabled
                        && settings.localLightShadowsEnabled
                        && settings.spotLightsEnabled
                        && settings.deferredRenderingEnabled),
                "A catalog scene resolved an incorrect spot-shadow family.");
            Expect(plan.execution.pointShadows
                    == (expected.shadowedPointLights
                        && settings.shadowsEnabled
                        && settings.localLightShadowsEnabled
                        && settings.pointLightsEnabled
                        && settings.deferredRenderingEnabled),
                "A catalog scene resolved an incorrect point-shadow family.");
            Expect(plan.graphOptions.interactiveTerrainEnabled
                    == expected.terrain,
                "A non-terrain catalog scene activated Terrain.");
            Expect(plan.execution.gpuDriven
                    == (settings.gpuDrivenEnabled
                        && expected.gpuCandidate),
                "A catalog scene resolved an incorrect GPU-driven family.");

            // Build a real graph for every catalog row. Disable unrelated
            // feature-lab implementations here so this matrix isolates the
            // content-driven pass families under test.
            graph.Reset();
            auto matrixOptions = plan.graphOptions;
            matrixOptions.planarReflectionsEnabled = false;
            matrixOptions.physicalAtmosphereEnabled = false;
            matrixOptions.fftOceanEnabled = false;
            matrixOptions.spectralOceanEnabled = false;
            matrixOptions.localWaveEnabled = false;
            matrixOptions.fluidEnabled = false;
            if (matrixOptions.interactiveTerrainEnabled)
                publishTerrainContribution();
            if (matrixOptions.gpuDrivenEnabled)
                publishGpuDrivenContribution();
            (void)buildSharedGraph(
                sharedResources,
                matrixOptions,
                sharedCallbacks);
            graph.Compile();
            const auto matrixPasses = graph.GetPassDescriptions();
            const auto hasPass = [&](const std::string_view name)
            {
                return std::ranges::any_of(matrixPasses,
                    [name](const auto& pass)
                    {
                        return pass.name == name;
                    });
            };
            const auto matrixExecution =
                Prism::Renderer::ResolveScenePipelineExecution(
                    matrixOptions);
            Expect(hasPass("Shadow") == matrixExecution.shadows
                    && hasPass("ClusteredLightBuild")
                        == matrixExecution.clusteredLighting
                    && hasPass("SpotShadows")
                        == matrixExecution.spotShadows
                    && hasPass("PointShadows")
                        == matrixExecution.pointShadows
                    && hasPass("GTAO") == matrixExecution.gtao
                    && hasPass("ScreenSpaceReflections")
                        == matrixExecution.screenSpaceComposite
                    && hasPass("GpuVisibility")
                        == matrixExecution.gpuDriven
                    && hasPass("InteractiveTerrain.BrushAndErosion")
                        == matrixOptions.interactiveTerrainUpdateEnabled,
                "A catalog scene's real graph disagreed with its resolved content-dependent pass families.");
            Expect(hasPass("HiZBuild") == matrixExecution.hiZ
                    && hasPass("TemporalResolve")
                        == matrixExecution.temporalAntiAliasing
                    && hasPass("BloomExtract")
                        == matrixExecution.bloom
                    && hasPass("Tonemap")
                    && hasPass("OutputReady"),
                "A catalog scene's real graph disagreed with its resolved Hi-Z or fullscreen pass families.");
        }

        const auto deferredTargets =
            Prism::Renderer::
                BuildDeferredTransientTextureRequests(
                    1920,
                    1080);
        const auto findTransientTarget =
            [&](const std::string_view name)
            {
                return std::ranges::find(
                    deferredTargets,
                    name,
                    &Prism::RHI::
                        TransientTextureRequest::name);
            };
        const auto gbuffer0 =
            findTransientTarget("GBuffer0");
        const auto bloomA =
            findTransientTarget("BloomA");
        std::string transientValidationError;
        Expect(
            gbuffer0 != deferredTargets.end()
                && bloomA != deferredTargets.end()
                && bloomA->description.width == 960
                && bloomA->description.height == 540
                && bloomA->allocationIndex
                    != gbuffer0->allocationIndex
                && Prism::RHI::
                    ValidateTransientTextureRequests(
                        deferredTargets,
                        &transientValidationError),
            "Half-resolution Bloom targets must use valid transient slots distinct from full-resolution GBuffer targets.");

        Prism::Renderer::WaterOpticsTransientLayoutConfig
            waterLayoutConfig{};
        waterLayoutConfig.width = 1920u;
        waterLayoutConfig.height = 1080u;
        waterLayoutConfig.refractionResolutionScale = 0.5f;
        const auto waterTargets = Prism::Renderer::
            BuildWaterOpticsTransientTextureRequests(
                waterLayoutConfig);
        const auto findWaterTarget = [&](const std::string_view name) {
            return std::ranges::find(
                waterTargets,
                name,
                &Prism::RHI::TransientTextureRequest::name);
        };
        const auto waterGBuffer0 = findWaterTarget("WaterGBuffer0");
        const auto waterGBuffer1 = findWaterTarget("WaterGBuffer1");
        const auto waterGBuffer2 = findWaterTarget("WaterGBuffer2");
        const auto waterDepth = findWaterTarget("WaterCompositeDepth");
        const auto waterRefraction = findWaterTarget("WaterRefraction");
        const auto waterVolume =
            findWaterTarget("WaterVolumetricCurrent");
        Expect(waterGBuffer0 != waterTargets.end()
                && waterGBuffer1 != waterTargets.end()
                && waterGBuffer2 != waterTargets.end()
                && waterDepth != waterTargets.end()
                && waterRefraction != waterTargets.end()
                && waterVolume != waterTargets.end()
                && waterGBuffer0->description.format
                    == Prism::RHI::Format::Rgba16Float
                && waterGBuffer1->description.format
                    == Prism::RHI::Format::Rgba8Unorm
                && waterGBuffer2->description.format
                    == Prism::RHI::Format::Rgba8Unorm
                && waterDepth->description.format
                    == Prism::RHI::Format::D32Float
                && waterRefraction->description.width == 960u
                && waterRefraction->description.height == 540u
                && waterVolume->description.width == 960u
                && waterVolume->description.height == 540u
                && Prism::RHI::ValidateTransientTextureRequests(
                    waterTargets, &transientValidationError),
            "Water optical transient targets did not preserve their named formats, scaled extents, or allocation contract.");
        Expect(Prism::Renderer::AreWaterOpticsTexturesAliasCompatible(
                   waterGBuffer1->description,
                   waterGBuffer2->description)
                && !Prism::Renderer::AreWaterOpticsTexturesAliasCompatible(
                    waterGBuffer0->description,
                    waterRefraction->description),
            "Water optical alias compatibility ignored format, usage, or extent.");

        waterLayoutConfig.width = 0u;
        waterLayoutConfig.height = 0u;
        waterLayoutConfig.volumetricResolutionScale = 0.0f;
        waterLayoutConfig.causticResolution = 1u;
        const auto minimumWaterTargets = Prism::Renderer::
            BuildWaterOpticsTransientTextureRequests(waterLayoutConfig);
        Expect(std::ranges::all_of(minimumWaterTargets,
                   [](const Prism::RHI::TransientTextureRequest& request) {
                       return request.description.width >= 1u
                           && request.description.height >= 1u;
                   })
                && Prism::RHI::ValidateTransientTextureRequests(
                    minimumWaterTargets, &transientValidationError),
            "Water optical layout must normalize zero-sized resize inputs to valid one-pixel resources.");

        graph.Reset();
        sharedCallbackCount = 0;
        Prism::Renderer::SharedRenderGraphOptions
            reflectionAndVarianceOptions{};
        reflectionAndVarianceOptions.planarReflectionsEnabled = true;
        reflectionAndVarianceOptions.varianceShadowsEnabled = true;
        (void)buildSharedGraph(
            sharedResources,
            reflectionAndVarianceOptions,
            sharedCallbacks);
        graph.Execute(commandContext);
        const auto reflectionAndVarianceSummary =
            graph.GetCompilationSummary();
        const auto reflectionAndVariancePasses =
            graph.GetPassDescriptions();
        const auto containsPass =
            [&](const std::string_view name)
        {
            return std::ranges::any_of(
                reflectionAndVariancePasses,
                [name](const auto& pass)
                {
                    return pass.name == name;
                });
        };
        Expect(
            sharedCallbackCount == 20
                && reflectionAndVarianceSummary.declaredPassCount == 21
                && containsPass("PlanarReflection")
                && containsPass("ShadowMoments")
                && containsPass("ShadowMomentsHorizontal")
                && containsPass("ShadowMomentsVertical"),
            "The shared frontend did not schedule planar and variance shadow passes.");

        graph.Reset();
        sharedCallbackCount = 0;
        Prism::RHI::BufferDescription
            gpuObjectDescription{};
        gpuObjectDescription.size = 96;
        gpuObjectDescription.stride = 48;
        gpuObjectDescription.usage =
            Prism::RHI::BufferUsage::Storage;
        gpuObjectDescription.memoryAccess =
            Prism::RHI::MemoryAccess::GpuOnly;
        Prism::RHI::BufferDescription
            sharedIndirectDescription{};
        sharedIndirectDescription.size = 40;
        sharedIndirectDescription.stride = 20;
        sharedIndirectDescription.usage =
            Prism::RHI::BufferUsage::Storage
            | Prism::RHI::BufferUsage::Indirect
            | Prism::RHI::BufferUsage::
                CopyDestination;
        sharedIndirectDescription.memoryAccess =
            Prism::RHI::MemoryAccess::GpuOnly;
        MockBuffer sharedGpuObjects(
            gpuObjectDescription);
        MockBuffer sharedIndirectArguments(
            sharedIndirectDescription);
        MockBuffer sharedIndirectDrawCounts(
            sharedIndirectDescription);
        Prism::Renderer::SharedRenderGraphOptions
            gpuDrivenOptions{};
        gpuDrivenOptions.gpuDrivenEnabled = true;
        graph.GetBlackboard().Publish<
            Prism::Renderer::GpuDrivenVisibilityFeatureSlot>(
                Prism::Renderer::GpuDrivenVisibilityGraphContribution{
                    &sharedGpuObjects,
                    &sharedIndirectArguments,
                    &sharedIndirectDrawCounts,
                    Prism::RHI::ResourceState::UnorderedAccess,
                    Prism::RHI::ResourceState::UnorderedAccess,
                    Prism::RHI::ResourceState::UnorderedAccess,
                    sharedCallback},
                {"test.gpu-visibility",
                 Prism::Renderer::RenderGraphBlackboardValueScope::ViewLocal,
                 0u,
                 graph.GetResourceGeneration(),
                 1u});
        const auto gpuDrivenHandles =
            buildSharedGraph(
                sharedResources,
                gpuDrivenOptions,
                sharedCallbacks);
        graph.Execute(commandContext);
        const auto gpuDrivenSummary =
            graph.GetCompilationSummary();
        const auto gpuDrivenPasses =
            graph.GetPassDescriptions();
        Expect(
            sharedCallbackCount == 17
                && gpuDrivenSummary
                       .declaredPassCount == 18
                && gpuDrivenSummary
                       .activePassCount == 18
                && gpuDrivenSummary
                       .parameterPassCount == 18
                && gpuDrivenSummary
                       .registeredBufferCount == 10
                && gpuDrivenSummary
                       .versionedResourceCount == 22
                && gpuDrivenPasses[1].name
                    == "GpuVisibility"
                && gpuDrivenHandles
                       .indirectArguments.version
                    == 1
                && std::ranges::any_of(
                    commandContext
                        .bufferBarrierHistory,
                    [](const Prism::RHI::
                           BufferBarrier& barrier)
                    {
                        return barrier.before
                                   == Prism::RHI::
                                       ResourceState::
                                           UnorderedAccess
                            && barrier.after
                                   == Prism::RHI::
                                       ResourceState::
                                           IndirectArgument;
                    }),
            "The shared frontend did not schedule GPU visibility and its indirect-argument barrier.");

        graph.Reset();
        executionOrder.clear();
        commandContext.nativeQueueSwitching = true;
        commandContext.activeQueue =
            Prism::RHI::CommandQueueType::Graphics;
        commandContext.queueSwitches.clear();
        graph.ImportResource("Input");
        graph.AddContextPass(
            "UsedProducer",
            {"Input"},
            {"Used"},
            [&](Prism::RHI::ICommandContext&)
            {
                executionOrder.emplace_back(
                    "UsedProducer");
            });
        graph.AddContextPass(
            "DeadProducer",
            {"Input"},
            {"Dead"},
            [&](Prism::RHI::ICommandContext&)
            {
                executionOrder.emplace_back(
                    "DeadProducer");
            });
        graph.AddContextPass(
            "FinalOutput",
            {"Used"},
            {"Output"},
            [&](Prism::RHI::ICommandContext&)
            {
                executionOrder.emplace_back(
                    "FinalOutput");
            });
        graph.MarkOutput("Output");
        graph.SetQueueExecutionMode(
            Prism::Renderer::RenderGraph::
                QueueExecutionMode::Native);
        graph.Execute(commandContext);
        Expect(
            executionOrder.size() == 2
                && executionOrder[0] == "UsedProducer"
                && executionOrder[1] == "FinalOutput",
            "RenderGraph Pass Culling did not remove an unreachable pass.");
        const auto cullingSummary =
            graph.GetCompilationSummary();
        Expect(
            cullingSummary.passCullingEnabled
                && cullingSummary.declaredPassCount == 3
                && cullingSummary.activePassCount == 2
                && cullingSummary.culledPassCount == 1,
            "RenderGraph Pass Culling summary is invalid.");
        const auto culledPasses =
            graph.GetPassDescriptions();
        Expect(
            culledPasses[1].culled
                && culledPasses[1].cullReason
                    == "not_reachable_from_output"
                && culledPasses[2].dependencies.size()
                    == 1
                && culledPasses[2].dependencies[0] == 0,
            "RenderGraph did not expose culling and dependency diagnostics.");

        const std::string cachedCullingSignature =
            graph.GetGraphSignature();
        graph.Reset();
        graph.ImportResource("Input");
        graph.AddContextPass(
            "UsedProducer",
            {"Input"},
            {"Used"},
            [](Prism::RHI::ICommandContext&) {});
        graph.AddContextPass(
            "DeadProducer",
            {"Input"},
            {"Dead"},
            [](Prism::RHI::ICommandContext&) {});
        graph.AddContextPass(
            "FinalOutput",
            {"Used"},
            {"Output"},
            [](Prism::RHI::ICommandContext&) {});
        graph.MarkOutput("Output");
        graph.SetQueueExecutionMode(
            Prism::Renderer::RenderGraph::
                QueueExecutionMode::Native);
        graph.Execute(commandContext);
        Expect(
            graph.GetCompilationSummary()
                    .compilationCacheHit
                && graph.GetGraphSignature()
                    == cachedCullingSignature
                && graph.GetCompilationSummary()
                       .activePassCount == 2
                && executionOrder.size() == 2,
            "RenderGraph did not reuse an identical callback-free compiled topology.");

        {
            Prism::Renderer::RenderGraph cacheBindingGraph;
            MockCommandContext cacheContext;
            MockTexture firstTexture;
            MockTexture currentTexture;
            std::size_t retiredCallbackCount = 0;
            std::size_t currentCallbackCount = 0;
            const auto addWritePass = [](
                Prism::Renderer::RenderGraph& targetGraph,
                auto&& callback)
            {
                targetGraph.AddResourceContextPass(
                    "CacheWrite",
                    {},
                    {{"CacheTexture",
                      Prism::RHI::ResourceState::RenderTarget}},
                    std::forward<decltype(callback)>(callback),
                    Prism::Renderer::RenderGraph::PassOptions{
                        Prism::Renderer::RenderGraph::QueueClass::Graphics,
                        true,
                        false});
            };

            (void)cacheBindingGraph.DeclareTexture(
                "CacheTexture",
                firstTexture,
                Prism::RHI::ResourceState::ShaderResource);
            addWritePass(
                cacheBindingGraph,
                [&](Prism::RHI::ICommandContext&)
                {
                    ++retiredCallbackCount;
                });
            cacheBindingGraph.Execute(cacheContext);
            Expect(
                retiredCallbackCount == 1
                    && !cacheBindingGraph
                            .GetCompilationSummary()
                            .compilationCacheHit,
                "The initial cache binding graph did not execute normally.");

            cacheBindingGraph.Reset();
            Expect(
                !cacheBindingGraph.GetCompilationSummary().compiled
                    && cacheBindingGraph.GetPassInfos().empty()
                    && cacheBindingGraph.GetGraphSignature().empty()
                    && cacheBindingGraph
                           .GetResourceDescriptions()
                           .empty(),
                "RenderGraph reset exposed stale compiled or execution diagnostics.");

            cacheContext.barriers.clear();
            cacheContext.textureBarriers = 0;
            const auto currentHandle =
                cacheBindingGraph.DeclareTexture(
                    "CacheTexture",
                    currentTexture,
                    Prism::RHI::ResourceState::ShaderResource);
            addWritePass(
                cacheBindingGraph,
                [&](Prism::RHI::ICommandContext&)
                {
                    ++currentCallbackCount;
                });
            cacheBindingGraph.Execute(cacheContext);
            const auto cacheReport =
                Prism::Renderer::BuildRenderGraphReport(
                    cacheBindingGraph,
                    Prism::RHI::GraphicsApi::Vulkan);
            Expect(
                cacheBindingGraph.GetCompilationSummary()
                        .compilationCacheHit
                    && retiredCallbackCount == 1
                    && currentCallbackCount == 1
                    && &cacheBindingGraph.ResolveTexture(
                           currentHandle)
                        == &currentTexture
                    && cacheContext.barriers.size() == 1
                    && cacheContext.barriers[0].texture
                        == &currentTexture
                    && cacheContext.barriers[0].before
                        == Prism::RHI::ResourceState::ShaderResource
                    && cacheContext.barriers[0].after
                        == Prism::RHI::ResourceState::RenderTarget
                    && cacheReport.at("compilation")
                           .at("compilationCacheHit")
                        == true
                    && cacheReport.at("resources")
                           .at(0)
                           .at("stateBits")
                        == static_cast<std::uint32_t>(
                            Prism::RHI::ResourceState::RenderTarget),
                "A compilation cache hit reused stale resources, initial state, callbacks, or diagnostics.");

            MockTexture changedStateTexture;
            cacheBindingGraph.Reset();
            (void)cacheBindingGraph.DeclareTexture(
                "CacheTexture",
                changedStateTexture,
                Prism::RHI::ResourceState::Undefined);
            addWritePass(
                cacheBindingGraph,
                [](Prism::RHI::ICommandContext&) {});
            cacheBindingGraph.Compile();
            Expect(
                !cacheBindingGraph.GetCompilationSummary()
                     .compilationCacheHit,
                "RenderGraph reused a cache plan after the initial resource state changed.");

            Prism::RHI::TextureDescription
                changedDescription =
                    changedStateTexture.GetDescription();
            changedDescription.width *= 2;
            MockTexture changedDescriptionTexture(
                changedDescription);
            cacheBindingGraph.Reset();
            (void)cacheBindingGraph.DeclareTexture(
                "CacheTexture",
                changedDescriptionTexture,
                Prism::RHI::ResourceState::Undefined);
            addWritePass(
                cacheBindingGraph,
                [](Prism::RHI::ICommandContext&) {});
            cacheBindingGraph.Compile();
            Expect(
                !cacheBindingGraph.GetCompilationSummary()
                     .compilationCacheHit,
                "RenderGraph reused a cache plan after a resource description changed.");
        }

        graph.Reset();
        graph.ImportResource("Input");
        graph.AddContextPass(
            "ChangedTopology",
            {"Input"},
            {"Output"},
            [](Prism::RHI::ICommandContext&) {});
        graph.MarkOutput("Output");
        graph.SetQueueExecutionMode(
            Prism::Renderer::RenderGraph::
                QueueExecutionMode::Native);
        graph.Compile();
        Expect(
            !graph.GetCompilationSummary()
                 .compilationCacheHit,
            "RenderGraph reused a cache entry after its topology changed.");

        graph.Reset();
        executionOrder.clear();
        graph.ImportResource("Input");
        graph.AddContextPass(
            "SideEffectInput",
            {"Input"},
            {"Prepared"},
            [&](Prism::RHI::ICommandContext&)
            {
                executionOrder.emplace_back(
                    "SideEffectInput");
            });
        graph.AddContextPass(
            "Readback",
            {"Prepared"},
            {},
            [&](Prism::RHI::ICommandContext&)
            {
                executionOrder.emplace_back(
                    "Readback");
            },
            Prism::Renderer::RenderGraph::PassOptions{
                Prism::Renderer::RenderGraph::QueueClass::Graphics,
                true,
                false});
        graph.Execute(commandContext);
        Expect(
            executionOrder.size() == 2
                && graph.GetCompilationSummary()
                       .activePassCount == 2,
            "A side-effect Pass did not keep its producer alive.");

        Prism::RHI::TextureDescription transientDescription{
            Prism::RHI::TextureDimension::Texture2D,
            256,
            256,
            1,
            1,
            1,
            Prism::RHI::Format::Rgba16Float,
            Prism::RHI::TextureUsage::RenderTarget
                | Prism::RHI::TextureUsage::ShaderResource,
            Prism::RHI::MemoryAccess::GpuOnly};
        graph.Reset();
        graph.DeclareTransientTexture(
            "TransientA",
            transientDescription);
        graph.DeclareTransientTexture(
            "TransientB",
            transientDescription);
        graph.AddContextPass(
            "WriteA",
            {},
            {"TransientA"},
            [](Prism::RHI::ICommandContext&) {});
        graph.AddContextPass(
            "ConsumeA",
            {"TransientA"},
            {},
            [](Prism::RHI::ICommandContext&) {},
            Prism::Renderer::RenderGraph::PassOptions{
                Prism::Renderer::RenderGraph::QueueClass::Graphics,
                true,
                false});
        graph.AddContextPass(
            "WriteB",
            {},
            {"TransientB"},
            [](Prism::RHI::ICommandContext&) {});
        graph.AddContextPass(
            "ConsumeB",
            {"TransientB"},
            {"Output"},
            [](Prism::RHI::ICommandContext&) {});
        graph.MarkOutput("Output");
        graph.Compile();
        const auto aliasingSummary =
            graph.GetCompilationSummary();
        Expect(
            aliasingSummary.transientResourceCount == 2
                && aliasingSummary.transientAllocationCount
                    == 1
                && aliasingSummary.transientLogicalBytes
                    > aliasingSummary.transientPhysicalBytes
                && aliasingSummary.transientAliasedBytes
                    == aliasingSummary.transientPhysicalBytes,
            "RenderGraph did not reuse a compatible non-overlapping transient allocation.");
        const auto transientResources =
            graph.GetResourceDescriptions();
        const auto transientA = std::ranges::find(
            transientResources,
            std::string("TransientA"),
            &Prism::Renderer::RenderGraph::ResourceDescription::name);
        const auto transientB = std::ranges::find(
            transientResources,
            std::string("TransientB"),
            &Prism::Renderer::RenderGraph::ResourceDescription::name);
        Expect(
            transientA != transientResources.end()
                && transientB != transientResources.end()
                && transientA->lastUse
                    < transientB->firstUse
                && transientA->physicalAllocation
                    == transientB->physicalAllocation,
            "RenderGraph transient lifetimes or alias slots are invalid.");

        MockTexture nativeTransientA(
            transientDescription,
            {17, 0, 524288, 524288, 524288});
        MockTexture nativeTransientB(
            transientDescription,
            {17, 0, 524288, 524288, 524288});
        graph.Reset();
        graph.DeclareTransientTexture(
            "NativeA",
            nativeTransientA,
            Prism::RHI::ResourceState::Undefined);
        graph.DeclareTransientTexture(
            "NativeB",
            nativeTransientB,
            Prism::RHI::ResourceState::Undefined);
        graph.AddResourceContextPass(
            "WriteNativeA",
            {},
            {{"NativeA",
              Prism::RHI::ResourceState::RenderTarget}},
            [](Prism::RHI::ICommandContext&) {});
        graph.AddResourceContextPass(
            "ConsumeNativeA",
            {{"NativeA",
              Prism::RHI::ResourceState::ShaderResource}},
            {},
            [](Prism::RHI::ICommandContext&) {},
            Prism::Renderer::RenderGraph::PassOptions{
                Prism::Renderer::RenderGraph::QueueClass::Graphics,
                true,
                false});
        graph.AddResourceContextPass(
            "WriteNativeB",
            {},
            {{"NativeB",
              Prism::RHI::ResourceState::RenderTarget}},
            [](Prism::RHI::ICommandContext&) {});
        graph.AddResourceContextPass(
            "ConsumeNativeB",
            {{"NativeB",
              Prism::RHI::ResourceState::ShaderResource}},
            {{"NativeOutput",
              Prism::RHI::ResourceState::Undefined}},
            [](Prism::RHI::ICommandContext&) {});
        graph.MarkOutput("NativeOutput");
        const std::uint32_t aliasingBarrierBase =
            commandContext.aliasingBarriers;
        graph.Execute(commandContext);
        const auto nativeAliasingSummary =
            graph.GetCompilationSummary();
        Expect(
            nativeAliasingSummary
                .nativeTransientAliasingApplied
                && nativeAliasingSummary
                       .nativeTransientAliasingPlanValid
                && nativeAliasingSummary
                       .nativeTransientAllocationCount == 1
                && nativeAliasingSummary
                       .nativeTransientLogicalBytes == 1048576
                && nativeAliasingSummary
                       .nativeTransientPhysicalBytes == 524288
                && nativeAliasingSummary
                       .expectedAliasingBarrierCount == 2
                && nativeAliasingSummary
                       .executedAliasingBarrierCount == 2
                && commandContext.aliasingBarriers
                    == aliasingBarrierBase + 2,
            "RenderGraph did not apply the native transient aliasing plan.");

        MockBuffer nativeTransientBufferA(
            transientBufferDescription,
            {23, 0, 4096, 4096, 4096});
        MockBuffer nativeTransientBufferB(
            transientBufferDescription,
            {23, 0, 4096, 4096, 4096});
        graph.Reset();
        graph.DeclareTransientBuffer(
            "NativeBufferA",
            nativeTransientBufferA,
            Prism::RHI::ResourceState::Undefined);
        graph.DeclareTransientBuffer(
            "NativeBufferB",
            nativeTransientBufferB,
            Prism::RHI::ResourceState::Undefined);
        graph.AddResourceContextPass(
            "WriteNativeBufferA",
            {},
            {{"NativeBufferA",
              Prism::RHI::ResourceState::UnorderedAccess}},
            [](Prism::RHI::ICommandContext&) {});
        graph.AddResourceContextPass(
            "ConsumeNativeBufferA",
            {{"NativeBufferA",
              Prism::RHI::ResourceState::ShaderResource}},
            {},
            [](Prism::RHI::ICommandContext&) {},
            Prism::Renderer::RenderGraph::PassOptions{
                Prism::Renderer::RenderGraph::QueueClass::Graphics,
                true,
                false});
        graph.AddResourceContextPass(
            "WriteNativeBufferB",
            {},
            {{"NativeBufferB",
              Prism::RHI::ResourceState::UnorderedAccess}},
            [](Prism::RHI::ICommandContext&) {});
        graph.AddResourceContextPass(
            "ConsumeNativeBufferB",
            {{"NativeBufferB",
              Prism::RHI::ResourceState::ShaderResource}},
            {{"NativeBufferOutput",
              Prism::RHI::ResourceState::Undefined}},
            [](Prism::RHI::ICommandContext&) {});
        graph.MarkOutput("NativeBufferOutput");
        const std::uint32_t bufferAliasingBarrierBase =
            commandContext.bufferAliasingBarriers;
        graph.Execute(commandContext);
        const auto nativeBufferAliasingSummary =
            graph.GetCompilationSummary();
        Expect(
            nativeBufferAliasingSummary
                    .nativeTransientAliasingApplied
                && nativeBufferAliasingSummary
                       .nativeTransientAliasingPlanValid
                && nativeBufferAliasingSummary
                       .nativeTransientAllocationCount == 1
                && nativeBufferAliasingSummary
                       .nativeTransientLogicalBytes == 8192
                && nativeBufferAliasingSummary
                       .nativeTransientPhysicalBytes == 4096
                && nativeBufferAliasingSummary
                       .expectedAliasingBarrierCount == 2
                && nativeBufferAliasingSummary
                       .executedAliasingBarrierCount == 2
                && commandContext.bufferAliasingBarriers
                    == bufferAliasingBarrierBase + 2,
            "RenderGraph did not apply native transient buffer aliasing.");

        graph.Reset();
        executionOrder.clear();
        graph.ImportResource("Input");
        graph.AddContextPass(
            "GraphicsProduce",
            {"Input"},
            {"GraphicsData"},
            [&](Prism::RHI::ICommandContext&)
            {
                Expect(
                    commandContext.activeQueue
                        == Prism::RHI::CommandQueueType::Graphics,
                    "The graphics producer ran on the wrong command queue.");
                executionOrder.emplace_back(
                    "GraphicsProduce");
            });
        graph.AddContextPass(
            "ComputeProcess",
            {"GraphicsData"},
            {"ComputeData"},
            [&](Prism::RHI::ICommandContext&)
            {
                Expect(
                    commandContext.activeQueue
                        == Prism::RHI::CommandQueueType::Compute,
                    "The compute pass ran on the wrong command queue.");
                executionOrder.emplace_back(
                    "ComputeProcess");
            },
            Prism::Renderer::RenderGraph::PassOptions{
                Prism::Renderer::RenderGraph::QueueClass::Compute,
                false,
                true});
        graph.AddContextPass(
            "GraphicsConsume",
            {"ComputeData"},
            {"Output"},
            [&](Prism::RHI::ICommandContext&)
            {
                Expect(
                    commandContext.activeQueue
                        == Prism::RHI::CommandQueueType::Graphics,
                    "The graphics consumer ran on the wrong command queue.");
                executionOrder.emplace_back(
                    "GraphicsConsume");
            });
        graph.MarkOutput("Output");
        graph.Execute(commandContext);
        const auto queueSummary =
            graph.GetCompilationSummary();
        const auto& queueSync =
            graph.GetQueueSyncDescriptions();
        Expect(
            executionOrder.size() == 3
                && !queueSummary.serialQueueExecution
                && queueSummary
                       .nativeMultiQueueSubmissionApplied
                && queueSummary.nativeQueueSwitchCount == 2
                && queueSummary.nativeQueueSegmentCount == 3
                && queueSummary.crossQueueSyncCount == 2
                && commandContext.queueSwitches.size() == 2
                && queueSync.size() == 2
                && queueSync[0].resources[0]
                    == "GraphicsData"
                && queueSync[1].resources[0]
                    == "ComputeData",
            "RenderGraph did not execute the expected native Graphics/Compute schedule.");
        Prism::RHI::GraphicsDeviceCapabilities
            reportCapabilities{};
        reportCapabilities.graphicsApi =
            Prism::RHI::GraphicsApi::Vulkan;
        reportCapabilities.adapterName =
            "Test Adapter";
        reportCapabilities.limits
            .maxTextureDimension2D = 16384;
        reportCapabilities.features
            .descriptorIndexing = true;
        Prism::RHI::DescriptorAllocatorStatistics
            reportDescriptorStatistics{};
        reportDescriptorStatistics.poolCount = 2;
        reportDescriptorStatistics.setCapacity = 768;
        reportDescriptorStatistics
            .allocatedSetCount = 12;
        Prism::RHI::UploadQueueStatistics
            reportUploadStatistics{};
        reportUploadStatistics.uploadedBytes = 4096;
        reportUploadStatistics.submittedBatchCount = 3;
        reportUploadStatistics.lastSubmittedTicket = 7;
        reportUploadStatistics.completedTicket = 6;
        reportUploadStatistics.outstandingBatchCount = 1;
        Prism::RHI::ResourceRetirementStatistics
            reportRetirementStatistics{};
        reportRetirementStatistics
            .totalRetiredObjectCount = 9;
        reportRetirementStatistics
            .pendingObjectCount = 2;
        const nlohmann::json graphReport =
            Prism::Renderer::BuildRenderGraphReport(
                graph,
                Prism::RHI::GraphicsApi::Vulkan,
                &reportCapabilities,
                &reportDescriptorStatistics,
                &reportUploadStatistics,
                &reportRetirementStatistics);
        Expect(
            graphReport.value("format", std::string{})
                    == "PrismRenderGraphReport"
                && graphReport.value("version", 0u) == 11u
                && graphReport.at("compilation")
                       .at("resourceModel")
                       .contains(
                           "registeredTextureCount")
                && graphReport.at("compilation")
                       .at("queueExecutionMode")
                    == "native_multi_queue"
                && graphReport.at("compilation")
                       .at("requestedQueueExecutionMode")
                    == "native"
                && graphReport.at("compilation")
                       .at("crossQueueSyncCount")
                       .get<std::size_t>() == 2
                && graphReport.at("compilation")
                       .at("transient")
                       .contains("native")
                && graphReport.at("rhi")
                       .at("deviceCapabilities")
                       .at("adapterName")
                    == "Test Adapter"
                && graphReport.at("rhi")
                       .at("deviceCapabilities")
                       .at("features")
                       .at("descriptorIndexing")
                       .get<bool>()
                && graphReport.at("rhi")
                       .at("descriptorAllocator")
                       .at("setCapacity")
                       .get<std::uint32_t>()
                    == 768
                && graphReport.at("rhi")
                       .at("uploadQueue")
                       .at("uploadedBytes")
                       .get<std::uint64_t>()
                     == 4096
                && graphReport.at("rhi")
                       .at("uploadQueue")
                       .at("lastSubmittedTicket")
                       .get<std::uint64_t>()
                    == 7
                && graphReport.at("rhi")
                       .at("resourceRetirement")
                       .at("pendingObjectCount")
                       .get<std::uint32_t>()
                    == 2
                && graphReport.at("queueSync").size()
                    == 2,
            "RenderGraph v7 diagnostics lost legacy native queue execution data.");

        Prism::Renderer::RenderGraph serialGraph;
        std::vector<Prism::RHI::CommandQueueType>
            serialPassQueues;
        serialGraph.ImportResource("Input");
        serialGraph.AddContextPass(
            "GraphicsProduce",
            {"Input"},
            {"GraphicsData"},
            [&](Prism::RHI::ICommandContext& context)
            {
                serialPassQueues.push_back(
                    context.GetActiveCommandQueue());
            });
        serialGraph.AddContextPass(
            "ComputeProcess",
            {"GraphicsData"},
            {"ComputeData"},
            [&](Prism::RHI::ICommandContext& context)
            {
                serialPassQueues.push_back(
                    context.GetActiveCommandQueue());
            },
            Prism::Renderer::RenderGraph::PassOptions{
                Prism::Renderer::RenderGraph::QueueClass::Compute,
                false,
                true});
        serialGraph.AddContextPass(
            "GraphicsConsume",
            {"ComputeData"},
            {"Output"},
            [&](Prism::RHI::ICommandContext& context)
            {
                serialPassQueues.push_back(
                    context.GetActiveCommandQueue());
            });
        serialGraph.MarkOutput("Output");
        serialGraph.SetQueueExecutionMode(
            Prism::Renderer::RenderGraph::
                QueueExecutionMode::Serial);
        const std::size_t queueSwitchBase =
            commandContext.queueSwitches.size();
        serialGraph.Execute(commandContext);
        const auto serialSummary =
            serialGraph.GetCompilationSummary();
        Expect(
            serialPassQueues.size() == 3
                && std::ranges::all_of(
                    serialPassQueues,
                    [](const Prism::RHI::CommandQueueType queue)
                    {
                        return queue
                            == Prism::RHI::CommandQueueType::Graphics;
                    })
                && serialSummary.serialQueueExecution
                && !serialSummary
                        .nativeMultiQueueSubmissionApplied
                && !serialSummary.multiQueueGpuTimestamps
                && serialSummary.requestedQueueExecutionMode
                    == Prism::Renderer::RenderGraph::
                        QueueExecutionMode::Serial
                && commandContext.queueSwitches.size()
                    == queueSwitchBase,
            "RenderGraph serial A/B mode used a native compute queue.");

        Prism::Renderer::RenderGraph dagGraph;
        std::vector<std::string> dagExecutionOrder;
        bool expectNativeDagExecution = true;
        dagGraph.ImportResource("Input");
        dagGraph.AddContextPass(
            "RootProduce",
            {"Input"},
            {"GraphicsInput", "ComputeInput"},
            [&](Prism::RHI::ICommandContext& context)
            {
                Expect(
                    context.GetActiveCommandQueue()
                        == Prism::RHI::CommandQueueType::Graphics,
                    "The DAG root ran on the wrong command queue.");
            });
        dagGraph.AddContextPass(
            "GraphicsBranch",
            {"GraphicsInput"},
            {"GraphicsResult"},
            [&](Prism::RHI::ICommandContext& context)
            {
                Expect(
                    context.GetActiveCommandQueue()
                        == Prism::RHI::CommandQueueType::Graphics,
                    "The DAG graphics branch ran on the wrong command queue.");
                context.Draw(3, 1, 0, 0);
            },
            Prism::Renderer::RenderGraph::PassOptions{
                Prism::Renderer::RenderGraph::QueueClass::Graphics,
                false,
                true,
                true,
                Prism::Renderer::RenderGraph::ParallelRecordingContract::
                    AuditedIndependent()});
        dagGraph.AddContextPass(
            "ComputeBranch",
            {"ComputeInput"},
            {"ComputeResult"},
            [&](Prism::RHI::ICommandContext& context)
            {
                Expect(
                    context.GetActiveCommandQueue()
                        == (expectNativeDagExecution
                                ? Prism::RHI::CommandQueueType::Compute
                                : Prism::RHI::CommandQueueType::Graphics),
                    "The DAG compute branch ran on the wrong command queue.");
                context.Dispatch(1, 1, 1);
            },
            Prism::Renderer::RenderGraph::PassOptions{
                Prism::Renderer::RenderGraph::QueueClass::Compute,
                false,
                true,
                true,
                Prism::Renderer::RenderGraph::ParallelRecordingContract::
                    AuditedIndependent()});
        dagGraph.AddContextPass(
            "Join",
            {"GraphicsResult", "ComputeResult"},
            {"Output"},
            [&](Prism::RHI::ICommandContext& context)
            {
                Expect(
                    context.GetActiveCommandQueue()
                        == Prism::RHI::CommandQueueType::Graphics,
                    "The DAG join ran on the wrong command queue.");
            });
        dagGraph.MarkOutput("Output");
        dagGraph.SetQueueExecutionMode(
            Prism::Renderer::RenderGraph::
                QueueExecutionMode::Native);

        commandContext.independentBatchSubmission = true;
        commandContext.deferredBatchSubmission = true;
        commandContext.batchQueues.clear();
        commandContext.batchWaits.clear();
        commandContext.batchSignals.clear();
        commandContext.continuationWaits.clear();
        commandContext.graphicsResumed = false;
        commandContext.batchesFlushed = false;
        dagGraph.Execute(
            commandContext,
            [&](const std::string_view name)
            {
                dagExecutionOrder.emplace_back(name);
            });

        const auto dagSummary =
            dagGraph.GetCompilationSummary();
        const auto& dagBatches =
            dagGraph.GetQueueBatchDescriptions();
        const bool dagScheduleValid =
            dagExecutionOrder
                    == std::vector<std::string>{
                        "RootProduce",
                        "ComputeBranch",
                        "GraphicsBranch",
                        "Join"}
                && dagSummary
                       .independentQueueBatchSubmissionAvailable
                && dagSummary
                       .dagQueueBatchExecutionApplied
                && dagSummary
                       .nativeMultiQueueSubmissionApplied
                && dagSummary
                       .deferredQueueBatchSubmissionApplied
                && dagSummary
                       .parallelCommandRecordingApplied
                && dagSummary.parallelRecordedPassCount == 2
                && dagSummary.recordedCommandCount == 2
                && !dagSummary.serialQueueExecution
                && dagSummary.queueBatchCount == 4
                && dagSummary
                       .crossQueueBatchDependencyCount == 2
                && dagSummary.overlapOpportunityCount >= 1
                && dagBatches.size() == 4
                && commandContext.batchQueues
                    == std::vector<
                        Prism::RHI::CommandQueueType>{
                        Prism::RHI::CommandQueueType::Graphics,
                        Prism::RHI::CommandQueueType::Compute,
                        Prism::RHI::CommandQueueType::Graphics,
                        Prism::RHI::CommandQueueType::Graphics}
                && commandContext.batchWaits.size() == 4
                && commandContext.batchWaits[1].size() == 1
                && commandContext.batchWaits[1][0].queue
                    == Prism::RHI::CommandQueueType::Graphics
                && commandContext.batchWaits[3].size() == 1
                && commandContext.batchWaits[3][0].queue
                    == Prism::RHI::CommandQueueType::Compute
                && commandContext.batchSignals.size() == 4
                && commandContext.batchesFlushed
                && commandContext.graphicsResumed
                && commandContext.continuationWaits.size() == 2;
        if (!dagScheduleValid)
        {
            std::cerr
                << "DAG schedule diagnostics: order=";
            for (const std::string& passName :
                 dagExecutionOrder)
            {
                std::cerr << passName << ',';
            }
            std::cerr
                << " independent="
                << dagSummary
                       .independentQueueBatchSubmissionAvailable
                << " dag="
                << dagSummary
                       .dagQueueBatchExecutionApplied
                << " native="
                << dagSummary
                       .nativeMultiQueueSubmissionApplied
                << " deferred="
                << dagSummary
                       .deferredQueueBatchSubmissionApplied
                << " parallel="
                << dagSummary
                       .parallelCommandRecordingApplied
                << " parallelPasses="
                << dagSummary.parallelRecordedPassCount
                << " commands="
                << dagSummary.recordedCommandCount
                << " serial="
                << dagSummary.serialQueueExecution
                << " batches="
                << dagSummary.queueBatchCount
                << " crossBatchDeps="
                << dagSummary
                       .crossQueueBatchDependencyCount
                << " overlap="
                << dagSummary.overlapOpportunityCount
                << " describedBatches="
                << dagBatches.size()
                << " submittedBatches="
                << commandContext.batchQueues.size()
                << " waits=";
            for (const auto& waits :
                 commandContext.batchWaits)
            {
                std::cerr << waits.size() << ',';
            }
            std::cerr
                << " signals="
                << commandContext.batchSignals.size()
                << " flushed="
                << commandContext.batchesFlushed
                << " resumed="
                << commandContext.graphicsResumed
                << " continuationWaits="
                << commandContext.continuationWaits.size()
                << '\n';
        }
        Expect(
            dagScheduleValid,
            "RenderGraph did not execute the independent DAG queue-batch schedule.");
        const nlohmann::json dagReport =
            Prism::Renderer::BuildRenderGraphReport(
                dagGraph,
                Prism::RHI::GraphicsApi::Vulkan);
        Expect(
            dagReport.value("version", 0u) == 11u
                && dagReport.at("compilation")
                       .at("queueExecutionMode")
                    == "dag_multi_queue"
                && dagReport.at("compilation")
                       .at("queueInfrastructure")
                       .at("independentBatchSubmission")
                       .get<bool>()
                && dagReport.at("compilation")
                       .at("queueInfrastructure")
                       .at("dagQueueBatchExecutionApplied")
                       .get<bool>()
                && dagReport.at("compilation")
                       .at("queueInfrastructure")
                       .at("parallelCommandRecordingApplied")
                       .get<bool>()
                && dagReport.at("compilation")
                       .at("queueInfrastructure")
                       .at("deferredBatchSubmissionApplied")
                       .get<bool>()
                && dagReport.at("compilation")
                       .at("queueBatchPlan")
                       .at("batchCount")
                       .get<std::size_t>() == 4
                && dagReport.at("compilation")
                       .at("queueBatchPlan")
                       .at("overlapOpportunityCount")
                       .get<std::size_t>() >= 1
                && dagReport.at("queueBatches").size() == 4,
            "RenderGraph v7 diagnostics lost DAG queue-batch data.");

        commandContext.nativeParallelRecording = true;
        commandContext.appendedNativeRecordings = 0;
        commandContext.batchQueues.clear();
        commandContext.batchWaits.clear();
        commandContext.batchSignals.clear();
        commandContext.continuationWaits.clear();
        commandContext.graphicsResumed = false;
        commandContext.batchesFlushed = false;
        dagExecutionOrder.clear();
        dagGraph.Execute(
            commandContext,
            [&](const std::string_view name)
            {
                dagExecutionOrder.emplace_back(name);
            });
        const auto nativeRecordingSummary =
            dagGraph.GetCompilationSummary();
        Expect(
            nativeRecordingSummary
                .nativeParallelCommandRecordingApplied
                && nativeRecordingSummary
                       .nativeParallelRecordedPassCount == 2
                && nativeRecordingSummary
                       .parallelRecordedPassCount == 2
                && nativeRecordingSummary
                       .recordedCommandCount == 0
                && commandContext
                       .appendedNativeRecordings == 2,
            "RenderGraph did not use the native parallel recording contract.");
        commandContext.nativeParallelRecording = false;

        dagGraph.SetQueueExecutionMode(
            Prism::Renderer::RenderGraph::
                QueueExecutionMode::Automatic);
        expectNativeDagExecution = false;
        dagExecutionOrder.clear();
        commandContext.batchQueues.clear();
        commandContext.batchWaits.clear();
        commandContext.batchSignals.clear();
        commandContext.continuationWaits.clear();
        commandContext.graphicsResumed = false;
        dagGraph.Execute(
            commandContext,
            [&](const std::string_view name)
            {
                dagExecutionOrder.emplace_back(name);
            });
        const auto automaticSummary =
            dagGraph.GetCompilationSummary();
        Expect(
            dagExecutionOrder
                    == std::vector<std::string>{
                        "RootProduce",
                        "GraphicsBranch",
                        "ComputeBranch",
                        "Join"}
                && automaticSummary.serialQueueExecution
                && !automaticSummary
                        .dagQueueBatchExecutionApplied
                && !automaticSummary
                        .nativeMultiQueueSubmissionApplied
                && automaticSummary
                       .requestedQueueExecutionMode
                    == Prism::Renderer::RenderGraph::
                        QueueExecutionMode::Automatic
                && commandContext.batchQueues.empty()
                && !commandContext.graphicsResumed,
            "RenderGraph automatic mode did not preserve the safe serial policy.");

        Prism::Renderer::QueueSchedulingDecision
            profitableDecision{};
        profitableDecision.historyAvailable = true;
        profitableDecision.selectNative = true;
        profitableDecision.reason =
            "estimated_positive_benefit";
        dagGraph.SetAutomaticQueueDecision(
            profitableDecision);
        expectNativeDagExecution = true;
        dagExecutionOrder.clear();
        commandContext.batchQueues.clear();
        commandContext.batchWaits.clear();
        commandContext.batchSignals.clear();
        commandContext.continuationWaits.clear();
        commandContext.graphicsResumed = false;
        commandContext.batchesFlushed = false;
        dagGraph.Execute(
            commandContext,
            [&](const std::string_view name)
            {
                dagExecutionOrder.emplace_back(name);
            });
        const auto profitableAutoSummary =
            dagGraph.GetCompilationSummary();
        Expect(
            profitableAutoSummary
                .dagQueueBatchExecutionApplied
                && profitableAutoSummary
                       .nativeMultiQueueSubmissionApplied
                && profitableAutoSummary
                       .automaticQueueDecision.selectNative
                && commandContext.batchesFlushed,
            "RenderGraph automatic mode ignored a profitable historical decision.");

        const std::filesystem::path costModelPath =
            std::filesystem::current_path()
            / "render-graph-cost-model-test.json";
        std::error_code removeError;
        std::filesystem::remove(
            costModelPath,
            removeError);
        Prism::Renderer::QueueSchedulingCostModel
            costModel;
        costModel.Configure(
            {"vulkan", "Mock GPU", 1280, 720},
            costModelPath);
        const std::array<
            Prism::Renderer::QueueTimingSample,
            2> serialTimings{{
            {"GraphicsWork", 1.5f,
             Prism::RHI::CommandQueueType::Graphics,
             0.0, 1.5, true},
            {"Renderer", 2.0f,
             Prism::RHI::CommandQueueType::Graphics,
             0.0, 2.0, true}}};
        const std::array<
            Prism::Renderer::QueueTimingSample,
            3> nativeTimings{{
            {"GraphicsWork", 1.0f,
             Prism::RHI::CommandQueueType::Graphics,
             0.0, 1.0, true},
            {"ComputeWork", 0.6f,
             Prism::RHI::CommandQueueType::Compute,
             0.2, 0.8, true},
            {"Renderer", 1.3f,
             Prism::RHI::CommandQueueType::Graphics,
             0.0, 1.3, true}}};
        for (std::size_t sample = 0;
             sample < 2;
             ++sample)
        {
            costModel.Observe(
                "profitable-graph",
                false,
                serialTimings);
            costModel.Observe(
                "profitable-graph",
                true,
                nativeTimings);
        }
        const auto historicalDecision =
            costModel.Evaluate(
                "profitable-graph",
                1);
        Expect(
            historicalDecision.historyAvailable
                && historicalDecision
                       .exactHistoryAvailable
                && historicalDecision
                       .predictionAvailable
                && historicalDecision.selectNative
                && historicalDecision.serialSamples == 2
                && historicalDecision.nativeSamples == 2
                && std::abs(
                       historicalDecision
                           .serialP50Milliseconds
                       - 2.0) < 0.0001
                && std::abs(
                       historicalDecision
                           .serialP95Milliseconds
                       - 2.0) < 0.0001
                && std::abs(
                       historicalDecision
                           .nativeP50Milliseconds
                       - 1.3) < 0.0001
                && std::abs(
                       historicalDecision
                           .nativeP95Milliseconds
                       - 1.3) < 0.0001
                && historicalDecision
                       .estimatedNetBenefitMilliseconds
                    > historicalDecision
                          .requiredBenefitMilliseconds,
            "The historical queue cost model did not select a measured profitable native schedule.");
        Expect(
            !std::filesystem::exists(costModelPath),
            "The queue cost model performed persistence I/O on its real-time update path.");
        Expect(
            costModel.Save(),
            "The queue cost model could not explicitly persist its samples.");
        Prism::Renderer::QueueSchedulingCostModel
            reloadedCostModel;
        reloadedCostModel.Configure(
            {"vulkan", "Mock GPU", 1280, 720},
            costModelPath);
        const auto reloadedDecision =
            reloadedCostModel.Evaluate(
                "profitable-graph",
                1);
        Expect(
            reloadedDecision.historyAvailable
                && reloadedDecision.selectNative,
            "The historical queue cost model did not persist its samples.");

        const Prism::Renderer::
            QueueSchedulingGraphProfile predictedProfile{
                "predicted-graph",
                1,
                {
                    {"GraphicsWork",
                     Prism::RHI::CommandQueueType::Graphics,
                     {}},
                    {"ComputeWork",
                     Prism::RHI::CommandQueueType::Compute,
                     {}},
                    {"Join",
                     Prism::RHI::CommandQueueType::Graphics,
                     {0, 1}},
                }};
        const std::array<
            Prism::Renderer::QueueTimingSample,
            4> predictedSerialTimings{{
            {"GraphicsWork", 1.0f,
             Prism::RHI::CommandQueueType::Graphics,
             0.0, 1.0, true},
            {"ComputeWork", 0.6f,
             Prism::RHI::CommandQueueType::Graphics,
             1.0, 1.6, true},
            {"Join", 0.2f,
             Prism::RHI::CommandQueueType::Graphics,
             1.6, 1.8, true},
            {"Renderer", 2.0f,
             Prism::RHI::CommandQueueType::Graphics,
             0.0, 2.0, true}}};
        reloadedCostModel.Observe(
            predictedProfile,
            false,
            predictedSerialTimings);
        reloadedCostModel.Observe(
            predictedProfile,
            false,
            predictedSerialTimings);
        const auto predictedDecision =
            reloadedCostModel.Evaluate(
                predictedProfile);
        Expect(
            predictedDecision.predictionAvailable
                && predictedDecision
                       .predictedFromPassHistory
                && !predictedDecision
                        .exactHistoryAvailable
                && !predictedDecision.selectNative
                && predictedDecision.reason
                    == "prediction_requires_measurement"
                && predictedDecision
                       .estimatedOverlapMilliseconds
                    > 0.0,
            "The queue cost model promoted an unmeasured DAG prediction or failed to estimate its overlap.");

        bool controlledProbeObserved = false;
        for (std::size_t evaluation = 0;
             evaluation < 120;
             ++evaluation)
        {
            const auto exploration =
                reloadedCostModel.Evaluate(
                    predictedProfile);
            controlledProbeObserved =
                controlledProbeObserved
                || (exploration.selectNative
                    && exploration
                           .controlledExploration
                    && exploration.reason
                        == "controlled_native_probe");
        }
        Expect(
            controlledProbeObserved,
            "The queue cost model never scheduled its bounded native exploration sample.");
        Expect(
            reloadedCostModel.Save(),
            "The queue cost model could not persist its predicted graph history.");

        Prism::Renderer::QueueSchedulingCostModel
            concurrentWriter;
        concurrentWriter.Configure(
            {"vulkan", "Mock GPU", 1280, 720},
            costModelPath);
        concurrentWriter.Observe(
            predictedProfile,
            false,
            predictedSerialTimings);
        Expect(
            concurrentWriter.Save(),
            "The queue cost model writer could not flush its pending sample.");
        Prism::Renderer::QueueSchedulingCostModel
            mergedReader;
        mergedReader.Configure(
            {"vulkan", "Mock GPU", 1280, 720},
            costModelPath);
        const auto mergedDecision =
            mergedReader.Evaluate(
                predictedProfile);
        Expect(
            mergedDecision.serialSamples == 3,
            "The locked queue cost model persistence lost a writer's samples.");
        Expect(
            mergedReader.Save(),
            "The merged queue cost model could not flush its evaluation state.");

        const std::filesystem::path destructorSavePath =
            std::filesystem::current_path()
            / "render-graph-cost-model-destructor-test.json";
        std::filesystem::remove(
            destructorSavePath,
            removeError);
        {
            Prism::Renderer::QueueSchedulingCostModel
                destructorWriter;
            destructorWriter.Configure(
                {"vulkan", "Destructor GPU", 1280, 720},
                destructorSavePath);
            destructorWriter.Observe(
                "destructor-graph",
                false,
                serialTimings);
            Expect(
                !std::filesystem::exists(
                    destructorSavePath),
                "The queue cost model wrote before its throttled or lifetime persistence boundary.");
        }
        Expect(
            std::filesystem::exists(destructorSavePath),
            "The queue cost model destructor did not flush pending history.");
        {
            Prism::Renderer::QueueSchedulingCostModel
                destructorReader;
            destructorReader.Configure(
                {"vulkan", "Destructor GPU", 1280, 720},
                destructorSavePath);
            const auto destructorDecision =
                destructorReader.Evaluate(
                    "destructor-graph",
                    1);
            Expect(
                destructorDecision.serialSamples == 1,
                "The queue cost model could not reload destructor-persisted history.");
        }
        std::filesystem::remove(
            destructorSavePath,
            removeError);

        Prism::Renderer::QueueSchedulingCostModel
            memoryOnlyCostModel;
        memoryOnlyCostModel.Configure(
            {"vulkan", "Memory GPU", 1280, 720},
            {});
        for (std::size_t sample = 0;
             sample < 2;
             ++sample)
        {
            memoryOnlyCostModel.Observe(
                "memory-only-graph",
                false,
                serialTimings);
            memoryOnlyCostModel.Observe(
                "memory-only-graph",
                true,
                nativeTimings);
        }
        const auto memoryOnlyDecision =
            memoryOnlyCostModel.Evaluate(
                "memory-only-graph",
                1);
        Expect(
            memoryOnlyDecision.exactHistoryAvailable
                && memoryOnlyDecision.serialSamples == 2
                && memoryOnlyDecision.nativeSamples == 2,
            "The queue cost model discarded in-memory history when persistence was disabled.");
        std::filesystem::remove(
            costModelPath,
            removeError);
        commandContext.independentBatchSubmission = false;
        commandContext.deferredBatchSubmission = false;
        commandContext.nativeQueueSwitching = false;

        {
            using Graph = Prism::Renderer::RenderGraph;
            using State = Prism::RHI::ResourceState;
            using Queue = Prism::RHI::CommandQueueType;
            MockCommandContext context;
            context.nativeQueueSwitching = true;
            context.independentBatchSubmission = true;
            context.deferredBatchSubmission = true;
            MockTexture imported;
            MockTexture shared;
            MockTexture reusedBloom;
            Graph handoff;
            (void)handoff.ImportTexture("Imported", imported, State::ShaderResource);
            (void)handoff.ImportTexture("Shared", shared, State::ShaderResource);
            (void)handoff.DeclareTransientTexture("Bloom", reusedBloom, State::ShaderResource);
            handoff.AddResourceContextPass("GraphicsRoot", {}, {{"Shared", State::RenderTarget}},
                [](Prism::RHI::ICommandContext&) {});
            handoff.AddResourceContextPass("ComputeRead", {{"Shared", State::ShaderResource},
                {"Imported", State::ShaderResource}}, {{"Token", State::Undefined}, {"Bloom", State::UnorderedAccess}},
                [](Prism::RHI::ICommandContext&) {}, {Graph::QueueClass::Compute});
            handoff.AddContextPass("ComputeBridge", {"Token"}, {"Bridge"},
                [](Prism::RHI::ICommandContext&) {}, {Graph::QueueClass::Compute});
            handoff.AddResourceContextPass("GraphicsJoin", {{"Shared", State::ShaderResource},
                {"Bridge", State::Undefined}}, {{"Output", State::Undefined}},
                [](Prism::RHI::ICommandContext&) {});
            handoff.MarkOutput("Output");
            handoff.SetQueueExecutionMode(Graph::QueueExecutionMode::Native);
            handoff.Execute(context);
            const auto hasBarrier = [&](const Prism::RHI::ITexture* texture, State before, State after, Queue queue)
            {
                for (std::size_t i = 0; i < context.barriers.size(); ++i)
                    if (context.barriers[i].texture == texture && context.barriers[i].before == before
                        && context.barriers[i].after == after && context.barrierQueues[i] == queue) return true;
                return false;
            };
            Expect(hasBarrier(&imported, State::ShaderResource, State::Common, Queue::Graphics),
                "Compute-first imports were not released on the graphics prologue.");
            Expect(hasBarrier(&reusedBloom, State::ShaderResource, State::Common, Queue::Graphics),
                "A reused transient did not release its previous frame's graphics state.");
            Expect(hasBarrier(&shared, State::ShaderResource, State::Common, Queue::Compute)
                && hasBarrier(&shared, State::Common, State::ShaderResource, Queue::Graphics),
                "Transitive dependency reduction lost a read/read ownership transfer.");
            Expect(context.batchWaits.size() >= 2 && !context.batchWaits[1].empty(),
                "An imported compute resource did not wait for its graphics release.");
        }

        MockGraphicsPipeline pipeline;
        MockBuffer vertexBuffer;
        MockBuffer indexBuffer;
        MockDescriptorSet descriptorSet;
        Prism::Renderer::IndexedGeometryDraw geometryDraw{};
        geometryDraw.descriptorSet = &descriptorSet;
        geometryDraw.vertexBuffer = &vertexBuffer;
        geometryDraw.indexBuffer = &indexBuffer;
        geometryDraw.indexFormat =
            Prism::RHI::IndexFormat::UInt16;
        geometryDraw.indexCount = 36;
        geometryDraw.dynamicBufferOffsets.push_back({1, 256});
        Prism::Renderer::RhiGeometryPass geometryPass;
        geometryPass.Execute(commandContext, pipeline, std::span(&geometryDraw, 1));
        Expect(commandContext.graphicsPipelineBinds == 1, "Shared geometry pass did not bind its pipeline.");
        Expect(commandContext.descriptorSetBinds == 1 && commandContext.vertexBufferBinds == 1
                   && commandContext.indexBufferBinds == 1 && commandContext.indexedDraws == 1,
               "Shared geometry pass did not record a complete indexed draw.");
        Expect(commandContext.lastDynamicOffsets.size() == 1
                   && commandContext.lastDynamicOffsets[0].binding == 1
                   && commandContext.lastDynamicOffsets[0].offset == 256,
               "Shared geometry pass did not forward its dynamic buffer offset.");

        const std::uint32_t drawCountBeforeFullscreen =
            commandContext.draws;
        Prism::Renderer::FullscreenTrianglePass fullscreenPass;
        fullscreenPass.Execute(commandContext);
        Expect(
            commandContext.draws
                    == drawCountBeforeFullscreen + 1
                && commandContext.lastVertexCount == 3,
               "Fullscreen pass did not record a three-vertex draw.");

        MockTextureView shadowDepthView;
        Prism::RHI::RenderingInfo shadowRendering{};
        shadowRendering.width = 1024;
        shadowRendering.height = 1024;
        Prism::RHI::RenderingAttachment shadowDepth{};
        shadowDepth.view = &shadowDepthView;
        shadowRendering.depthAttachment = shadowDepth;
        Prism::Renderer::RhiShadowPass shadowPass;
        shadowPass.Execute(commandContext, shadowRendering, pipeline, std::span(&geometryDraw, 1));
        Expect(commandContext.renderingScopes == 1 && commandContext.renderingEnds == 1,
               "Shared shadow pass did not delimit its rendering scope.");
        Expect(commandContext.indexedDraws == 2,
               "Shared shadow pass did not submit its indexed geometry.");

        MockTextureView gbufferView(Prism::RHI::TextureViewType::RenderTarget);
        Prism::RHI::RenderingInfo gbufferRendering{};
        gbufferRendering.width = 1280;
        gbufferRendering.height = 720;
        Prism::RHI::RenderingAttachment gbufferAttachment{};
        gbufferAttachment.view = &gbufferView;
        gbufferRendering.colorAttachments = {gbufferAttachment, gbufferAttachment, gbufferAttachment, gbufferAttachment};
        gbufferRendering.depthAttachment = shadowDepth;
        Prism::Renderer::RhiGeometryRenderingPass geometryRenderingPass;
        geometryRenderingPass.Execute(commandContext, gbufferRendering, pipeline, std::span(&geometryDraw, 1));
        Expect(commandContext.renderingScopes == 2 && commandContext.renderingEnds == 2,
               "Shared GBuffer geometry pass did not delimit its rendering scope.");
        Expect(commandContext.indexedDraws == 3,
               "Shared GBuffer geometry pass did not submit indexed geometry.");

        Prism::RHI::RenderingInfo hdrRendering{};
        hdrRendering.width = 1280;
        hdrRendering.height = 720;
        hdrRendering.colorAttachments.push_back(gbufferAttachment);
        const std::array<Prism::Renderer::FullscreenDraw, 2> fullscreenDraws = {
            Prism::Renderer::FullscreenDraw{&pipeline, nullptr},
            Prism::Renderer::FullscreenDraw{&pipeline, &descriptorSet},
        };
        Prism::Renderer::RhiFullscreenRenderingPass fullscreenRenderingPass;
        fullscreenRenderingPass.Execute(commandContext, hdrRendering, fullscreenDraws);
        Expect(commandContext.renderingScopes == 3 && commandContext.renderingEnds == 3,
               "Shared fullscreen pass did not delimit its rendering scope.");
        Expect(
            commandContext.draws
                    == drawCountBeforeFullscreen + 3
                && commandContext.lastVertexCount == 3,
               "Shared fullscreen pass did not submit both full-screen triangles.");

        std::cout << "RenderGraph and shared RHI pass tests passed.\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "RenderGraph test failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
