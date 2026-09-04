#include "Renderer/Graph/QueueScheduler.h"

#include "Core/Assert.h"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <vector>

namespace Prism::Renderer
{
namespace
{
constexpr std::size_t InvalidIndex =
    std::numeric_limits<std::size_t>::max();

std::size_t QueueIndex(const GraphQueueClass queue)
{
    return queue == GraphQueueClass::Compute ? 1u : 0u;
}

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

void BuildQueueSyncDescriptions(
    const GraphDescription& description,
    CompiledGraph& compiledGraph)
{
    compiledGraph.queueSyncDescriptions.clear();
    for (const GraphPassDeclaration& consumer : description.passes)
    {
        if (!consumer.active)
        {
            continue;
        }
        for (const std::size_t dependency : consumer.executionDependencies)
        {
            Core::Check(
                dependency < description.passes.size(),
                "A RenderGraph queue dependency is out of range.");
            const GraphPassDeclaration& producer =
                description.passes[dependency];
            if (producer.options.queue == consumer.options.queue)
            {
                continue;
            }

            GraphQueueSyncDescription sync{};
            sync.producerPass = producer.originalIndex;
            sync.consumerPass = consumer.originalIndex;
            sync.producerQueue = producer.options.queue;
            sync.consumerQueue = consumer.options.queue;
            for (const std::string& written : producer.writes)
            {
                if (std::ranges::find(consumer.reads, written)
                        != consumer.reads.end()
                    || std::ranges::find(consumer.writes, written)
                        != consumer.writes.end())
                {
                    sync.resources.push_back(written);
                }
            }
            for (const std::string& read : producer.reads)
            {
                if (std::ranges::find(consumer.writes, read)
                        != consumer.writes.end()
                    || std::ranges::find(consumer.reads, read)
                        != consumer.reads.end())
                {
                    sync.resources.push_back(read);
                }
            }
            std::ranges::sort(sync.resources);
            const auto uniqueResources = std::ranges::unique(sync.resources);
            sync.resources.erase(uniqueResources.begin(), uniqueResources.end());
            compiledGraph.queueSyncDescriptions.push_back(std::move(sync));
        }
    }
    compiledGraph.summary.crossQueueSyncCount =
        compiledGraph.queueSyncDescriptions.size();
}

void BuildQueueBatches(
    const GraphDescription& description,
    CompiledGraph& compiledGraph)
{
    std::vector<GraphQueueBatchDescription>& batches =
        compiledGraph.queueBatchDescriptions;
    batches.clear();
    compiledGraph.queueBatchSubmissionOrder.clear();
    compiledGraph.passToQueueBatch.assign(
        description.passes.size(),
        InvalidIndex);

    std::vector<bool> incomingCrossQueue(
        description.passes.size(),
        false);
    std::vector<bool> outgoingCrossQueue(
        description.passes.size(),
        false);
    for (const GraphPassDeclaration& consumer : description.passes)
    {
        if (!consumer.active)
        {
            continue;
        }
        for (const std::size_t dependency : consumer.executionDependencies)
        {
            Core::Check(
                dependency < description.passes.size(),
                "A RenderGraph queue dependency is out of range.");
            const GraphPassDeclaration& producer =
                description.passes[dependency];
            if (producer.options.queue == consumer.options.queue)
            {
                continue;
            }
            incomingCrossQueue[consumer.originalIndex] = true;
            outgoingCrossQueue[producer.originalIndex] = true;
        }
    }

    std::array<std::size_t, 2> previousQueueBatch{
        InvalidIndex,
        InvalidIndex};
    for (const GraphQueueClass queue :
         {GraphQueueClass::Graphics, GraphQueueClass::Compute})
    {
        GraphQueueBatchDescription* current = nullptr;
        std::size_t previousPass = InvalidIndex;
        for (const GraphPassDeclaration& pass : description.passes)
        {
            if (!pass.active || pass.options.queue != queue)
            {
                continue;
            }
            const bool split =
                current != nullptr
                && (incomingCrossQueue[pass.originalIndex]
                    || (previousPass != InvalidIndex
                        && outgoingCrossQueue[previousPass]));
            if (current == nullptr || split)
            {
                GraphQueueBatchDescription batch{};
                batch.batchIndex = batches.size();
                batch.queue = queue;
                if (previousQueueBatch[QueueIndex(queue)] != InvalidIndex)
                {
                    batch.dependencies.push_back(
                        previousQueueBatch[QueueIndex(queue)]);
                }
                batches.push_back(std::move(batch));
                current = &batches.back();
                previousQueueBatch[QueueIndex(queue)] = current->batchIndex;
            }
            current->passes.push_back(pass.originalIndex);
            current->firstExecutionIndex = std::min(
                current->firstExecutionIndex,
                pass.executionIndex);
            current->lastExecutionIndex =
                current->lastExecutionIndex == InvalidIndex
                ? pass.executionIndex
                : std::max(current->lastExecutionIndex, pass.executionIndex);
            compiledGraph.passToQueueBatch[pass.originalIndex] =
                current->batchIndex;
            previousPass = pass.originalIndex;
        }
    }

    for (const GraphPassDeclaration& consumer : description.passes)
    {
        if (!consumer.active)
        {
            continue;
        }
        Core::Check(
            compiledGraph.passToQueueBatch[consumer.originalIndex]
                != InvalidIndex,
            "An active RenderGraph pass has no queue batch.");
        GraphQueueBatchDescription& consumerBatch =
            batches[compiledGraph.passToQueueBatch[consumer.originalIndex]];
        for (const std::size_t dependency : consumer.executionDependencies)
        {
            const std::size_t producerBatch =
                compiledGraph.passToQueueBatch[dependency];
            Core::Check(
                producerBatch != InvalidIndex,
                "An active RenderGraph dependency has no queue batch.");
            if (producerBatch != consumerBatch.batchIndex)
            {
                AddUniqueDependency(
                    consumerBatch.dependencies,
                    producerBatch);
            }
        }
        std::ranges::sort(consumerBatch.dependencies);
    }

    std::vector<std::size_t> indegrees(batches.size(), 0);
    std::vector<std::vector<std::size_t>> dependents(batches.size());
    for (const GraphQueueBatchDescription& batch : batches)
    {
        indegrees[batch.batchIndex] = batch.dependencies.size();
        for (const std::size_t dependency : batch.dependencies)
        {
            dependents[dependency].push_back(batch.batchIndex);
        }
    }

    std::vector<std::size_t> ready;
    for (std::size_t batchIndex = 0; batchIndex < indegrees.size(); ++batchIndex)
    {
        if (indegrees[batchIndex] == 0)
        {
            ready.push_back(batchIndex);
        }
    }
    GraphQueueClass previousQueue = GraphQueueClass::Compute;
    while (!ready.empty())
    {
        const auto chooseBatch =
            [&](const bool requireDifferentQueue)
            {
                return std::ranges::min_element(
                    ready,
                    [&](const std::size_t left, const std::size_t right)
                    {
                        const GraphQueueBatchDescription& leftBatch =
                            batches[left];
                        const GraphQueueBatchDescription& rightBatch =
                            batches[right];
                        const bool leftPreferred =
                            !requireDifferentQueue
                            || leftBatch.queue != previousQueue;
                        const bool rightPreferred =
                            !requireDifferentQueue
                            || rightBatch.queue != previousQueue;
                        if (leftPreferred != rightPreferred)
                        {
                            return leftPreferred;
                        }
                        return leftBatch.firstExecutionIndex
                            < rightBatch.firstExecutionIndex;
                    });
            };
        auto selected = chooseBatch(
            !compiledGraph.queueBatchSubmissionOrder.empty());
        const std::size_t batchIndex = *selected;
        ready.erase(selected);
        compiledGraph.queueBatchSubmissionOrder.push_back(batchIndex);
        previousQueue = batches[batchIndex].queue;
        for (const std::size_t dependent : dependents[batchIndex])
        {
            Core::Check(
                indegrees[dependent] > 0,
                "RenderGraph queue-batch indegree underflow.");
            if (--indegrees[dependent] == 0)
            {
                ready.push_back(dependent);
            }
        }
    }
    Core::Check(
        compiledGraph.queueBatchSubmissionOrder.size() == batches.size(),
        "RenderGraph queue-batch dependencies contain a cycle.");
    if (!compiledGraph.queueBatchSubmissionOrder.empty())
    {
        Core::Check(
            batches[compiledGraph.queueBatchSubmissionOrder.front()].queue
                == GraphQueueClass::Graphics,
            "RenderGraph queue-batch execution must begin on graphics.");
    }

    std::size_t crossQueueDependencies = 0;
    for (const GraphQueueBatchDescription& batch : batches)
    {
        for (const std::size_t dependency : batch.dependencies)
        {
            if (batches[dependency].queue != batch.queue)
            {
                ++crossQueueDependencies;
            }
        }
    }

    const auto reaches =
        [&](const std::size_t start, const std::size_t target)
        {
            std::vector<std::size_t> pending{start};
            std::vector<bool> visited(batches.size(), false);
            while (!pending.empty())
            {
                const std::size_t current = pending.back();
                pending.pop_back();
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
                    batches[current].dependencies.begin(),
                    batches[current].dependencies.end());
            }
            return false;
        };
    std::size_t overlapOpportunities = 0;
    for (std::size_t left = 0; left < batches.size(); ++left)
    {
        for (std::size_t right = left + 1; right < batches.size(); ++right)
        {
            if (batches[left].queue != batches[right].queue
                && !reaches(left, right)
                && !reaches(right, left))
            {
                ++overlapOpportunities;
            }
        }
    }

    compiledGraph.summary.queueBatchCount = batches.size();
    compiledGraph.summary.crossQueueBatchDependencyCount =
        crossQueueDependencies;
    compiledGraph.summary.overlapOpportunityCount = overlapOpportunities;
}
} // namespace

void QueueScheduler::Build(
    const GraphDescription& description,
    CompiledGraph& compiledGraph)
{
    BuildQueueSyncDescriptions(description, compiledGraph);
    BuildQueueBatches(description, compiledGraph);
}
} // namespace Prism::Renderer
