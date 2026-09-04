#include "Renderer/RenderGraph.h"
#include "Renderer/Graph/CompiledGraph.h"
#include "Renderer/Graph/GraphCompiler.h"
#include "Renderer/Graph/GraphExecutionState.h"
#include "Renderer/Graph/GraphExecutor.h"
#include "Renderer/Graph/QueueScheduler.h"
#include "Renderer/Graph/ResourceLifetimePlanner.h"
#include "Renderer/Graph/ResourceStateTracker.h"
#include "Renderer/Graph/TransientAliasPlanner.h"
#include "RHI/ICommandContext.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
void Expect(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class MockTexture final : public Prism::RHI::ITexture
{
public:
    MockTexture() = default;

    explicit MockTexture(
        Prism::RHI::TextureDescription description,
        std::optional<Prism::RHI::TransientTextureAllocationInfo>
            allocation = std::nullopt)
        : m_description(description),
          m_allocation(std::move(allocation))
    {
    }

    Prism::RHI::GraphicsApi GetGraphicsApi() const override
    {
        return Prism::RHI::GraphicsApi::Vulkan;
    }

    const Prism::RHI::TextureDescription& GetDescription() const override
    {
        return m_description;
    }

    const Prism::RHI::TransientTextureAllocationInfo*
        GetTransientAllocationInfo() const override
    {
        return m_allocation.has_value() ? &*m_allocation : nullptr;
    }

private:
    Prism::RHI::TextureDescription m_description{
        Prism::RHI::TextureDimension::Texture2D,
        64,
        64,
        1,
        1,
        1,
        Prism::RHI::Format::Rgba16Float,
        Prism::RHI::TextureUsage::RenderTarget
            | Prism::RHI::TextureUsage::ShaderResource
            | Prism::RHI::TextureUsage::UnorderedAccess,
        Prism::RHI::MemoryAccess::GpuOnly};
    std::optional<Prism::RHI::TransientTextureAllocationInfo> m_allocation;
};

class MockCommandContext final : public Prism::RHI::ICommandContext
{
public:
    Prism::RHI::GraphicsApi GetGraphicsApi() const override
    {
        return Prism::RHI::GraphicsApi::Vulkan;
    }

    Prism::RHI::CommandQueueCapabilities GetQueueCapabilities() const override
    {
        return {true, true, true, true, true, true, true, false};
    }

    Prism::RHI::CommandQueueType GetActiveCommandQueue() const override
    {
        return activeQueue;
    }

    bool SwitchCommandQueue(const Prism::RHI::CommandQueueType queue) override
    {
        activeQueue = queue;
        queueSwitches.push_back(queue);
        return true;
    }

    bool BeginQueueBatch(
        const Prism::RHI::CommandQueueType queue,
        const std::span<const Prism::RHI::QueueSyncPoint> waits) override
    {
        activeQueue = queue;
        batchQueues.push_back(queue);
        batchWaits.emplace_back(waits.begin(), waits.end());
        batchOpen = true;
        return true;
    }

    Prism::RHI::QueueSyncPoint EndQueueBatch() override
    {
        Expect(batchOpen, "A mock queue batch was ended before it began.");
        batchOpen = false;
        const std::size_t queueIndex =
            activeQueue == Prism::RHI::CommandQueueType::Compute ? 1u : 0u;
        return {activeQueue, nextBatchValues[queueIndex]++};
    }

    bool FlushQueueBatches() override
    {
        batchesFlushed = !batchOpen;
        return batchesFlushed;
    }

    bool ResumeGraphicsQueue(
        const std::span<const Prism::RHI::QueueSyncPoint> waits) override
    {
        continuationWaits.assign(waits.begin(), waits.end());
        activeQueue = Prism::RHI::CommandQueueType::Graphics;
        return !batchOpen;
    }

    std::unique_ptr<Prism::RHI::IParallelCommandRecording>
        CreateParallelCommandRecording(Prism::RHI::CommandQueueType) override
    {
        return {};
    }

    bool AppendParallelCommandRecording(
        std::unique_ptr<Prism::RHI::IParallelCommandRecording>) override
    {
        return false;
    }

    void BeginRendering(const Prism::RHI::RenderingInfo&) override {}
    void EndRendering() override {}
    void BindGraphicsPipeline(const Prism::RHI::IGraphicsPipeline&) override {}
    void BindComputePipeline(const Prism::RHI::IComputePipeline&) override {}
    void BindVertexBuffer(const Prism::RHI::IBuffer&, std::uint32_t) override {}
    void BindIndexBuffer(const Prism::RHI::IBuffer&, Prism::RHI::IndexFormat) override {}
    void BindDescriptorSet(
        const Prism::RHI::IDescriptorSet&,
        std::span<const Prism::RHI::DynamicBufferOffset>) override {}
    void DrawIndexed(
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::int32_t,
        std::uint32_t) override {}
    void DrawIndexedIndirect(
        const Prism::RHI::IBuffer&,
        std::size_t,
        std::uint32_t,
        std::uint32_t,
        const Prism::RHI::IBuffer*,
        std::size_t) override {}
    void Draw(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override {}
    void Dispatch(std::uint32_t, std::uint32_t, std::uint32_t) override {}
    void CopyBuffer(
        const Prism::RHI::IBuffer&,
        Prism::RHI::IBuffer&,
        std::size_t,
        std::size_t,
        std::size_t) override {}

    void TextureBarrier(const Prism::RHI::TextureBarrier& barrier) override
    {
        textureBarriers.push_back(barrier);
        barrierQueues.push_back(activeQueue);
    }

    void BufferBarrier(const Prism::RHI::BufferBarrier&) override {}
    void GlobalBarrier(const Prism::RHI::GlobalBarrier&) override {}
    void TextureViewBarrier(
        const Prism::RHI::ITextureView&,
        Prism::RHI::ResourceState,
        Prism::RHI::ResourceState) override {}
    void TextureAliasingBarrier(
        const Prism::RHI::ITexture* before,
        Prism::RHI::ITexture& after) override
    {
        textureAliasSources.push_back(before);
        textureAliasDestinations.push_back(&after);
    }
    void BufferAliasingBarrier(
        const Prism::RHI::IBuffer*,
        Prism::RHI::IBuffer&) override {}

    Prism::RHI::CommandQueueType activeQueue =
        Prism::RHI::CommandQueueType::Graphics;
    bool batchOpen = false;
    bool batchesFlushed = false;
    std::array<std::uint64_t, 2> nextBatchValues{1, 1};
    std::vector<Prism::RHI::CommandQueueType> queueSwitches;
    std::vector<Prism::RHI::CommandQueueType> batchQueues;
    std::vector<std::vector<Prism::RHI::QueueSyncPoint>> batchWaits;
    std::vector<Prism::RHI::QueueSyncPoint> continuationWaits;
    std::vector<Prism::RHI::TextureBarrier> textureBarriers;
    std::vector<Prism::RHI::CommandQueueType> barrierQueues;
    std::vector<const Prism::RHI::ITexture*> textureAliasSources;
    std::vector<const Prism::RHI::ITexture*> textureAliasDestinations;
};

void TestOutputCullingAndDependencies()
{
    using Graph = Prism::Renderer::RenderGraph;
    Graph graph;
    graph.ImportResource("Input");
    graph.AddContextPass("UsedProducer", {"Input"}, {"Used"},
        [](Prism::RHI::ICommandContext&) {});
    graph.AddContextPass("DeadProducer", {"Input"}, {"Dead"},
        [](Prism::RHI::ICommandContext&) {});
    graph.AddContextPass("FinalOutput", {"Used"}, {"Output"},
        [](Prism::RHI::ICommandContext&) {});
    graph.MarkOutput("Output");
    graph.Compile();

    const auto& summary = graph.GetCompilationSummary();
    const auto passes = graph.GetPassDescriptions();
    Expect(summary.declaredPassCount == 3 && summary.activePassCount == 2
            && summary.culledPassCount == 1,
        "Output reachability changed the culling counts.");
    Expect(passes[1].culled
            && passes[1].cullReason == "not_reachable_from_output"
            && passes[2].dependencies == std::vector<std::size_t>{0},
        "Output reachability changed the reduced execution dependency.");
}

void TestCompilerDataBoundaries()
{
    using namespace Prism::Renderer;

    static_assert(std::is_same_v<RenderGraph::PassOptions, GraphPassOptions>);
    static_assert(std::is_same_v<RenderGraph::CompilationSummary,
        GraphCompilationSummary>);
    static_assert(std::is_copy_constructible_v<CompiledGraph>);
    static_assert(!std::is_invocable_v<GraphCompiledPass>);

    std::weak_ptr<int> callbackLifetime;
    CompiledGraph plan;
    {
        GraphDescription description;
        auto callbackOwner = std::make_shared<int>(7);
        callbackLifetime = callbackOwner;
        GraphPassDeclaration declaration;
        declaration.info.name = "CallbackOwner";
        declaration.execute = [callbackOwner]() { (void)*callbackOwner; };
        description.passes.push_back(std::move(declaration));

        plan.declarationFingerprint = 42;
        plan.passes.push_back({0, true, {}, {}, {}});
        plan.graphSignature = "stable-plan";
        plan.valid = true;
    }
    Expect(callbackLifetime.expired()
            && plan.valid
            && plan.passes.size() == 1
            && plan.graphSignature == "stable-plan",
        "The compiled plan retained a declaration callback lifetime.");

    GraphExecutionState execution;
    execution.passInfos.push_back({"PreviousFrame", 4.0f});
    const CompiledGraph planBeforeExecution = plan;
    GraphCompilationSummary activeSummary = plan.summary;
    execution.BeginExecution(activeSummary);
    Expect(execution.executing
            && execution.executionGeneration == 1
            && execution.passInfos.empty()
            && plan.graphSignature == planBeforeExecution.graphSignature
            && plan.passes.front().executionIndex
                == planBeforeExecution.passes.front().executionIndex,
        "Per-execution reset mutated the compiled plan or retained prior timing.");
    activeSummary.executedAliasingBarrierCount = 3;
    execution.EndExecution(activeSummary, true);
    Expect(!execution.executing,
        "The per-execution state did not close its execution lifetime.");
    Expect(activeSummary.executedAliasingBarrierCount == 0
            && execution.GetVisibleSummary(activeSummary)
                   .executedAliasingBarrierCount == 3,
        "The compiled plan retained mutable state from the previous execution.");
}

void TestExtractedCompilerPlanning()
{
    using namespace Prism::Renderer;

    GraphDescription description;
    const auto addPass =
        [&](std::string name,
            std::vector<std::string> reads,
            std::vector<std::string> writes)
        {
            GraphPassDeclaration pass;
            pass.info.name = std::move(name);
            pass.reads = std::move(reads);
            pass.writes = std::move(writes);
            pass.originalIndex = description.passes.size();
            description.passes.push_back(std::move(pass));
        };
    addPass("Producer", {}, {"A"});
    addPass("DeadBranch", {}, {"Dead"});
    addPass("Transform", {"A"}, {"B"});
    addPass("Consumer", {"A", "B"}, {"Output"});
    description.outputResources.emplace("Output");

    GraphCompiler::BuildLivenessDependencies(description);
    GraphCompiler::CullPasses(description);
    const std::size_t activeCount =
        GraphCompiler::AssignExecutionIndices(description);
    GraphCompiler::BuildExecutionDependencies(description);
    GraphCompiler::ReduceTransitiveDependencies(description);
    CompiledGraph plan;
    GraphCompiler::WriteCompiledPassPlans(description, plan);

    Expect(activeCount == 3
            && description.passes[0].active
            && !description.passes[1].active
            && description.passes[1].cullReason
                == "not_reachable_from_output"
            && description.passes[2].active
            && description.passes[3].active,
        "The extracted compiler changed retained-pass liveness.");
    Expect(description.passes[3].reads
                == std::vector<std::string>{"A", "B"}
            && description.passes[3].writes
                == std::vector<std::string>{"Output"}
            && description.passes[3].executionDependencies
                == std::vector<std::size_t>{2},
        "The extracted compiler lost accesses or changed the reduced DAG.");
    Expect(plan.passes.size() == description.passes.size()
            && !plan.passes[1].active
            && plan.passes[3].executionDependencies
                == std::vector<std::size_t>{2},
        "The compiled pass plan diverged from the compiler result.");

    GraphDescription invalid;
    GraphPassDeclaration invalidPass;
    invalidPass.originalIndex = 3;
    invalid.passes.push_back(std::move(invalidPass));
    bool invalidIndexRejected = false;
    try
    {
        GraphCompiler::BuildLivenessDependencies(invalid);
    }
    catch (const std::runtime_error&)
    {
        invalidIndexRejected = true;
    }
    Expect(invalidIndexRejected,
        "The extracted compiler accepted an invalid pass index.");
}

void TestExtractedResourceLifetimes()
{
    using namespace Prism::Renderer;
    using State = Prism::RHI::ResourceState;

    const Prism::RHI::TextureDescription textureDescription{
        Prism::RHI::TextureDimension::Texture2D,
        64, 64, 1, 4, 1,
        Prism::RHI::Format::Rgba16Float,
        Prism::RHI::TextureUsage::ShaderResource
            | Prism::RHI::TextureUsage::UnorderedAccess,
        Prism::RHI::MemoryAccess::GpuOnly};
    GraphDescription description;
    description.importedResources.emplace("Input");
    description.transientTextures.emplace(
        "Temp",
        GraphTransientTextureDeclaration{
            textureDescription,
            State::ShaderResource});
    description.registeredTextures.push_back({
        "History", 1, 2,
        RenderGraphResourceLifetime::History,
        nullptr, textureDescription});

    GraphPassDeclaration producer;
    producer.info.name = "Producer";
    producer.reads = {"Input"};
    producer.writes = {"Temp"};
    producer.originalIndex = 0;
    producer.executionIndex = 0;
    producer.resourceWrites.push_back({
        "Temp", State::UnorderedAccess, true, false, 0,
        {2, 1, 0, 1}, {}});
    description.passes.push_back(std::move(producer));

    GraphPassDeclaration consumer;
    consumer.info.name = "Consumer";
    consumer.reads = {"Temp"};
    consumer.writes = {"Output"};
    consumer.originalIndex = 1;
    consumer.executionIndex = 1;
    description.passes.push_back(std::move(consumer));

    GraphPassDeclaration dead;
    dead.info.name = "NoConsumer";
    dead.writes = {"Dead"};
    dead.originalIndex = 2;
    dead.active = false;
    description.passes.push_back(std::move(dead));

    GraphExecutionState execution;
    execution.textureResources.emplace(
        "Input",
        GraphTextureExecutionResource{
            nullptr, State::ShaderResource,
            std::vector<State>(4, State::ShaderResource)});
    CompiledGraph plan;
    ResourceLifetimePlanner::Build(description, execution, plan);

    const auto& input = plan.compiledResources.at("Input");
    const auto& transient = plan.compiledResources.at("Temp");
    const auto& output = plan.compiledResources.at("Output");
    const auto& history = plan.compiledResources.at("History");
    const auto& deadResource = plan.compiledResources.at("Dead");
    Expect(input.imported && input.firstUse == 0 && input.lastUse == 0,
        "Imported resource lifetime changed in the extracted planner.");
    Expect(transient.transient && transient.texture
            && transient.firstUse == 0 && transient.lastUse == 1
            && transient.estimatedBytes > 0,
        "Transient resource lifetime or size changed in the extracted planner.");
    Expect(output.firstUse == 1 && output.lastUse == 1
            && history.imported && history.history
            && history.currentVersion == 2,
        "Export/history lifetime metadata changed in the extracted planner.");
    Expect(!deadResource.active
            && deadResource.firstUse
                == std::numeric_limits<std::size_t>::max()
            && description.passes[0].resourceWrites[0]
                   .textureRange.baseMipLevel == 2,
        "An unused resource became live or subresource access was mutated.");
}

void TestVersionsAndHistory()
{
    using Graph = Prism::Renderer::RenderGraph;
    using State = Prism::RHI::ResourceState;

    MockTexture versionedTexture;
    Graph graph;
    const Prism::Renderer::TextureHandle texture =
        graph.DeclareTexture("Versioned", versionedTexture, State::ShaderResource);
    auto write = graph.CreatePassParameters();
    const Prism::Renderer::TextureHandle version1 =
        write.WriteTexture(texture, State::UnorderedAccess);
    graph.AddParameterPass("WriteVersion", std::move(write),
        [](Prism::RHI::ICommandContext&,
           const Prism::Renderer::RenderGraphPassResources&) {});

    bool staleVersionRejected = false;
    try
    {
        auto staleRead = graph.CreatePassParameters();
        staleRead.ReadTexture(texture, State::ShaderResource);
        graph.AddParameterPass("StaleRead", std::move(staleRead),
            [](Prism::RHI::ICommandContext&,
               const Prism::Renderer::RenderGraphPassResources&) {});
    }
    catch (const std::runtime_error&)
    {
        staleVersionRejected = true;
    }
    Expect(staleVersionRejected, "A stale texture version was accepted.");

    auto read = graph.CreatePassParameters();
    read.ReadTexture(version1, State::ShaderResource);
    graph.AddParameterPass("ReadVersion", std::move(read),
        [](Prism::RHI::ICommandContext&,
           const Prism::Renderer::RenderGraphPassResources&) {},
        {Graph::QueueClass::Graphics, true, false});
    graph.Compile();
    const auto resources = graph.GetResourceDescriptions();
    const auto resource = std::ranges::find(resources, std::string("Versioned"),
        &Graph::ResourceDescription::name);
    Expect(resource != resources.end() && resource->currentVersion == 1
            && graph.GetCompilationSummary().versionedResourceCount == 1,
        "Texture version compilation metadata changed.");

    const Prism::Renderer::TextureHandle oldGeneration = version1;
    graph.Reset();
    bool generationRejected = false;
    try
    {
        (void)graph.ResolveTexture(oldGeneration);
    }
    catch (const std::runtime_error&)
    {
        generationRejected = true;
    }
    Expect(generationRejected, "A handle from an old graph generation was accepted.");

    MockTexture previous;
    MockTexture current;
    const Prism::Renderer::TextureHistoryHandle history =
        graph.ImportTextureHistory("TemporalColor", previous, State::ShaderResource,
            current, State::ShaderResource);
    auto historyParameters = graph.CreatePassParameters();
    historyParameters.ReadTexture(history.previous, State::ShaderResource);
    const Prism::Renderer::TextureHandle historyVersion =
        historyParameters.WriteTexture(history.current, State::RenderTarget);
    graph.AddParameterPass("ResolveHistory", std::move(historyParameters),
        [](Prism::RHI::ICommandContext&,
           const Prism::Renderer::RenderGraphPassResources&) {});
    graph.MarkOutput(historyVersion);
    graph.Compile();
    Expect(graph.GetCompilationSummary().historyResourceCount == 2
            && graph.GetCompilationSummary().versionedResourceCount == 1,
        "History imports or the current history version changed.");
}

void TestExtractedTransientAliasing()
{
    using namespace Prism::Renderer;
    using Queue = GraphQueueClass;
    using State = Prism::RHI::ResourceState;

    const Prism::RHI::TextureDescription compatible{
        Prism::RHI::TextureDimension::Texture2D,
        64,
        64,
        1,
        1,
        1,
        Prism::RHI::Format::Rgba16Float,
        Prism::RHI::TextureUsage::RenderTarget
            | Prism::RHI::TextureUsage::ShaderResource,
        Prism::RHI::MemoryAccess::GpuOnly};
    Prism::RHI::TextureDescription incompatible = compatible;
    incompatible.width = 128;

    GraphDescription description;
    description.transientTextures.emplace(
        "AliasA",
        GraphTransientTextureDeclaration{compatible, State::Undefined});
    description.transientTextures.emplace(
        "AliasB",
        GraphTransientTextureDeclaration{compatible, State::Undefined});
    description.transientTextures.emplace(
        "Overlap",
        GraphTransientTextureDeclaration{compatible, State::Undefined});
    description.transientTextures.emplace(
        "Incompatible",
        GraphTransientTextureDeclaration{incompatible, State::Undefined});

    const auto addPass =
        [&](const char* name,
            const Queue queue,
            std::vector<std::string> reads,
            std::vector<std::string> writes,
            std::vector<std::size_t> dependencies)
        {
            GraphPassDeclaration pass;
            pass.info.name = name;
            pass.originalIndex = description.passes.size();
            pass.executionIndex = pass.originalIndex;
            pass.options.queue = queue;
            pass.active = true;
            pass.reads = std::move(reads);
            pass.writes = std::move(writes);
            pass.executionDependencies = std::move(dependencies);
            description.passes.push_back(std::move(pass));
        };

    addPass("WriteAliasA", Queue::Graphics, {}, {"AliasA", "Overlap"}, {});
    addPass("ConsumeAliasA", Queue::Compute, {"AliasA"}, {}, {0});
    addPass("WriteAliasB", Queue::Graphics, {"Overlap"}, {"AliasB"}, {0});
    addPass("ConsumeAliasB", Queue::Compute, {"AliasB"}, {}, {1, 2});
    addPass("WriteIncompatible", Queue::Graphics, {}, {"Incompatible"}, {2});
    addPass(
        "ConsumeIncompatible",
        Queue::Compute,
        {"Incompatible"},
        {},
        {3, 4});

    GraphExecutionState executionState;
    CompiledGraph compiledGraph;
    ResourceLifetimePlanner::Build(
        description,
        executionState,
        compiledGraph);
    TransientAliasPlanner::Build(
        description,
        executionState,
        compiledGraph);

    const GraphCompiledResource& aliasA =
        compiledGraph.compiledResources.at("AliasA");
    const GraphCompiledResource& aliasB =
        compiledGraph.compiledResources.at("AliasB");
    const GraphCompiledResource& overlap =
        compiledGraph.compiledResources.at("Overlap");
    const GraphCompiledResource& incompatibleResource =
        compiledGraph.compiledResources.at("Incompatible");
    Expect(
        aliasA.physicalAllocation == aliasB.physicalAllocation,
        "Compatible non-overlapping resources did not reuse an alias slot.");
    Expect(
        overlap.physicalAllocation != aliasA.physicalAllocation
            && incompatibleResource.physicalAllocation
                != aliasA.physicalAllocation,
        "Overlapping or incompatible resources incorrectly reused an alias slot.");
    Expect(
        std::ranges::find(
            description.passes[2].executionDependencies,
            std::size_t{1})
            != description.passes[2].executionDependencies.end(),
        "Cross-queue alias reuse did not add the producer dependency.");

    GraphCompiler::ReduceTransitiveDependencies(description);
    Expect(
        description.passes[2].executionDependencies
            == std::vector<std::size_t>{1},
        "Transitive reduction discarded the required alias dependency.");

    GraphDescription nativeDescription;
    nativeDescription.transientTextures.emplace(
        "NativeA",
        GraphTransientTextureDeclaration{compatible, State::Undefined});
    nativeDescription.transientTextures.emplace(
        "NativeB",
        GraphTransientTextureDeclaration{compatible, State::Undefined});
    GraphPassDeclaration nativeWriteA;
    nativeWriteA.info.name = "NativeWriteA";
    nativeWriteA.originalIndex = 0;
    nativeWriteA.executionIndex = 0;
    nativeWriteA.active = true;
    nativeWriteA.writes = {"NativeA"};
    nativeDescription.passes.push_back(std::move(nativeWriteA));
    GraphPassDeclaration nativeReadA;
    nativeReadA.info.name = "NativeReadA";
    nativeReadA.originalIndex = 1;
    nativeReadA.executionIndex = 1;
    nativeReadA.active = true;
    nativeReadA.reads = {"NativeA"};
    nativeReadA.executionDependencies = {0};
    nativeDescription.passes.push_back(std::move(nativeReadA));
    GraphPassDeclaration nativeWriteB;
    nativeWriteB.info.name = "NativeWriteB";
    nativeWriteB.originalIndex = 2;
    nativeWriteB.executionIndex = 2;
    nativeWriteB.active = true;
    nativeWriteB.writes = {"NativeB"};
    nativeDescription.passes.push_back(std::move(nativeWriteB));
    GraphPassDeclaration nativeReadB;
    nativeReadB.info.name = "NativeReadB";
    nativeReadB.originalIndex = 3;
    nativeReadB.executionIndex = 3;
    nativeReadB.active = true;
    nativeReadB.reads = {"NativeB"};
    nativeReadB.executionDependencies = {2};
    nativeDescription.passes.push_back(std::move(nativeReadB));

    MockTexture nativeA(
        compatible,
        Prism::RHI::TransientTextureAllocationInfo{
            41, 0, 32768, 32768, 32768});
    MockTexture nativeB(
        compatible,
        Prism::RHI::TransientTextureAllocationInfo{
            41, 0, 32768, 32768, 32768});
    GraphExecutionState nativeExecution;
    nativeExecution.textureResources.emplace(
        "NativeA",
        GraphTextureExecutionResource{
            &nativeA, State::Undefined, {State::Undefined}});
    nativeExecution.textureResources.emplace(
        "NativeB",
        GraphTextureExecutionResource{
            &nativeB, State::Undefined, {State::Undefined}});
    CompiledGraph nativeGraph;
    ResourceLifetimePlanner::Build(
        nativeDescription,
        nativeExecution,
        nativeGraph);
    TransientAliasPlanner::Build(
        nativeDescription,
        nativeExecution,
        nativeGraph);
    Expect(
        nativeGraph.summary.nativeTransientAliasingApplied
            && nativeGraph.summary.nativeTransientAliasingPlanValid
            && nativeGraph.summary.nativeTransientAllocationCount == 1
            && nativeGraph.summary.nativeTransientLogicalBytes == 65536
            && nativeGraph.summary.nativeTransientPhysicalBytes == 32768
            && nativeGraph.summary.expectedAliasingBarrierCount == 2,
        "Native alias pool reuse accounting changed in the extracted planner.");
}

void TestTransientLifetimeAliasing()
{
    using Graph = Prism::Renderer::RenderGraph;
    const Prism::RHI::TextureDescription description{
        Prism::RHI::TextureDimension::Texture2D, 256, 256, 1, 1, 1,
        Prism::RHI::Format::Rgba16Float,
        Prism::RHI::TextureUsage::RenderTarget
            | Prism::RHI::TextureUsage::ShaderResource,
        Prism::RHI::MemoryAccess::GpuOnly};
    Graph graph;
    graph.DeclareTransientTexture("TransientA", description);
    graph.DeclareTransientTexture("TransientB", description);
    graph.AddContextPass("WriteA", {}, {"TransientA"},
        [](Prism::RHI::ICommandContext&) {});
    graph.AddContextPass("ConsumeA", {"TransientA"}, {},
        [](Prism::RHI::ICommandContext&) {},
        {Graph::QueueClass::Graphics, true, false});
    graph.AddContextPass("WriteB", {}, {"TransientB"},
        [](Prism::RHI::ICommandContext&) {});
    graph.AddContextPass("ConsumeB", {"TransientB"}, {"Output"},
        [](Prism::RHI::ICommandContext&) {});
    graph.MarkOutput("Output");
    graph.Compile();

    const auto resources = graph.GetResourceDescriptions();
    const auto transientA = std::ranges::find(resources, std::string("TransientA"),
        &Graph::ResourceDescription::name);
    const auto transientB = std::ranges::find(resources, std::string("TransientB"),
        &Graph::ResourceDescription::name);
    const auto& summary = graph.GetCompilationSummary();
    Expect(transientA != resources.end() && transientB != resources.end()
            && transientA->lastUse < transientB->firstUse
            && transientA->physicalAllocation == transientB->physicalAllocation,
        "Non-overlapping transient lifetimes no longer share an alias slot.");
    Expect(summary.transientResourceCount == 2
            && summary.transientAllocationCount == 1
            && summary.transientAliasedBytes == summary.transientPhysicalBytes,
        "Transient alias accounting changed.");
}

void TestExtractedQueueScheduling()
{
    using namespace Prism::Renderer;
    using Queue = GraphQueueClass;

    GraphDescription description;
    description.queueExecutionMode = GraphQueueExecutionMode::Automatic;
    description.automaticQueueDecision.selectNative = true;
    description.automaticQueueDecision.reason = "fixture_native";
    const auto addPass =
        [&](const char* name,
            const Queue queue,
            std::vector<std::string> reads,
            std::vector<std::string> writes,
            std::vector<std::size_t> dependencies)
        {
            GraphPassDeclaration pass;
            pass.info.name = name;
            pass.originalIndex = description.passes.size();
            pass.executionIndex = pass.originalIndex;
            pass.options.queue = queue;
            pass.active = true;
            pass.reads = std::move(reads);
            pass.writes = std::move(writes);
            pass.executionDependencies = std::move(dependencies);
            description.passes.push_back(std::move(pass));
        };
    addPass(
        "Root",
        Queue::Graphics,
        {"Input"},
        {"GraphicsInput", "ComputeInput"},
        {});
    addPass(
        "GraphicsBranch",
        Queue::Graphics,
        {"GraphicsInput"},
        {"GraphicsResult"},
        {0});
    addPass(
        "ComputeBranch",
        Queue::Compute,
        {"ComputeInput"},
        {"ComputeResult"},
        {0});
    addPass(
        "Join",
        Queue::Graphics,
        {"GraphicsResult", "ComputeResult"},
        {"Output"},
        {1, 2});

    CompiledGraph compiledGraph;
    QueueScheduler::Build(description, compiledGraph);
    Expect(
        compiledGraph.queueSyncDescriptions.size() == 2
            && compiledGraph.queueSyncDescriptions[0].resources
                == std::vector<std::string>{"ComputeInput"}
            && compiledGraph.queueSyncDescriptions[1].resources
                == std::vector<std::string>{"ComputeResult"},
        "Extracted queue synchronization lost the complete resource relation.");
    Expect(
        compiledGraph.queueBatchDescriptions.size() == 4
            && compiledGraph.passToQueueBatch
                == std::vector<std::size_t>{0, 1, 3, 2}
            && compiledGraph.queueBatchSubmissionOrder
                == std::vector<std::size_t>{0, 3, 1, 2},
        "Extracted queue batch partition or stable submission order changed.");
    Expect(
        compiledGraph.queueBatchDescriptions[2].dependencies
                == std::vector<std::size_t>{1, 3}
            && compiledGraph.queueBatchDescriptions[3].dependencies
                == std::vector<std::size_t>{0}
            && compiledGraph.summary.crossQueueSyncCount == 2
            && compiledGraph.summary.queueBatchCount == 4
            && compiledGraph.summary.crossQueueBatchDependencyCount == 2
            && compiledGraph.summary.overlapOpportunityCount >= 1,
        "Extracted queue DAG dependencies or overlap accounting changed.");
    Expect(
        description.queueExecutionMode == GraphQueueExecutionMode::Automatic
            && description.automaticQueueDecision.selectNative
            && description.automaticQueueDecision.reason == "fixture_native",
        "Queue planning mutated the serial/native/automatic policy input.");
}

void TestQueueHandoffsAndSubresourceRestore()
{
    using Graph = Prism::Renderer::RenderGraph;
    using Queue = Prism::RHI::CommandQueueType;
    using State = Prism::RHI::ResourceState;

    MockTexture imported;
    MockTexture shared;
    MockTexture reusedTransient;
    MockCommandContext context;
    Graph graph;
    (void)graph.ImportTexture("Imported", imported, State::ShaderResource);
    (void)graph.ImportTexture("Shared", shared, State::ShaderResource);
    (void)graph.DeclareTransientTexture(
        "ReusedTransient", reusedTransient, State::ShaderResource);
    graph.AddResourceContextPass("GraphicsRoot", {},
        {{"Shared", State::RenderTarget}},
        [](Prism::RHI::ICommandContext&) {});
    graph.AddResourceContextPass("ComputeRead",
        {{"Shared", State::ShaderResource}, {"Imported", State::ShaderResource}},
        {{"Token", State::Undefined}, {"ReusedTransient", State::UnorderedAccess}},
        [](Prism::RHI::ICommandContext&) {}, {Graph::QueueClass::Compute});
    graph.AddContextPass("ComputeBridge", {"Token"}, {"Bridge"},
        [](Prism::RHI::ICommandContext&) {}, {Graph::QueueClass::Compute});
    graph.AddResourceContextPass("GraphicsJoin",
        {{"Shared", State::ShaderResource}, {"Bridge", State::Undefined}},
        {{"Output", State::Undefined}},
        [](Prism::RHI::ICommandContext&) {});
    graph.MarkOutput("Output");
    graph.SetQueueExecutionMode(Graph::QueueExecutionMode::Native);
    graph.Execute(context);

    const auto hasBarrier = [&](const Prism::RHI::ITexture* texture,
                                const State before,
                                const State after,
                                const Queue queue)
    {
        for (std::size_t index = 0; index < context.textureBarriers.size(); ++index)
        {
            const auto& barrier = context.textureBarriers[index];
            if (barrier.texture == texture && barrier.before == before
                && barrier.after == after && context.barrierQueues[index] == queue)
            {
                return true;
            }
        }
        return false;
    };
    Expect(hasBarrier(&imported, State::ShaderResource, State::Common, Queue::Graphics)
            && hasBarrier(&reusedTransient, State::ShaderResource,
                State::Common, Queue::Graphics),
        "The graphics prologue no longer releases initialized compute-first textures.");
    Expect(hasBarrier(&shared, State::ShaderResource, State::Common, Queue::Compute)
            && hasBarrier(&shared, State::Common,
                State::ShaderResource, Queue::Graphics),
        "Transitive dependency reduction dropped a queue ownership transfer.");
    Expect(graph.GetCompilationSummary().crossQueueBatchDependencyCount == 2
            && context.batchWaits.size() >= 2 && !context.batchWaits[1].empty()
            && context.batchesFlushed,
        "The cross-queue DAG or its fence waits changed.");

    const Prism::RHI::TextureDescription mipDescription{
        Prism::RHI::TextureDimension::Texture2D, 128, 128, 1, 4, 1,
        Prism::RHI::Format::Rgba16Float,
        Prism::RHI::TextureUsage::ShaderResource
            | Prism::RHI::TextureUsage::UnorderedAccess,
        Prism::RHI::MemoryAccess::GpuOnly};
    MockTexture mipTexture(mipDescription);
    MockCommandContext mipContext;
    Graph mipGraph;
    const Prism::Renderer::TextureHandle mipHandle =
        mipGraph.DeclareTexture("MipChain", mipTexture, State::ShaderResource);
    auto writeMip = mipGraph.CreatePassParameters();
    const Prism::Renderer::TextureHandle writtenMip =
        writeMip.WriteTexture(mipHandle, State::UnorderedAccess, {2, 1, 0, 1});
    mipGraph.AddParameterPass("WriteMip2", std::move(writeMip),
        [](Prism::RHI::ICommandContext&,
           const Prism::Renderer::RenderGraphPassResources&) {});
    auto restoreMip = mipGraph.CreatePassParameters();
    restoreMip.ReadTexture(writtenMip, State::ShaderResource, {2, 1, 0, 1});
    mipGraph.AddParameterPass("RestoreMip2", std::move(restoreMip),
        [](Prism::RHI::ICommandContext&,
           const Prism::Renderer::RenderGraphPassResources&) {},
        {Graph::QueueClass::Graphics, true, false});
    mipGraph.Execute(mipContext);

    Expect(mipContext.textureBarriers.size() == 2
            && std::ranges::all_of(mipContext.textureBarriers,
                [](const Prism::RHI::TextureBarrier& barrier)
                {
                    return barrier.baseMipLevel == 2 && barrier.mipLevelCount == 1
                        && barrier.baseArrayLayer == 0 && barrier.arrayLayerCount == 1;
                })
            && mipContext.textureBarriers[1].before == State::UnorderedAccess
            && mipContext.textureBarriers[1].after == State::ShaderResource,
        "A mip-local write no longer restores only the accessed mip.");
}

void TestExtractedResourceStateTracking()
{
    using namespace Prism::Renderer;
    using State = Prism::RHI::ResourceState;

    const Prism::RHI::TextureDescription description{
        Prism::RHI::TextureDimension::Texture2D,
        64,
        64,
        2,
        4,
        1,
        Prism::RHI::Format::Rgba16Float,
        Prism::RHI::TextureUsage::ShaderResource
            | Prism::RHI::TextureUsage::UnorderedAccess,
        Prism::RHI::MemoryAccess::GpuOnly};
    MockTexture texture(description);
    GraphExecutionState executionState;
    executionState.textureResources.emplace(
        "Texture",
        GraphTextureExecutionResource{
            &texture,
            State::ShaderResource,
            std::vector<State>(8, State::ShaderResource)});
    CompiledGraph compiledGraph;
    compiledGraph.compiledResources.emplace(
        "Texture",
        GraphCompiledResource{
            .name = "Texture",
            .texture = true,
            .active = true,
            .state = State::ShaderResource});
    MockCommandContext context;

    GraphPassDeclaration sameStateRead;
    sameStateRead.info.name = "SameStateRead";
    sameStateRead.active = true;
    GraphTrackedResourceAccess wholeRead;
    wholeRead.name = "Texture";
    wholeRead.state = State::ShaderResource;
    wholeRead.texture = true;
    sameStateRead.resourceReads.push_back(wholeRead);
    ResourceStateTracker::PreparePassBarriers(
        sameStateRead,
        executionState,
        compiledGraph,
        context);
    Expect(
        context.textureBarriers.empty(),
        "An unchanged native texture state emitted an invalid transition.");

    GraphPassDeclaration mipWrite;
    mipWrite.info.name = "MipWrite";
    mipWrite.active = true;
    GraphTrackedResourceAccess mipAccess;
    mipAccess.name = "Texture";
    mipAccess.state = State::UnorderedAccess;
    mipAccess.texture = true;
    mipAccess.textureRange = {2, 1, 1, 1};
    mipWrite.resourceWrites.push_back(mipAccess);
    ResourceStateTracker::PreparePassBarriers(
        mipWrite,
        executionState,
        compiledGraph,
        context);
    ResourceStateTracker::PreparePassBarriers(
        mipWrite,
        executionState,
        compiledGraph,
        context);
    GraphPassDeclaration mipRestore;
    mipRestore.info.name = "MipRestore";
    mipRestore.active = true;
    mipAccess.state = State::ShaderResource;
    mipRestore.resourceReads.push_back(mipAccess);
    ResourceStateTracker::PreparePassBarriers(
        mipRestore,
        executionState,
        compiledGraph,
        context);
    const auto& states =
        executionState.textureResources.at("Texture").subresourceStates;
    Expect(
        context.textureBarriers.size() == 3
            && context.textureBarriers[0].baseMipLevel == 2
            && context.textureBarriers[0].baseArrayLayer == 1
            && context.textureBarriers[1].before == State::UnorderedAccess
            && context.textureBarriers[1].after == State::UnorderedAccess
            && context.textureBarriers[2].after == State::ShaderResource
            && std::ranges::all_of(
                states,
                [](const State state)
                {
                    return state == State::ShaderResource;
                }),
        "Extracted per-subresource or UAV barrier tracking changed.");

    GraphDescription handoffDescription;
    GraphPassDeclaration graphics;
    graphics.info.name = "Graphics";
    graphics.originalIndex = 0;
    graphics.executionIndex = 0;
    graphics.active = true;
    graphics.options.queue = GraphQueueClass::Graphics;
    graphics.reads = {"Texture"};
    handoffDescription.passes.push_back(std::move(graphics));
    GraphPassDeclaration compute;
    compute.info.name = "Compute";
    compute.originalIndex = 1;
    compute.executionIndex = 1;
    compute.active = true;
    compute.options.queue = GraphQueueClass::Compute;
    compute.reads = {"Texture"};
    handoffDescription.passes.push_back(std::move(compute));
    ResourceStateTracker::PrepareQueueHandoff(
        GraphQueueClass::Graphics,
        GraphQueueClass::Compute,
        1,
        handoffDescription,
        executionState,
        compiledGraph,
        context);
    Expect(
        context.textureBarriers.size() == 4
            && context.textureBarriers.back().before == State::ShaderResource
            && context.textureBarriers.back().after == State::Common
            && std::ranges::all_of(
                states,
                [](const State state)
                {
                    return state == State::Common;
                }),
        "Extracted queue handoff did not release the complete texture state.");

    MockTexture nativeA(
        description,
        Prism::RHI::TransientTextureAllocationInfo{
            73, 0, 65536, 65536, 65536});
    MockTexture nativeB(
        description,
        Prism::RHI::TransientTextureAllocationInfo{
            73, 0, 65536, 65536, 65536});
    GraphExecutionState aliasExecution;
    aliasExecution.textureResources.emplace(
        "NativeA",
        GraphTextureExecutionResource{
            &nativeA,
            State::ShaderResource,
            std::vector<State>(8, State::ShaderResource)});
    aliasExecution.textureResources.emplace(
        "NativeB",
        GraphTextureExecutionResource{
            &nativeB,
            State::ShaderResource,
            std::vector<State>(8, State::ShaderResource)});
    CompiledGraph aliasGraph;
    GraphCompiledResource firstAlias;
    firstAlias.name = "NativeA";
    firstAlias.texture = true;
    firstAlias.transient = true;
    firstAlias.active = true;
    firstAlias.firstUse = 0;
    firstAlias.lastUse = 1;
    firstAlias.nativePoolId = 73;
    firstAlias.nativeAllocation = 0;
    aliasGraph.compiledResources.emplace("NativeA", firstAlias);
    GraphCompiledResource secondAlias = firstAlias;
    secondAlias.name = "NativeB";
    secondAlias.firstUse = 2;
    secondAlias.lastUse = 3;
    aliasGraph.compiledResources.emplace("NativeB", secondAlias);
    aliasGraph.nativeAliasGroups.emplace(
        "73:0",
        std::vector<std::string>{"NativeA", "NativeB"});
    GraphPassDeclaration aliasPass;
    aliasPass.info.name = "UseNativeB";
    aliasPass.executionIndex = 2;
    GraphTrackedResourceAccess aliasAccess;
    aliasAccess.name = "NativeB";
    aliasAccess.state = State::UnorderedAccess;
    aliasAccess.texture = true;
    aliasPass.resourceWrites.push_back(aliasAccess);
    ResourceStateTracker::PrepareAliasingBarriers(
        aliasPass,
        aliasExecution,
        aliasGraph,
        context);
    Expect(
        context.textureAliasSources
                == std::vector<const Prism::RHI::ITexture*>{&nativeA}
            && context.textureAliasDestinations
                == std::vector<const Prism::RHI::ITexture*>{&nativeB}
            && aliasGraph.summary.executedAliasingBarrierCount == 1
            && std::ranges::all_of(
                aliasExecution.textureResources.at("NativeB")
                    .subresourceStates,
                [](const State state)
                {
                    return state == State::Undefined;
                }),
        "Extracted native alias barrier did not reset the reused resource state.");
}

void TestExtractedExecutorFailureAndRecovery()
{
    using namespace Prism::Renderer;

    GraphDescription description;
    description.importedResources.emplace("Input");
    std::vector<std::string> executionOrder;
    GraphPassDeclaration first;
    first.info.name = "First";
    first.originalIndex = 0;
    first.executionIndex = 0;
    first.active = true;
    first.reads = {"Input"};
    first.writes = {"Intermediate"};
    first.execute = [&]() { executionOrder.emplace_back("First"); };
    description.passes.push_back(std::move(first));
    GraphPassDeclaration failing;
    failing.info.name = "Failing";
    failing.originalIndex = 1;
    failing.executionIndex = 1;
    failing.active = true;
    failing.reads = {"Intermediate"};
    failing.writes = {"Output"};
    failing.execute = []() { throw std::runtime_error("recording failed"); };
    description.passes.push_back(std::move(failing));

    CompiledGraph compiledGraph;
    compiledGraph.summary.compiled = true;
    compiledGraph.summary.activePassCount = 2;
    GraphExecutionState executionState;
    executionState.passInfos.push_back({"stale", 1.0f});
    bool rejected = false;
    try
    {
        GraphExecutor::Execute(
            description,
            executionState,
            compiledGraph,
            false);
    }
    catch (const std::runtime_error&)
    {
        rejected = true;
    }
    Expect(
        rejected
            && executionOrder == std::vector<std::string>{"First"}
            && !executionState.executing
            && executionState.passInfos.empty()
            && !executionState.hasCompletedSummary
            && compiledGraph.summary.compiled
            && compiledGraph.summary.activePassCount == 2,
        "Extracted executor published a partial failed execution.");

    description.passes[1].execute =
        [&]() { executionOrder.emplace_back("Recovered"); };
    GraphExecutor::Execute(
        description,
        executionState,
        compiledGraph,
        false);
    Expect(
        executionOrder
                == std::vector<std::string>{"First", "First", "Recovered"}
            && !executionState.executing
            && executionState.hasCompletedSummary
            && executionState.passInfos.size() == 2
            && executionState.completedSummary.compiled
            && compiledGraph.summary.compiled,
        "Extracted executor did not recover on the next complete execution.");

    GraphDescription recordingDescription;
    recordingDescription.importedResources.emplace("Input");
    recordingDescription.queueExecutionMode =
        GraphQueueExecutionMode::Native;
    const auto addContextPass =
        [&](const char* name,
            const GraphQueueClass queue,
            std::vector<std::string> reads,
            std::vector<std::string> writes,
            std::vector<std::size_t> dependencies,
            GraphPassDeclaration::ContextExecuteCallback callback,
            const bool parallel)
        {
            GraphPassDeclaration pass;
            pass.info.name = name;
            pass.originalIndex = recordingDescription.passes.size();
            pass.executionIndex = pass.originalIndex;
            pass.active = true;
            pass.options.queue = queue;
            pass.options.parallelRecordable = parallel;
            if (parallel)
            {
                pass.options.parallelRecordingContract =
                    Prism::Renderer::GraphParallelRecordingContract::
                        AuditedIndependent();
            }
            pass.reads = std::move(reads);
            pass.writes = std::move(writes);
            pass.executionDependencies = std::move(dependencies);
            pass.contextExecute = std::move(callback);
            recordingDescription.passes.push_back(std::move(pass));
        };
    addContextPass(
        "GraphicsProduce",
        GraphQueueClass::Graphics,
        {"Input"},
        {"GraphicsData"},
        {},
        [](Prism::RHI::ICommandContext&) {},
        false);
    addContextPass(
        "FailingComputeRecord",
        GraphQueueClass::Compute,
        {"GraphicsData"},
        {"ComputeData"},
        {0},
        [](Prism::RHI::ICommandContext&)
        {
            throw std::runtime_error("parallel recording failed");
        },
        true);
    addContextPass(
        "GraphicsConsume",
        GraphQueueClass::Graphics,
        {"ComputeData"},
        {"Output"},
        {1},
        [](Prism::RHI::ICommandContext&) {},
        false);
    CompiledGraph recordingGraph;
    QueueScheduler::Build(recordingDescription, recordingGraph);
    GraphExecutionState recordingState;
    MockCommandContext recordingContext;
    bool recordingRejected = false;
    try
    {
        GraphExecutor::Execute(
            recordingDescription,
            recordingState,
            recordingGraph,
            false,
            recordingContext);
    }
    catch (const std::runtime_error&)
    {
        recordingRejected = true;
    }
    Expect(
        recordingRejected
            && !recordingState.executing
            && recordingState.passInfos.empty()
            && !recordingState.hasCompletedSummary
            && recordingContext.batchQueues.empty(),
        "A failed parallel recording submitted a partial queue batch.");
}
} // namespace

int main()
{
    try
    {
        TestCompilerDataBoundaries();
        TestExtractedCompilerPlanning();
        TestExtractedResourceLifetimes();
        TestExtractedTransientAliasing();
        TestOutputCullingAndDependencies();
        TestVersionsAndHistory();
        TestTransientLifetimeAliasing();
        TestExtractedQueueScheduling();
        TestQueueHandoffsAndSubresourceRestore();
        TestExtractedResourceStateTracking();
        TestExtractedExecutorFailureAndRecovery();
        std::cout << "RenderGraph compiler baseline tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "RenderGraph compiler baseline tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
