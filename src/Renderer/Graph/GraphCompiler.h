#pragma once

#include "Renderer/Graph/CompiledGraph.h"

#include <cstddef>

namespace Prism::Renderer
{
class GraphCompiler
{
public:
    static void BuildLivenessDependencies(GraphDescription& description);
    static void CullPasses(GraphDescription& description);
    [[nodiscard]] static std::size_t AssignExecutionIndices(
        GraphDescription& description);
    static void BuildExecutionDependencies(GraphDescription& description);
    static void ReduceTransitiveDependencies(GraphDescription& description);
    static void WriteCompiledPassPlans(
        const GraphDescription& description,
        CompiledGraph& compiledGraph);
};
} // namespace Prism::Renderer
