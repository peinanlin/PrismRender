#pragma once

#include "Renderer/Graph/GraphExecutionState.h"

#include <string_view>

namespace Prism::RHI
{
class ICommandContext;
}

namespace Prism::Renderer
{
class ResourceStateTracker
{
public:
    static void PreparePassBarriers(
        const GraphPassDeclaration& pass,
        GraphExecutionState& executionState,
        CompiledGraph& compiledGraph,
        RHI::ICommandContext& commandContext);
    static void PrepareAliasingBarriers(
        const GraphPassDeclaration& pass,
        GraphExecutionState& executionState,
        CompiledGraph& compiledGraph,
        RHI::ICommandContext& commandContext);
    static void TransitionResourceToCommon(
        std::string_view resourceName,
        GraphExecutionState& executionState,
        CompiledGraph& compiledGraph,
        RHI::ICommandContext& commandContext);
    static void PrepareQueueHandoff(
        GraphQueueClass sourceQueue,
        GraphQueueClass destinationQueue,
        std::size_t destinationExecutionIndex,
        const GraphDescription& description,
        GraphExecutionState& executionState,
        CompiledGraph& compiledGraph,
        RHI::ICommandContext& commandContext);
    static void PrepareBatchQueueHandoffs(
        const GraphQueueBatchDescription& batch,
        const GraphDescription& description,
        GraphExecutionState& executionState,
        CompiledGraph& compiledGraph,
        RHI::ICommandContext& commandContext);

private:
    static void PrepareTextureAccessBarrier(
        const GraphTrackedResourceAccess& access,
        bool write,
        GraphExecutionState& executionState,
        CompiledGraph& compiledGraph,
        RHI::ICommandContext& commandContext);
    static void PrepareBufferAccessBarrier(
        const GraphTrackedResourceAccess& access,
        bool write,
        GraphExecutionState& executionState,
        CompiledGraph& compiledGraph,
        RHI::ICommandContext& commandContext);
};
} // namespace Prism::Renderer
