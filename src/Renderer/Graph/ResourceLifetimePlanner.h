#pragma once

#include "Renderer/Graph/GraphExecutionState.h"

namespace Prism::Renderer
{
class ResourceLifetimePlanner
{
public:
    static void Build(
        const GraphDescription& description,
        const GraphExecutionState& executionState,
        CompiledGraph& compiledGraph);
};
} // namespace Prism::Renderer
