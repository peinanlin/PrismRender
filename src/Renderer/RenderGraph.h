#pragma once

#include "Renderer/Graph/CompiledGraph.h"
#include "Renderer/Graph/GraphExecutionState.h"
#include "RHI/GraphicsResources.h"
#include "Renderer/QueueSchedulingCostModel.h"
#include "Renderer/RenderGraphResources.h"

#include <functional>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Prism::RHI
{
class ICommandContext;
class IBuffer;
class ITexture;
class ITextureView;
}

namespace Prism::Core
{
class ITaskExecutor;
}

namespace Prism::Renderer
{
class RenderGraph
{
public:
    RenderGraph() = default;
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RenderGraph(RenderGraph&&) = delete;
    RenderGraph& operator=(RenderGraph&&) = delete;

    using ExecuteCallback = std::function<void()>;
    using ContextExecuteCallback = std::function<void(RHI::ICommandContext&)>;
    using ParameterExecuteCallback = std::function<void(
        RHI::ICommandContext&,
        const RenderGraphPassResources&)>;
    using MarkerCallback = std::function<void(std::string_view)>;

    using QueueClass = GraphQueueClass;
    using QueueExecutionMode = GraphQueueExecutionMode;
    using ParallelRecordingContract = GraphParallelRecordingContract;
    using PassOptions = GraphPassOptions;
    using PassInfo = GraphPassInfo;
    using ResourceAccess = GraphResourceAccess;
    using ResourceDescription = GraphResourceDescription;
    using PassDescription = GraphPassDescription;
    using QueueSyncDescription = GraphQueueSyncDescription;
    using QueueBatchDescription = GraphQueueBatchDescription;
    using CompilationSummary = GraphCompilationSummary;

    void Reset();
    void ImportResource(std::string resourceName);
    TextureHandle DeclareTexture(
        std::string resourceName,
        RHI::ITexture& texture,
        RHI::ResourceState initialState);
    TextureHandle ImportTexture(
        std::string resourceName,
        RHI::ITexture& texture,
        RHI::ResourceState initialState);
    TextureHandle DeclareTransientTexture(
        std::string resourceName,
        const RHI::TextureDescription& description,
        RHI::ResourceState initialState =
            RHI::ResourceState::Undefined);
    TextureHandle DeclareTransientTexture(
        std::string resourceName,
        RHI::ITexture& texture,
        RHI::ResourceState initialState);
    BufferHandle DeclareBuffer(
        std::string resourceName,
        RHI::IBuffer& buffer,
        RHI::ResourceState initialState);
    BufferHandle ImportBuffer(
        std::string resourceName,
        RHI::IBuffer& buffer,
        RHI::ResourceState initialState);
    BufferHandle DeclareTransientBuffer(
        std::string resourceName,
        const RHI::BufferDescription& description,
        RHI::ResourceState initialState =
            RHI::ResourceState::Undefined);
    BufferHandle DeclareTransientBuffer(
        std::string resourceName,
        RHI::IBuffer& buffer,
        RHI::ResourceState initialState);
    TextureViewHandle DeclareTextureView(
        std::string viewName,
        TextureHandle texture,
        RHI::ITextureView& view);
    TextureHistoryHandle ImportTextureHistory(
        std::string resourceName,
        RHI::ITexture& previous,
        RHI::ResourceState previousState,
        RHI::ITexture& current,
        RHI::ResourceState currentState);
    void MarkOutput(std::string resourceName);
    void MarkOutput(TextureHandle handle);
    void MarkOutput(BufferHandle handle);
    void SetPassCullingEnabled(bool enabled);
    void SetDetailedProfilingEnabled(bool enabled) noexcept
    { m_detailedProfilingEnabled = enabled; }
    [[nodiscard]] bool IsDetailedProfilingEnabled() const noexcept
    { return m_detailedProfilingEnabled; }
    void SetQueueExecutionMode(QueueExecutionMode mode);
    void SetAutomaticQueueDecision(
        QueueSchedulingDecision decision);
    [[nodiscard]] static QueueExecutionMode
        ParseQueueExecutionMode(std::string_view value);
    void AddPass(
        std::string name,
        std::initializer_list<std::string_view> reads,
        std::initializer_list<std::string_view> writes,
        ExecuteCallback execute,
        PassOptions options = {});
    void AddContextPass(
        std::string name,
        std::initializer_list<std::string_view> reads,
        std::initializer_list<std::string_view> writes,
        ContextExecuteCallback execute,
        PassOptions options = {});
    void AddResourceContextPass(
        std::string name,
        std::initializer_list<ResourceAccess> reads,
        std::initializer_list<ResourceAccess> writes,
        ContextExecuteCallback execute,
        PassOptions options = {});
    [[nodiscard]] RenderGraphPassParameters
        CreatePassParameters() const;
    void AddParameterPass(
        std::string name,
        RenderGraphPassParameters parameters,
        ParameterExecuteCallback execute,
        PassOptions options = {});
    void Compile();
    void Execute(const MarkerCallback& beginMarker = {}, const MarkerCallback& endMarker = {});
    void Execute(
        RHI::ICommandContext& commandContext,
        const MarkerCallback& beginMarker = {},
        const MarkerCallback& endMarker = {});
    void Execute(
        RHI::ICommandContext& commandContext,
        Core::ITaskExecutor& taskExecutor,
        const MarkerCallback& beginMarker = {},
        const MarkerCallback& endMarker = {});

    float GetCpuMilliseconds(std::string_view passName) const;
    const std::vector<PassInfo>& GetPassInfos() const;
    [[nodiscard]] std::vector<ResourceDescription> GetResourceDescriptions() const;
    [[nodiscard]] std::vector<PassDescription> GetPassDescriptions() const;
    [[nodiscard]] const std::vector<QueueSyncDescription>&
        GetQueueSyncDescriptions() const;
    [[nodiscard]] const std::vector<QueueBatchDescription>&
        GetQueueBatchDescriptions() const;
    [[nodiscard]] const CompilationSummary&
        GetCompilationSummary() const;
    [[nodiscard]] const std::string&
        GetGraphSignature() const;
    [[nodiscard]] QueueSchedulingGraphProfile
        GetQueueSchedulingProfile() const;
    [[nodiscard]] RHI::ITexture& ResolveTexture(
        TextureHandle handle) const;
    [[nodiscard]] RHI::IBuffer& ResolveBuffer(
        BufferHandle handle) const;
    [[nodiscard]] RHI::ITextureView&
        ResolveTextureView(
            TextureViewHandle handle) const;
    [[nodiscard]] RenderGraphBlackboard&
        GetBlackboard();
    [[nodiscard]] const RenderGraphBlackboard&
        GetBlackboard() const;
    [[nodiscard]] std::uint32_t GetResourceGeneration() const noexcept
    {
        return m_resourceGeneration;
    }

private:
    using TrackedResourceAccess = GraphTrackedResourceAccess;
    using Pass = GraphPassDeclaration;
    using TextureResource = GraphTextureExecutionResource;
    using BufferStateRange = GraphBufferStateRange;
    using BufferResource = GraphBufferExecutionResource;
    using TransientTextureResource = GraphTransientTextureDeclaration;
    using TransientBufferResource = GraphTransientBufferDeclaration;
    using RegisteredTexture = GraphRegisteredTexture;
    using RegisteredBuffer = GraphRegisteredBuffer;
    using RegisteredTextureView = GraphRegisteredTextureView;
    using CachedPassCompilation = GraphCompiledPass;
    using CompilationCacheEntry = CompiledGraph;

    void InvalidateCompilation();
    [[nodiscard]] std::uint64_t
        BuildDeclarationFingerprint() const;
    [[nodiscard]] bool TryRestoreCompilationCache(
        std::uint64_t declarationFingerprint);
    void StoreCompilationCache(
        std::uint64_t declarationFingerprint);
    void BuildGraphSignature();
    TextureHandle RegisterTextureHandle(
        std::string_view name,
        RHI::ITexture* texture,
        const RHI::TextureDescription& description,
        RenderGraphResourceLifetime lifetime);
    BufferHandle RegisterBufferHandle(
        std::string_view name,
        RHI::IBuffer* buffer,
        const RHI::BufferDescription& description,
        RenderGraphResourceLifetime lifetime);
    const RegisteredTexture& ValidateTextureHandle(
        TextureHandle handle,
        bool requireCurrentVersion) const;
    RegisteredTexture& ValidateTextureHandle(
        TextureHandle handle,
        bool requireCurrentVersion);
    const RegisteredBuffer& ValidateBufferHandle(
        BufferHandle handle,
        bool requireCurrentVersion) const;
    RegisteredBuffer& ValidateBufferHandle(
        BufferHandle handle,
        bool requireCurrentVersion);

    GraphDescription m_description;
    GraphExecutionState m_executionState;
    CompiledGraph m_compiledGraph;

    // Transitional declaration aliases keep the public graph-building facade
    // stable. Compiled and per-execution data are accessed through their
    // explicit owners so cached plans cannot masquerade as live frame state.
    std::unordered_set<std::string>& m_importedResources =
        m_description.importedResources;
    std::unordered_set<std::string>& m_outputResources =
        m_description.outputResources;
    std::unordered_map<std::string, TextureResource>& m_textureResources =
        m_executionState.textureResources;
    std::unordered_map<std::string, BufferResource>& m_bufferResources =
        m_executionState.bufferResources;
    std::unordered_map<std::string, TransientTextureResource>&
        m_transientTextures = m_description.transientTextures;
    std::unordered_map<std::string, TransientBufferResource>&
        m_transientBuffers = m_description.transientBuffers;
    std::vector<RegisteredTexture>& m_registeredTextures =
        m_description.registeredTextures;
    std::vector<RegisteredBuffer>& m_registeredBuffers =
        m_description.registeredBuffers;
    std::vector<RegisteredTextureView>& m_registeredTextureViews =
        m_description.registeredTextureViews;
    std::unordered_map<std::string, std::uint32_t>& m_textureHandleByName =
        m_description.textureHandleByName;
    std::unordered_map<std::string, std::uint32_t>& m_bufferHandleByName =
        m_description.bufferHandleByName;
    std::unordered_map<std::string, std::uint32_t>& m_textureViewHandleByName =
        m_description.textureViewHandleByName;
    std::uint32_t& m_resourceGeneration = m_description.resourceGeneration;
    std::vector<Pass>& m_passes = m_description.passes;
    bool& m_passCullingEnabled = m_description.passCullingEnabled;
    QueueExecutionMode& m_queueExecutionMode =
        m_description.queueExecutionMode;
    QueueSchedulingDecision& m_automaticQueueDecision =
        m_description.automaticQueueDecision;
    RenderGraphBlackboard& m_blackboard = m_description.blackboard;

    bool m_detailedProfilingEnabled = false;
    std::optional<CompilationCacheEntry>
        m_compilationCache;
};
} // namespace Prism::Renderer
