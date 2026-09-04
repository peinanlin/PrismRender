#include "Renderer/RenderGraph.h"

#include "Core/Assert.h"
#include "Core/CpuTrace.h"
#include "RHI/DeferredCommandContext.h"
#include "RHI/ICommandContext.h"
#include "Renderer/Graph/GraphExecutor.h"
#include "Renderer/Graph/ResourceStateTracker.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Prism::Renderer
{
namespace
{
constexpr std::size_t InvalidIndex =
    std::numeric_limits<std::size_t>::max();

RHI::CommandQueueType ToCommandQueueType(
    const RenderGraph::QueueClass queue)
{
    return queue == RenderGraph::QueueClass::Compute
        ? RHI::CommandQueueType::Compute
        : RHI::CommandQueueType::Graphics;
}

} // namespace

void RenderGraph::Reset()
{
    ++m_resourceGeneration;
    if (m_resourceGeneration == 0)
    {
        m_resourceGeneration = 1;
    }
    m_importedResources.clear();
    m_outputResources.clear();
    m_textureResources.clear();
    m_bufferResources.clear();
    m_transientTextures.clear();
    m_transientBuffers.clear();
    m_registeredTextures.clear();
    m_registeredBuffers.clear();
    m_registeredTextureViews.clear();
    m_textureHandleByName.clear();
    m_bufferHandleByName.clear();
    m_textureViewHandleByName.clear();
    m_blackboard.Clear();
    m_passes.clear();
    m_automaticQueueDecision = {};
    m_compiledGraph.Clear();
    m_executionState.InvalidateCompiledPlan();
}

TextureHandle RenderGraph::DeclareTexture(
    std::string resourceName,
    RHI::ITexture& texture,
    const RHI::ResourceState initialState)
{
    InvalidateCompilation();
    Core::Check(!resourceName.empty(), "Render graph texture names must not be empty.");
    const std::string key = resourceName;
    texture.SetDebugName(key);
    const RHI::TextureDescription& description =
        texture.GetDescription();
    Core::Check(m_textureResources.emplace(
                    std::move(resourceName),
                    TextureResource{
                        &texture,
                        initialState,
                        std::vector<RHI::ResourceState>(
                            static_cast<std::size_t>(
                                description.mipLevels)
                                * description.arrayLayers,
                            initialState)}).second,
                "A render graph texture was declared more than once.");
    Core::Check(!m_importedResources.contains(key),
                "A render graph texture name conflicts with an imported logical resource.");
    return RegisterTextureHandle(
        key,
        &texture,
        description,
        RenderGraphResourceLifetime::Persistent);
}

TextureHandle RenderGraph::ImportTexture(
    std::string resourceName,
    RHI::ITexture& texture,
    const RHI::ResourceState initialState)
{
    InvalidateCompilation();
    const std::string key = resourceName;
    TextureHandle handle = DeclareTexture(
        std::move(resourceName),
        texture,
        initialState);
    m_importedResources.emplace(key);
    m_registeredTextures[handle.index].lifetime =
        RenderGraphResourceLifetime::External;
    return handle;
}

void RenderGraph::ImportResource(std::string resourceName)
{
    InvalidateCompilation();
    Core::Check(
        !resourceName.empty(),
        "Render graph resource names must not be empty.");
    Core::Check(
        !m_transientTextures.contains(resourceName),
        "An imported resource conflicts with a transient texture.");
    Core::Check(
        !m_bufferResources.contains(resourceName),
        "An imported logical resource conflicts with a buffer.");
    m_importedResources.emplace(std::move(resourceName));
}

TextureHandle RenderGraph::DeclareTransientTexture(
    std::string resourceName,
    const RHI::TextureDescription& description,
    const RHI::ResourceState initialState)
{
    InvalidateCompilation();
    Core::Check(
        !resourceName.empty(),
        "Render graph transient texture names must not be empty.");
    std::string validationError;
    Core::Check(
        RHI::ValidateTextureDescription(
            description,
            &validationError),
        validationError.c_str());
    Core::Check(
        !m_importedResources.contains(resourceName)
            && !m_textureResources.contains(resourceName),
        "A transient texture conflicts with an existing resource.");
    const std::string key = resourceName;
    Core::Check(
        m_transientTextures.emplace(
            std::move(resourceName),
            TransientTextureResource{
                description,
                initialState}).second,
        "A transient texture was declared more than once.");
    return RegisterTextureHandle(
        key,
        nullptr,
        description,
        RenderGraphResourceLifetime::Transient);
}

TextureHandle RenderGraph::DeclareTransientTexture(
    std::string resourceName,
    RHI::ITexture& texture,
    const RHI::ResourceState initialState)
{
    InvalidateCompilation();
    Core::Check(
        !resourceName.empty(),
        "Render graph transient texture names must not be empty.");
    const std::string key = resourceName;
    const RHI::TextureDescription& description =
        texture.GetDescription();
    Core::Check(
        !m_importedResources.contains(key),
        "An imported resource cannot also be transient.");
    Core::Check(
        m_textureResources.emplace(
            key,
            TextureResource{
                &texture,
                initialState,
                std::vector<RHI::ResourceState>(
                    static_cast<std::size_t>(
                        description.mipLevels)
                        * description.arrayLayers,
                    initialState)}).second,
        "A render graph texture was declared more than once.");
    Core::Check(
        m_transientTextures.emplace(
            std::move(resourceName),
            TransientTextureResource{
                description,
                initialState}).second,
        "A transient texture was declared more than once.");
    return RegisterTextureHandle(
        key,
        &texture,
        description,
        RenderGraphResourceLifetime::Transient);
}

TextureHandle RenderGraph::RegisterTextureHandle(
    const std::string_view name,
    RHI::ITexture* texture,
    const RHI::TextureDescription& description,
    const RenderGraphResourceLifetime lifetime)
{
    if (texture) texture->SetDebugName(name);
    Core::Check(
        !m_textureHandleByName.contains(
            std::string(name))
            && !m_bufferHandleByName.contains(
                std::string(name)),
        "A RenderGraph resource handle name was registered more than once.");
    Core::Check(
        m_registeredTextures.size()
            < InvalidRenderGraphIndex,
        "The RenderGraph texture handle table is full.");
    const std::uint32_t index =
        static_cast<std::uint32_t>(
            m_registeredTextures.size());
    m_registeredTextures.push_back({
        std::string(name),
        m_resourceGeneration,
        0,
        lifetime,
        texture,
        description});
    m_textureHandleByName.emplace(
        std::string(name),
        index);
    return {index, m_resourceGeneration, 0};
}

BufferHandle RenderGraph::RegisterBufferHandle(
    const std::string_view name,
    RHI::IBuffer* buffer,
    const RHI::BufferDescription& description,
    const RenderGraphResourceLifetime lifetime)
{
    Core::Check(
        !m_bufferHandleByName.contains(
            std::string(name))
            && !m_textureHandleByName.contains(
                std::string(name)),
        "A RenderGraph resource handle name was registered more than once.");
    Core::Check(
        m_registeredBuffers.size()
            < InvalidRenderGraphIndex,
        "The RenderGraph buffer handle table is full.");
    const std::uint32_t index =
        static_cast<std::uint32_t>(
            m_registeredBuffers.size());
    m_registeredBuffers.push_back({
        std::string(name),
        m_resourceGeneration,
        0,
        lifetime,
        buffer,
        description});
    m_bufferHandleByName.emplace(
        std::string(name),
        index);
    return {index, m_resourceGeneration, 0};
}

BufferHandle RenderGraph::DeclareBuffer(
    std::string resourceName,
    RHI::IBuffer& buffer,
    const RHI::ResourceState initialState)
{
    InvalidateCompilation();
    Core::Check(
        !resourceName.empty(),
        "RenderGraph buffer names must not be empty.");
    const std::string key = resourceName;
    buffer.SetDebugName(key);
    const RHI::BufferDescription& description =
        buffer.GetDescription();
    Core::Check(
        m_bufferResources.emplace(
            std::move(resourceName),
            BufferResource{
                &buffer,
                description,
                {{0, description.size, initialState}}})
            .second,
        "A RenderGraph buffer was declared more than once.");
    Core::Check(
        !m_importedResources.contains(key)
            && !m_transientBuffers.contains(key),
        "A RenderGraph buffer conflicts with an existing resource.");
    return RegisterBufferHandle(
        key,
        &buffer,
        description,
        RenderGraphResourceLifetime::Persistent);
}

BufferHandle RenderGraph::ImportBuffer(
    std::string resourceName,
    RHI::IBuffer& buffer,
    const RHI::ResourceState initialState)
{
    const std::string key = resourceName;
    BufferHandle handle = DeclareBuffer(
        std::move(resourceName),
        buffer,
        initialState);
    m_importedResources.emplace(key);
    m_registeredBuffers[handle.index].lifetime =
        RenderGraphResourceLifetime::External;
    return handle;
}

BufferHandle RenderGraph::DeclareTransientBuffer(
    std::string resourceName,
    const RHI::BufferDescription& description,
    const RHI::ResourceState initialState)
{
    InvalidateCompilation();
    Core::Check(
        !resourceName.empty(),
        "RenderGraph transient buffer names must not be empty.");
    std::string validationError;
    const void* validationInitialData =
        description.memoryAccess
                == RHI::MemoryAccess::GpuOnly
            ? &description
            : nullptr;
    const bool validDescription =
        RHI::ValidateBufferDescription(
            description,
            validationInitialData,
            &validationError);
    Core::Check(
        validDescription,
        validationError.c_str());
    Core::Check(
        !m_importedResources.contains(resourceName)
            && !m_textureResources.contains(resourceName)
            && !m_bufferResources.contains(resourceName),
        "A transient buffer conflicts with an existing resource.");
    const std::string key = resourceName;
    Core::Check(
        m_transientBuffers.emplace(
            std::move(resourceName),
            TransientBufferResource{
                description,
                initialState}).second,
        "A transient buffer was declared more than once.");
    return RegisterBufferHandle(
        key,
        nullptr,
        description,
        RenderGraphResourceLifetime::Transient);
}

BufferHandle RenderGraph::DeclareTransientBuffer(
    std::string resourceName,
    RHI::IBuffer& buffer,
    const RHI::ResourceState initialState)
{
    InvalidateCompilation();
    Core::Check(
        !resourceName.empty(),
        "RenderGraph transient buffer names must not be empty.");
    const std::string key = resourceName;
    const RHI::BufferDescription& description =
        buffer.GetDescription();
    Core::Check(
        !m_importedResources.contains(key)
            && !m_textureResources.contains(key),
        "A transient buffer conflicts with an existing resource.");
    Core::Check(
        m_bufferResources.emplace(
            key,
            BufferResource{
                &buffer,
                description,
                {{0, description.size, initialState}}})
            .second,
        "A RenderGraph buffer was declared more than once.");
    Core::Check(
        m_transientBuffers.emplace(
            std::move(resourceName),
            TransientBufferResource{
                description,
                initialState}).second,
        "A transient buffer was declared more than once.");
    return RegisterBufferHandle(
        key,
        &buffer,
        description,
        RenderGraphResourceLifetime::Transient);
}

TextureViewHandle RenderGraph::DeclareTextureView(
    std::string viewName,
    const TextureHandle texture,
    RHI::ITextureView& view)
{
    InvalidateCompilation();
    Core::Check(
        !viewName.empty(),
        "RenderGraph texture-view names must not be empty.");
    ValidateTextureHandle(texture, true);
    Core::Check(
        !m_textureViewHandleByName.contains(viewName),
        "A RenderGraph texture view was declared more than once.");
    const std::uint32_t index =
        static_cast<std::uint32_t>(
            m_registeredTextureViews.size());
    m_registeredTextureViews.push_back({
        viewName,
        m_resourceGeneration,
        texture,
        &view});
    m_textureViewHandleByName.emplace(
        std::move(viewName),
        index);
    return {
        index,
        m_resourceGeneration,
        texture};
}

TextureHistoryHandle RenderGraph::ImportTextureHistory(
    std::string resourceName,
    RHI::ITexture& previous,
    const RHI::ResourceState previousState,
    RHI::ITexture& current,
    const RHI::ResourceState currentState)
{
    Core::Check(
        !resourceName.empty(),
        "RenderGraph history names must not be empty.");
    TextureHistoryHandle result{};
    result.previous = ImportTexture(
        resourceName + ".Previous",
        previous,
        previousState);
    result.current = ImportTexture(
        resourceName + ".Current",
        current,
        currentState);
    m_registeredTextures[result.previous.index]
        .lifetime = RenderGraphResourceLifetime::History;
    m_registeredTextures[result.current.index]
        .lifetime = RenderGraphResourceLifetime::History;
    return result;
}

void RenderGraph::MarkOutput(std::string resourceName)
{
    InvalidateCompilation();
    Core::Check(
        !resourceName.empty(),
        "Render graph output names must not be empty.");
    m_outputResources.emplace(std::move(resourceName));
}

void RenderGraph::MarkOutput(
    const TextureHandle handle)
{
    const RegisteredTexture& resource =
        ValidateTextureHandle(handle, true);
    MarkOutput(resource.name);
}

void RenderGraph::MarkOutput(
    const BufferHandle handle)
{
    const RegisteredBuffer& resource =
        ValidateBufferHandle(handle, true);
    MarkOutput(resource.name);
}

void RenderGraph::SetPassCullingEnabled(
    const bool enabled)
{
    InvalidateCompilation();
    m_passCullingEnabled = enabled;
}

void RenderGraph::SetQueueExecutionMode(
    const QueueExecutionMode mode)
{
    m_queueExecutionMode = mode;
}

void RenderGraph::SetAutomaticQueueDecision(
    QueueSchedulingDecision decision)
{
    m_automaticQueueDecision =
        std::move(decision);
}

RenderGraph::QueueExecutionMode
RenderGraph::ParseQueueExecutionMode(
    const std::string_view value)
{
    if (value.empty() || value == "auto")
    {
        return QueueExecutionMode::Automatic;
    }
    if (value == "serial")
    {
        return QueueExecutionMode::Serial;
    }
    if (value == "native")
    {
        return QueueExecutionMode::Native;
    }
    throw std::invalid_argument(
        "RenderGraph queue execution mode must be auto, serial, or native.");
}

void RenderGraph::AddPass(
    std::string name,
    const std::initializer_list<std::string_view> reads,
    const std::initializer_list<std::string_view> writes,
    ExecuteCallback execute,
    const PassOptions options)
{
    InvalidateCompilation();
    Core::Check(!name.empty(), "Render graph pass names must not be empty.");
    Core::Check(static_cast<bool>(execute), "Render graph passes require an execute callback.");

    Pass pass{};
    pass.info.name = std::move(name);
    pass.execute = std::move(execute);
    pass.options = options;
    pass.originalIndex = m_passes.size();
    for (const std::string_view resource : reads)
    {
        pass.reads.emplace_back(resource);
    }
    for (const std::string_view resource : writes)
    {
        pass.writes.emplace_back(resource);
    }
    m_passes.push_back(std::move(pass));
}

void RenderGraph::AddContextPass(
    std::string name,
    const std::initializer_list<std::string_view> reads,
    const std::initializer_list<std::string_view> writes,
    ContextExecuteCallback execute,
    const PassOptions options)
{
    InvalidateCompilation();
    Core::Check(!name.empty(), "Render graph pass names must not be empty.");
    Core::Check(static_cast<bool>(execute), "Render graph context passes require an execute callback.");

    Pass pass{};
    pass.info.name = std::move(name);
    pass.contextExecute = std::move(execute);
    pass.options = options;
    pass.originalIndex = m_passes.size();
    for (const std::string_view resource : reads)
    {
        pass.reads.emplace_back(resource);
    }
    for (const std::string_view resource : writes)
    {
        pass.writes.emplace_back(resource);
    }
    m_passes.push_back(std::move(pass));
}

void RenderGraph::AddResourceContextPass(
    std::string name,
    const std::initializer_list<ResourceAccess> reads,
    const std::initializer_list<ResourceAccess> writes,
    ContextExecuteCallback execute,
    const PassOptions options)
{
    InvalidateCompilation();
    Core::Check(!name.empty(), "Render graph pass names must not be empty.");
    Core::Check(static_cast<bool>(execute), "Render graph context passes require an execute callback.");

    Pass pass{};
    pass.info.name = std::move(name);
    pass.contextExecute = std::move(execute);
    pass.options = options;
    pass.originalIndex = m_passes.size();
    for (const ResourceAccess& resource : reads)
    {
        Core::Check(!resource.name.empty(), "Render graph resource names must not be empty.");
        pass.reads.emplace_back(resource.name);
        pass.resourceReads.push_back({std::string(resource.name), resource.state});
    }
    for (const ResourceAccess& resource : writes)
    {
        Core::Check(!resource.name.empty(), "Render graph resource names must not be empty.");
        pass.writes.emplace_back(resource.name);
        pass.resourceWrites.push_back({std::string(resource.name), resource.state});
    }
    m_passes.push_back(std::move(pass));
}

RenderGraphPassParameters
RenderGraph::CreatePassParameters() const
{
    return {};
}

void RenderGraph::AddParameterPass(
    std::string name,
    RenderGraphPassParameters parameters,
    ParameterExecuteCallback execute,
    const PassOptions options)
{
    InvalidateCompilation();
    Core::Check(
        !name.empty(),
        "RenderGraph parameter passes require a name.");
    Core::Check(
        static_cast<bool>(execute),
        "RenderGraph parameter passes require an execute callback.");

    Pass pass{};
    pass.info.name = std::move(name);
    pass.parameterExecute = std::move(execute);
    pass.contextExecute =
        [this,
         parameterExecute = pass.parameterExecute,
         declaredParameters = parameters](
            RHI::ICommandContext& commandContext)
        {
            const RenderGraphPassResources resources(
                *this,
                declaredParameters);
            for (const TextureParameterAccess& access :
                 declaredParameters.GetTextureAccesses())
            {
                (void)resources.GetTexture(
                    access.handle);
            }
            for (const BufferParameterAccess& access :
                 declaredParameters.GetBufferAccesses())
            {
                (void)resources.GetBuffer(
                    access.handle);
            }
            for (const TextureViewParameterAccess& access :
                 declaredParameters
                     .GetTextureViewAccesses())
            {
                (void)resources.GetTextureView(
                    access.handle);
            }
            parameterExecute(
                commandContext,
                resources);
        };
    pass.parameters = parameters;
    pass.options = options;
    pass.originalIndex = m_passes.size();

    std::vector<std::uint32_t> textureVersions;
    textureVersions.reserve(m_registeredTextures.size());
    for (const RegisteredTexture& texture :
         m_registeredTextures)
    {
        textureVersions.push_back(
            texture.currentVersion);
    }
    std::vector<std::uint32_t> bufferVersions;
    bufferVersions.reserve(m_registeredBuffers.size());
    for (const RegisteredBuffer& buffer :
         m_registeredBuffers)
    {
        bufferVersions.push_back(
            buffer.currentVersion);
    }

    try
    {
    const auto addTextureAccess =
        [&](const TextureParameterAccess& access)
        {
            RegisteredTexture& resource =
                ValidateTextureHandle(
                    access.handle,
                    false);
            Core::Check(
                access.range.baseMipLevel
                        < resource.description.mipLevels
                    && access.range.baseArrayLayer
                        < resource.description.arrayLayers,
                "A RenderGraph texture parameter starts outside its subresources.");
            const std::uint32_t mipCount =
                access.range.mipLevelCount == 0
                ? resource.description.mipLevels
                      - access.range.baseMipLevel
                : access.range.mipLevelCount;
            const std::uint32_t layerCount =
                access.range.arrayLayerCount == 0
                ? resource.description.arrayLayers
                      - access.range.baseArrayLayer
                : access.range.arrayLayerCount;
            Core::Check(
                access.range.baseMipLevel
                            + mipCount
                        <= resource.description.mipLevels
                    && access.range.baseArrayLayer
                            + layerCount
                        <= resource.description.arrayLayers,
                "A RenderGraph texture parameter range is invalid.");
            if (access.mode
                == RenderGraphAccessMode::Read)
            {
                Core::Check(
                    access.handle.version
                        == resource.currentVersion,
                    "A RenderGraph texture pass reads a stale resource version.");
                pass.reads.push_back(resource.name);
                pass.resourceReads.push_back({
                    resource.name,
                    access.state,
                    true,
                    false,
                    access.handle.version,
                    access.range,
                    {}});
            }
            else
            {
                Core::Check(
                    access.handle.version
                        == resource.currentVersion + 1,
                    "A RenderGraph texture write does not produce the next version.");
                resource.currentVersion =
                    access.handle.version;
                pass.writes.push_back(resource.name);
                pass.resourceWrites.push_back({
                    resource.name,
                    access.state,
                    true,
                    false,
                    access.handle.version,
                    access.range,
                    {}});
            }
        };
    for (const TextureParameterAccess& access :
         parameters.GetTextureAccesses())
    {
        addTextureAccess(access);
    }

    for (const BufferParameterAccess& access :
         parameters.GetBufferAccesses())
    {
        RegisteredBuffer& resource =
            ValidateBufferHandle(
                access.handle,
                false);
        Core::Check(
            access.range.offset
                <= resource.description.size,
            "A RenderGraph buffer parameter starts outside its allocation.");
        const std::size_t rangeSize =
            access.range.size == 0
            ? resource.description.size
                  - access.range.offset
            : access.range.size;
        Core::Check(
            access.range.offset + rangeSize
                    <= resource.description.size,
            "A RenderGraph buffer parameter range is invalid.");
        if (access.mode
            == RenderGraphAccessMode::Read)
        {
            Core::Check(
                access.handle.version
                    == resource.currentVersion,
                "A RenderGraph buffer pass reads a stale resource version.");
            pass.reads.push_back(resource.name);
            pass.resourceReads.push_back({
                resource.name,
                access.state,
                false,
                true,
                access.handle.version,
                {},
                access.range});
        }
        else
        {
            Core::Check(
                access.handle.version
                    == resource.currentVersion + 1,
                "A RenderGraph buffer write does not produce the next version.");
            resource.currentVersion =
                access.handle.version;
            pass.writes.push_back(resource.name);
            pass.resourceWrites.push_back({
                resource.name,
                access.state,
                false,
                true,
                access.handle.version,
                {},
                access.range});
        }
    }

    for (const TextureViewParameterAccess& access :
         parameters.GetTextureViewAccesses())
    {
        Core::Check(
            access.handle.index
                    < m_registeredTextureViews.size()
                && access.handle.generation
                    == m_resourceGeneration,
            "A RenderGraph texture-view pass uses an invalid handle.");
        const RegisteredTextureView& view =
            m_registeredTextureViews[
                access.handle.index];
        Core::Check(
            view.generation == access.handle.generation,
            "A RenderGraph texture-view handle is stale.");
        const RHI::TextureViewDescription&
            viewDescription =
                view.view->GetDescription();
        TextureParameterAccess textureAccess{};
        textureAccess.handle =
            access.handle.texture;
        textureAccess.state = access.state;
        textureAccess.range = {
            viewDescription.baseMipLevel,
            viewDescription.mipLevelCount,
            viewDescription.baseArrayLayer,
            viewDescription.arrayLayerCount};
        textureAccess.mode = access.mode;
        addTextureAccess(textureAccess);
    }
    }
    catch (...)
    {
        for (std::size_t index = 0;
             index < textureVersions.size();
             ++index)
        {
            m_registeredTextures[index]
                .currentVersion =
                    textureVersions[index];
        }
        for (std::size_t index = 0;
             index < bufferVersions.size();
             ++index)
        {
            m_registeredBuffers[index]
                .currentVersion =
                    bufferVersions[index];
        }
        throw;
    }

    m_passes.push_back(std::move(pass));
}

void RenderGraph::Execute(
    const MarkerCallback& beginMarker,
    const MarkerCallback& endMarker)
{
    Compile();
    GraphExecutor::Execute(
        m_description,
        m_executionState,
        m_compiledGraph,
        m_detailedProfilingEnabled,
        beginMarker,
        endMarker);
}

void RenderGraph::Execute(
    RHI::ICommandContext& commandContext,
    const MarkerCallback& beginMarker,
    const MarkerCallback& endMarker)
{
    Compile();
    GraphExecutor::Execute(
        m_description,
        m_executionState,
        m_compiledGraph,
        m_detailedProfilingEnabled,
        commandContext,
        beginMarker,
        endMarker);
}

void RenderGraph::Execute(
    RHI::ICommandContext& commandContext,
    Core::ITaskExecutor& taskExecutor,
    const MarkerCallback& beginMarker,
    const MarkerCallback& endMarker)
{
    Compile();
    GraphExecutor::Execute(
        m_description,
        m_executionState,
        m_compiledGraph,
        m_detailedProfilingEnabled,
        commandContext,
        taskExecutor,
        beginMarker,
        endMarker);
}

float RenderGraph::GetCpuMilliseconds(const std::string_view passName) const
{
    for (const PassInfo& passInfo :
         m_executionState.passInfos)
    {
        if (passInfo.name == passName)
        {
            return passInfo.cpuMilliseconds;
        }
    }
    return 0.0f;
}

const std::vector<RenderGraph::PassInfo>& RenderGraph::GetPassInfos() const
{
    return m_executionState.passInfos;
}

std::vector<RenderGraph::ResourceDescription> RenderGraph::GetResourceDescriptions() const
{
    std::vector<ResourceDescription> descriptions;
    descriptions.reserve(
        m_compiledGraph.compiledResources.size());
    for (const auto& [name, compiled] :
         m_compiledGraph.compiledResources)
    {
        RHI::ResourceState state = compiled.state;
        const auto texture = m_textureResources.find(name);
        if (texture != m_textureResources.end())
        {
            state = texture->second.state;
        }
        const auto buffer = m_bufferResources.find(name);
        if (buffer != m_bufferResources.end()
            && !buffer->second.states.empty())
        {
            state = buffer->second.states.front().state;
        }
        descriptions.push_back({
            name,
            state,
            compiled.texture,
            compiled.buffer,
            compiled.imported,
            compiled.transient,
            compiled.history,
            compiled.currentVersion,
            texture != m_textureResources.end()
                ? texture->second
                      .subresourceStates.size()
                : 0,
            buffer != m_bufferResources.end()
                ? buffer->second.states.size()
                : 0,
            compiled.active,
            compiled.firstUse,
            compiled.lastUse,
            compiled.physicalAllocation,
            compiled.estimatedBytes,
            compiled.nativePoolId,
            compiled.nativeAllocation,
            compiled.nativeAllocationBytes});
    }
    std::ranges::sort(descriptions, {}, &ResourceDescription::name);
    return descriptions;
}

std::vector<RenderGraph::PassDescription> RenderGraph::GetPassDescriptions() const
{
    std::vector<PassDescription> descriptions;
    descriptions.reserve(m_passes.size());
    for (const Pass& pass : m_passes)
    {
        PassDescription description{};
        description.name = pass.info.name;
        description.cpuMilliseconds = pass.info.cpuMilliseconds;
        description.originalIndex = pass.originalIndex;
        description.executionIndex = pass.executionIndex;
        description.culled = !pass.active;
        description.cullReason = pass.cullReason;
        description.sideEffect = pass.options.sideEffect;
        description.parallelRecordable =
            pass.options.parallelRecordable;
        description.parallelRecordingAudited =
            pass.options.parallelRecordingContract.IsSatisfied();
        description.queue = pass.options.queue;
        description.dependencies =
            pass.executionDependencies;
        const auto appendAccesses = [this](
            const std::vector<std::string>& names,
            const std::vector<TrackedResourceAccess>& tracked,
            std::vector<PassDescription::Access>& destination)
        {
            destination.reserve(names.size());
            std::size_t trackedIndex = 0;
            for (const std::string& name : names)
            {
                destination.push_back({
                    name,
                    RHI::ResourceState::Undefined});
                if (trackedIndex < tracked.size()
                    && tracked[trackedIndex].name == name)
                {
                    const TrackedResourceAccess& access =
                        tracked[trackedIndex++];
                    destination.back().state =
                        access.state;
                    destination.back().kind =
                        (access.texture
                         || m_textureResources
                                .contains(access.name))
                        ? "texture"
                        : (access.buffer
                           || m_bufferResources
                                  .contains(access.name))
                            ? "buffer"
                            : "logical";
                    destination.back().version =
                        access.version;
                    destination.back().textureRange =
                        access.textureRange;
                    destination.back().bufferRange =
                        access.bufferRange;
                }
            }
        };
        appendAccesses(pass.reads, pass.resourceReads, description.reads);
        appendAccesses(pass.writes, pass.resourceWrites, description.writes);
        descriptions.push_back(std::move(description));
    }
    return descriptions;
}

const std::vector<RenderGraph::QueueSyncDescription>&
RenderGraph::GetQueueSyncDescriptions() const
{
    return m_compiledGraph.queueSyncDescriptions;
}

const std::vector<RenderGraph::QueueBatchDescription>&
RenderGraph::GetQueueBatchDescriptions() const
{
    return m_compiledGraph.queueBatchDescriptions;
}

const RenderGraph::CompilationSummary&
RenderGraph::GetCompilationSummary() const
{
    return m_executionState.GetVisibleSummary(
        m_compiledGraph.summary);
}

const std::string&
RenderGraph::GetGraphSignature() const
{
    return m_compiledGraph.graphSignature;
}

QueueSchedulingGraphProfile
RenderGraph::GetQueueSchedulingProfile() const
{
    QueueSchedulingGraphProfile profile{};
    profile.graphSignature =
        m_compiledGraph.graphSignature;
    profile.overlapOpportunityCount =
        m_compiledGraph.summary
            .overlapOpportunityCount;

    std::vector<std::size_t> activePasses;
    activePasses.reserve(m_passes.size());
    for (std::size_t passIndex = 0;
         passIndex < m_passes.size();
         ++passIndex)
    {
        if (m_passes[passIndex].active)
        {
            activePasses.push_back(passIndex);
        }
    }
    std::ranges::sort(
        activePasses,
        [&](const std::size_t left,
            const std::size_t right)
        {
            return m_passes[left].executionIndex
                < m_passes[right].executionIndex;
        });

    std::unordered_map<std::size_t, std::size_t>
        profileIndices;
    for (std::size_t profileIndex = 0;
         profileIndex < activePasses.size();
         ++profileIndex)
    {
        profileIndices.emplace(
            activePasses[profileIndex],
            profileIndex);
    }

    profile.passes.reserve(activePasses.size());
    for (const std::size_t passIndex :
         activePasses)
    {
        const Pass& pass = m_passes[passIndex];
        QueueSchedulingPassProfile passProfile{};
        passProfile.name = pass.info.name;
        passProfile.queue =
            ToCommandQueueType(pass.options.queue);
        for (const std::size_t dependency :
             pass.executionDependencies)
        {
            const auto found =
                profileIndices.find(dependency);
            if (found != profileIndices.end())
            {
                passProfile.dependencies.push_back(
                    found->second);
            }
        }
        profile.passes.push_back(
            std::move(passProfile));
    }
    return profile;
}

const RenderGraph::RegisteredTexture&
RenderGraph::ValidateTextureHandle(
    const TextureHandle handle,
    const bool requireCurrentVersion) const
{
    Core::Check(
        handle.IsValid()
            && handle.index
                < m_registeredTextures.size()
            && handle.generation
                == m_resourceGeneration,
        "A RenderGraph texture handle is invalid or belongs to an older graph generation.");
    const RegisteredTexture& resource =
        m_registeredTextures[handle.index];
    Core::Check(
        resource.generation == handle.generation,
        "A RenderGraph texture handle is stale.");
    if (requireCurrentVersion)
    {
        Core::Check(
            resource.currentVersion
                == handle.version,
            "A RenderGraph texture handle does not reference the current version.");
    }
    return resource;
}

RenderGraph::RegisteredTexture&
RenderGraph::ValidateTextureHandle(
    const TextureHandle handle,
    const bool requireCurrentVersion)
{
    return const_cast<RegisteredTexture&>(
        std::as_const(*this)
            .ValidateTextureHandle(
                handle,
                requireCurrentVersion));
}

const RenderGraph::RegisteredBuffer&
RenderGraph::ValidateBufferHandle(
    const BufferHandle handle,
    const bool requireCurrentVersion) const
{
    Core::Check(
        handle.IsValid()
            && handle.index
                < m_registeredBuffers.size()
            && handle.generation
                == m_resourceGeneration,
        "A RenderGraph buffer handle is invalid or belongs to an older graph generation.");
    const RegisteredBuffer& resource =
        m_registeredBuffers[handle.index];
    Core::Check(
        resource.generation == handle.generation,
        "A RenderGraph buffer handle is stale.");
    if (requireCurrentVersion)
    {
        Core::Check(
            resource.currentVersion
                == handle.version,
            "A RenderGraph buffer handle does not reference the current version.");
    }
    return resource;
}

RenderGraph::RegisteredBuffer&
RenderGraph::ValidateBufferHandle(
    const BufferHandle handle,
    const bool requireCurrentVersion)
{
    return const_cast<RegisteredBuffer&>(
        std::as_const(*this)
            .ValidateBufferHandle(
                handle,
                requireCurrentVersion));
}

RHI::ITexture& RenderGraph::ResolveTexture(
    const TextureHandle handle) const
{
    const RegisteredTexture& resource =
        ValidateTextureHandle(handle, false);
    Core::Check(
        resource.texture != nullptr,
        "A RenderGraph texture handle has no realized RHI resource.");
    return *resource.texture;
}

RHI::IBuffer& RenderGraph::ResolveBuffer(
    const BufferHandle handle) const
{
    const RegisteredBuffer& resource =
        ValidateBufferHandle(handle, false);
    Core::Check(
        resource.buffer != nullptr,
        "A RenderGraph buffer handle has no realized RHI resource.");
    return *resource.buffer;
}

RHI::ITextureView&
RenderGraph::ResolveTextureView(
    const TextureViewHandle handle) const
{
    Core::Check(
        handle.IsValid()
            && handle.index
                < m_registeredTextureViews.size()
            && handle.generation
                == m_resourceGeneration,
        "A RenderGraph texture-view handle is invalid or stale.");
    const RegisteredTextureView& resource =
        m_registeredTextureViews[handle.index];
    Core::Check(
        resource.generation == handle.generation
            && resource.view != nullptr,
        "A RenderGraph texture-view registration is invalid.");
    return *resource.view;
}

RenderGraphBlackboard& RenderGraph::GetBlackboard()
{
    return m_blackboard;
}

const RenderGraphBlackboard&
RenderGraph::GetBlackboard() const
{
    return m_blackboard;
}
} // namespace Prism::Renderer
