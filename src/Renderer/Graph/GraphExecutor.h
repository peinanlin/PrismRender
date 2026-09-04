#pragma once

#include "Renderer/Graph/GraphExecutionState.h"

#include <chrono>
#include <functional>
#include <string_view>
#include <unordered_set>

namespace Prism::RHI
{
class ICommandContext;
}

namespace Prism::Core
{
class ITaskExecutor;
}

namespace Prism::Renderer
{
using GraphMarkerCallback = std::function<void(std::string_view)>;

class GraphExecutor
{
public:
    static void Execute(
        GraphDescription& description,
        GraphExecutionState& executionState,
        CompiledGraph& compiledGraph,
        bool detailedProfilingEnabled,
        const GraphMarkerCallback& beginMarker = {},
        const GraphMarkerCallback& endMarker = {});
    static void Execute(
        GraphDescription& description,
        GraphExecutionState& executionState,
        CompiledGraph& compiledGraph,
        bool detailedProfilingEnabled,
        RHI::ICommandContext& commandContext,
        const GraphMarkerCallback& beginMarker = {},
        const GraphMarkerCallback& endMarker = {});
    static void Execute(
        GraphDescription& description,
        GraphExecutionState& executionState,
        CompiledGraph& compiledGraph,
        bool detailedProfilingEnabled,
        RHI::ICommandContext& commandContext,
        Core::ITaskExecutor& taskExecutor,
        const GraphMarkerCallback& beginMarker = {},
        const GraphMarkerCallback& endMarker = {});

private:
    static void ExecuteQueueBatches(
        GraphDescription& description,
        GraphExecutionState& executionState,
        CompiledGraph& compiledGraph,
        bool detailedProfilingEnabled,
        RHI::ICommandContext& commandContext,
        Core::ITaskExecutor& taskExecutor,
        const GraphMarkerCallback& beginMarker,
        const GraphMarkerCallback& endMarker);
    static void ValidateReads(
        const GraphPassDeclaration& pass,
        const std::unordered_set<std::string>& availableResources);
    static void RecordPassResult(
        GraphPassDeclaration& pass,
        bool detailedProfilingEnabled,
        const std::chrono::steady_clock::time_point& start);
};
} // namespace Prism::Renderer
