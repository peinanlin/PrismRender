#include "Renderer/Graph/GraphCompiler.h"

#include "Core/Assert.h"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace Prism::Renderer
{
namespace
{
constexpr std::size_t InvalidIndex =
    std::numeric_limits<std::size_t>::max();

void AddUniqueDependency(
    std::vector<std::size_t>& dependencies,
    const std::size_t dependency)
{
    if (std::ranges::find(dependencies, dependency)
        == dependencies.end())
    {
        dependencies.push_back(dependency);
    }
}

std::size_t QueueIndex(const GraphQueueClass queue)
{
    return queue == GraphQueueClass::Compute ? 1u : 0u;
}

void ValidatePassIndices(const GraphDescription& description)
{
    for (std::size_t index = 0; index < description.passes.size(); ++index)
    {
        Core::Check(
            description.passes[index].originalIndex == index,
            "A RenderGraph pass has an invalid declaration index.");
    }
}
} // namespace

void GraphCompiler::BuildLivenessDependencies(GraphDescription& description)
{
    ValidatePassIndices(description);
    std::unordered_map<std::string, std::size_t> lastWriter;
    for (GraphPassDeclaration& pass : description.passes)
    {
        pass.livenessDependencies.clear();
        for (const std::string& resource : pass.reads)
        {
            const auto writer = lastWriter.find(resource);
            if (writer != lastWriter.end())
            {
                AddUniqueDependency(
                    pass.livenessDependencies,
                    writer->second);
            }
        }
        for (const std::string& resource : pass.writes)
        {
            lastWriter[resource] = pass.originalIndex;
        }
    }
}

void GraphCompiler::CullPasses(GraphDescription& description)
{
    ValidatePassIndices(description);
    const bool hasExplicitRoots =
        !description.outputResources.empty()
        || std::ranges::any_of(
            description.passes,
            [](const GraphPassDeclaration& pass)
            {
                return pass.options.sideEffect
                    || !pass.options.allowCulling;
            });
    const bool cullingActive =
        description.passCullingEnabled && hasExplicitRoots;

    for (GraphPassDeclaration& pass : description.passes)
    {
        pass.active = !cullingActive;
        pass.cullReason.clear();
        pass.executionIndex = InvalidIndex;
        pass.info.cpuMilliseconds = 0.0f;
    }
    if (!cullingActive)
    {
        return;
    }

    std::unordered_map<std::string, std::size_t> lastWriter;
    for (const GraphPassDeclaration& pass : description.passes)
    {
        for (const std::string& resource : pass.writes)
        {
            lastWriter[resource] = pass.originalIndex;
        }
    }

    std::vector<std::size_t> pending;
    for (const GraphPassDeclaration& pass : description.passes)
    {
        if (pass.options.sideEffect || !pass.options.allowCulling)
        {
            pending.push_back(pass.originalIndex);
        }
    }
    for (const std::string& output : description.outputResources)
    {
        const auto writer = lastWriter.find(output);
        if (writer != lastWriter.end())
        {
            pending.push_back(writer->second);
        }
    }

    while (!pending.empty())
    {
        const std::size_t index = pending.back();
        pending.pop_back();
        Core::Check(
            index < description.passes.size(),
            "A RenderGraph liveness dependency is out of range.");
        GraphPassDeclaration& pass = description.passes[index];
        if (pass.active)
        {
            continue;
        }
        pass.active = true;
        pending.insert(
            pending.end(),
            pass.livenessDependencies.begin(),
            pass.livenessDependencies.end());
    }

    for (GraphPassDeclaration& pass : description.passes)
    {
        if (!pass.active)
        {
            pass.cullReason = "not_reachable_from_output";
        }
    }
}

std::size_t GraphCompiler::AssignExecutionIndices(
    GraphDescription& description)
{
    std::size_t executionIndex = 0;
    for (GraphPassDeclaration& pass : description.passes)
    {
        if (pass.active)
        {
            pass.executionIndex = executionIndex++;
        }
    }
    return executionIndex;
}

void GraphCompiler::BuildExecutionDependencies(GraphDescription& description)
{
    ValidatePassIndices(description);
    std::unordered_map<std::string, std::size_t> lastWriter;
    std::unordered_map<std::string, std::vector<std::size_t>>
        readersSinceWrite;
    std::unordered_map<std::string, std::size_t> lastAccessor;
    std::array<std::size_t, 2> lastQueuePass{
        InvalidIndex,
        InvalidIndex};

    for (GraphPassDeclaration& pass : description.passes)
    {
        pass.executionDependencies.clear();
        if (!pass.active)
        {
            continue;
        }

        const std::size_t queueIndex = QueueIndex(pass.options.queue);
        if (lastQueuePass[queueIndex] != InvalidIndex)
        {
            AddUniqueDependency(
                pass.executionDependencies,
                lastQueuePass[queueIndex]);
        }

        for (const std::string& resource : pass.reads)
        {
            const auto writer = lastWriter.find(resource);
            if (writer != lastWriter.end())
            {
                AddUniqueDependency(pass.executionDependencies, writer->second);
            }
            const auto accessor = lastAccessor.find(resource);
            if (accessor != lastAccessor.end()
                && accessor->second != pass.originalIndex
                && description.passes[accessor->second].options.queue
                    != pass.options.queue)
            {
                AddUniqueDependency(
                    pass.executionDependencies,
                    accessor->second);
            }
            readersSinceWrite[resource].push_back(pass.originalIndex);
            lastAccessor[resource] = pass.originalIndex;
        }

        for (const std::string& resource : pass.writes)
        {
            const auto writer = lastWriter.find(resource);
            if (writer != lastWriter.end())
            {
                AddUniqueDependency(pass.executionDependencies, writer->second);
            }
            auto readers = readersSinceWrite.find(resource);
            if (readers != readersSinceWrite.end())
            {
                for (const std::size_t reader : readers->second)
                {
                    if (reader != pass.originalIndex)
                    {
                        AddUniqueDependency(
                            pass.executionDependencies,
                            reader);
                    }
                }
                readers->second.clear();
            }
            lastWriter[resource] = pass.originalIndex;
            lastAccessor[resource] = pass.originalIndex;
        }
        lastQueuePass[queueIndex] = pass.originalIndex;
        std::ranges::sort(pass.executionDependencies);
    }
}

void GraphCompiler::ReduceTransitiveDependencies(GraphDescription& description)
{
    ValidatePassIndices(description);
    const auto reaches =
        [&](const std::size_t start, const std::size_t target)
        {
            std::vector<std::size_t> pending{start};
            std::vector<bool> visited(description.passes.size(), false);
            while (!pending.empty())
            {
                const std::size_t current = pending.back();
                pending.pop_back();
                Core::Check(
                    current < description.passes.size(),
                    "A RenderGraph execution dependency is out of range.");
                if (current == target)
                {
                    return true;
                }
                if (visited[current])
                {
                    continue;
                }
                visited[current] = true;
                pending.insert(
                    pending.end(),
                    description.passes[current]
                        .executionDependencies.begin(),
                    description.passes[current]
                        .executionDependencies.end());
            }
            return false;
        };

    for (GraphPassDeclaration& pass : description.passes)
    {
        if (!pass.active || pass.executionDependencies.size() < 2)
        {
            continue;
        }
        std::vector<std::size_t> reduced;
        reduced.reserve(pass.executionDependencies.size());
        for (const std::size_t dependency : pass.executionDependencies)
        {
            const bool redundant = std::ranges::any_of(
                pass.executionDependencies,
                [&](const std::size_t alternative)
                {
                    return alternative != dependency
                        && reaches(alternative, dependency);
                });
            if (!redundant)
            {
                reduced.push_back(dependency);
            }
        }
        pass.executionDependencies = std::move(reduced);
    }
}

void GraphCompiler::WriteCompiledPassPlans(
    const GraphDescription& description,
    CompiledGraph& compiledGraph)
{
    compiledGraph.passes.clear();
    compiledGraph.passes.reserve(description.passes.size());
    for (const GraphPassDeclaration& pass : description.passes)
    {
        compiledGraph.passes.push_back({
            pass.executionIndex,
            pass.active,
            pass.cullReason,
            pass.livenessDependencies,
            pass.executionDependencies});
    }
}
} // namespace Prism::Renderer
