#pragma once

#include "RHI/GraphicsResources.h"
#include "Renderer/QueueSchedulingCostModel.h"
#include "Renderer/RenderGraphResources.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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

namespace Prism::Renderer
{
enum class GraphQueueClass
{
    Graphics,
    Compute
};

enum class GraphQueueExecutionMode
{
    Automatic,
    Serial,
    Native
};

struct GraphParallelRecordingContract
{
    bool isolatedCommandContext = false;
    bool immutableBindings = false;
    bool noDynamicUploadOrCacheWrites = false;
    bool noFeatureStateWrites = false;

    [[nodiscard]] constexpr bool IsSatisfied() const noexcept
    {
        return isolatedCommandContext
            && immutableBindings
            && noDynamicUploadOrCacheWrites
            && noFeatureStateWrites;
    }

    [[nodiscard]] static constexpr GraphParallelRecordingContract
        AuditedIndependent() noexcept
    {
        return {true, true, true, true};
    }
};

struct GraphPassOptions
{
    GraphQueueClass queue = GraphQueueClass::Graphics;
    bool sideEffect = false;
    bool allowCulling = true;
    bool parallelRecordable = false;
    // This contract is an explicit call-site audit: the callback may only
    // record into its supplied context and read frame-stable bindings/state.
    GraphParallelRecordingContract parallelRecordingContract;
};

struct GraphPassInfo
{
    std::string name;
    float cpuMilliseconds = 0.0f;
};

struct GraphResourceAccess
{
    std::string_view name;
    RHI::ResourceState state;
};

struct GraphTrackedResourceAccess
{
    std::string name;
    RHI::ResourceState state;
    bool texture = false;
    bool buffer = false;
    std::uint32_t version = 0;
    TextureSubresourceRange textureRange;
    BufferRange bufferRange;
};

struct GraphPassDeclaration
{
    using ExecuteCallback = std::function<void()>;
    using ContextExecuteCallback =
        std::function<void(RHI::ICommandContext&)>;
    using ParameterExecuteCallback = std::function<void(
        RHI::ICommandContext&,
        const RenderGraphPassResources&)>;

    GraphPassInfo info;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    ExecuteCallback execute;
    ContextExecuteCallback contextExecute;
    ParameterExecuteCallback parameterExecute;
    RenderGraphPassParameters parameters;
    std::vector<GraphTrackedResourceAccess> resourceReads;
    std::vector<GraphTrackedResourceAccess> resourceWrites;
    GraphPassOptions options;
    std::size_t originalIndex = 0;

    // Compiler annotations retained with the graph declaration while the
    // immutable compiled plan owns its independent execution copy.
    std::size_t executionIndex =
        std::numeric_limits<std::size_t>::max();
    bool active = true;
    std::string cullReason;
    std::vector<std::size_t> livenessDependencies;
    std::vector<std::size_t> executionDependencies;
};

struct GraphTransientTextureDeclaration
{
    RHI::TextureDescription description;
    RHI::ResourceState initialState = RHI::ResourceState::Undefined;
};

struct GraphTransientBufferDeclaration
{
    RHI::BufferDescription description;
    RHI::ResourceState initialState = RHI::ResourceState::Undefined;
};

struct GraphRegisteredTexture
{
    std::string name;
    std::uint32_t generation = 0;
    std::uint32_t currentVersion = 0;
    RenderGraphResourceLifetime lifetime =
        RenderGraphResourceLifetime::Persistent;
    RHI::ITexture* texture = nullptr;
    RHI::TextureDescription description;
};

struct GraphRegisteredBuffer
{
    std::string name;
    std::uint32_t generation = 0;
    std::uint32_t currentVersion = 0;
    RenderGraphResourceLifetime lifetime =
        RenderGraphResourceLifetime::Persistent;
    RHI::IBuffer* buffer = nullptr;
    RHI::BufferDescription description;
};

struct GraphRegisteredTextureView
{
    std::string name;
    std::uint32_t generation = 0;
    TextureHandle texture;
    RHI::ITextureView* view = nullptr;
};

struct GraphDescription
{
    std::unordered_set<std::string> importedResources;
    std::unordered_set<std::string> outputResources;
    std::unordered_map<std::string, GraphTransientTextureDeclaration>
        transientTextures;
    std::unordered_map<std::string, GraphTransientBufferDeclaration>
        transientBuffers;
    std::vector<GraphRegisteredTexture> registeredTextures;
    std::vector<GraphRegisteredBuffer> registeredBuffers;
    std::vector<GraphRegisteredTextureView> registeredTextureViews;
    std::unordered_map<std::string, std::uint32_t> textureHandleByName;
    std::unordered_map<std::string, std::uint32_t> bufferHandleByName;
    std::unordered_map<std::string, std::uint32_t> textureViewHandleByName;
    std::uint32_t resourceGeneration = 1;
    std::vector<GraphPassDeclaration> passes;
    bool passCullingEnabled = true;
    GraphQueueExecutionMode queueExecutionMode =
        GraphQueueExecutionMode::Automatic;
    QueueSchedulingDecision automaticQueueDecision;
    RenderGraphBlackboard blackboard;
};
} // namespace Prism::Renderer
