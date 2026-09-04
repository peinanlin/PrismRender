#pragma once

#include "Renderer/Graph/CompiledGraph.h"

namespace Prism::Renderer
{
class QueueScheduler
{
public:
    static void Build(
        const GraphDescription& description,
        CompiledGraph& compiledGraph);
};
} // namespace Prism::Renderer
