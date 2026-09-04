#pragma once

#include "Renderer/Graph/GraphExecutionState.h"

namespace Prism::Renderer
{
class TransientAliasPlanner
{
public:
    static void Build(
        GraphDescription& description,
        const GraphExecutionState& executionState,
        CompiledGraph& compiledGraph);
};
} // namespace Prism::Renderer
