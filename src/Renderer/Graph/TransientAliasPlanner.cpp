#include "Renderer/Graph/TransientAliasPlanner.h"

#include "Core/Assert.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Prism::Renderer
{
namespace
{
constexpr std::size_t InvalidIndex =
    std::numeric_limits<std::size_t>::max();

bool AreAliasCompatible(
    const RHI::TextureDescription& left,
    const RHI::TextureDescription& right)
{
    return left.dimension == right.dimension
        && left.width == right.width
        && left.height == right.height
        && left.arrayLayers == right.arrayLayers
        && left.mipLevels == right.mipLevels
        && left.sampleCount == right.sampleCount
        && left.format == right.format
        && left.usage == right.usage
        && left.memoryAccess == right.memoryAccess;
}

bool AreAliasCompatible(
    const RHI::BufferDescription& left,
    const RHI::BufferDescription& right)
{
    return left.size == right.size
        && left.stride == right.stride
        && left.usage == right.usage
        && left.memoryAccess == right.memoryAccess;
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

void BuildLogicalPlan(
    const GraphDescription& description,
    CompiledGraph& compiledGraph)
{
    struct Allocation
    {
        std::size_t index = 0;
        std::size_t lastUse = 0;
        bool buffer = false;
        RHI::TextureDescription description;
        RHI::BufferDescription bufferDescription;
        std::uint64_t bytes = 0;
    };

    std::vector<GraphCompiledResource*> resources;
    for (auto& [name, resource] : compiledGraph.compiledResources)
    {
        (void)name;
        if (resource.transient && resource.active)
        {
            resources.push_back(&resource);
        }
    }
    std::ranges::sort(
        resources,
        [](const GraphCompiledResource* left,
           const GraphCompiledResource* right)
        {
            if (left->firstUse != right->firstUse)
            {
                return left->firstUse < right->firstUse;
            }
            return left->name < right->name;
        });

    std::vector<Allocation> allocations;
    for (GraphCompiledResource* resource : resources)
    {
        const bool buffer = resource->buffer;
        auto reusable = std::ranges::find_if(
            allocations,
            [&](const Allocation& allocation)
            {
                if (allocation.lastUse >= resource->firstUse
                    || allocation.buffer != buffer)
                {
                    return false;
                }
                if (buffer)
                {
                    return AreAliasCompatible(
                        allocation.bufferDescription,
                        description.transientBuffers.at(resource->name)
                            .description);
                }
                return AreAliasCompatible(
                    allocation.description,
                    description.transientTextures.at(resource->name)
                        .description);
            });
        if (reusable == allocations.end())
        {
            resource->physicalAllocation = allocations.size();
            allocations.push_back({
                allocations.size(),
                resource->lastUse,
                buffer,
                buffer
                    ? RHI::TextureDescription{}
                    : description.transientTextures.at(resource->name)
                          .description,
                buffer
                    ? description.transientBuffers.at(resource->name)
                          .description
                    : RHI::BufferDescription{},
                resource->estimatedBytes});
        }
        else
        {
            resource->physicalAllocation = reusable->index;
            reusable->lastUse = resource->lastUse;
            reusable->bytes = std::max(
                reusable->bytes,
                resource->estimatedBytes);
        }
    }

    GraphCompilationSummary& summary = compiledGraph.summary;
    summary.transientResourceCount = resources.size();
    summary.transientAllocationCount = allocations.size();
    for (const GraphCompiledResource* resource : resources)
    {
        summary.transientLogicalBytes += resource->estimatedBytes;
    }
    for (const Allocation& allocation : allocations)
    {
        summary.transientPhysicalBytes += allocation.bytes;
    }
    summary.transientAliasedBytes =
        summary.transientLogicalBytes >= summary.transientPhysicalBytes
        ? summary.transientLogicalBytes - summary.transientPhysicalBytes
        : 0;
}

void BuildNativePlan(
    const GraphDescription& description,
    const GraphExecutionState& executionState,
    CompiledGraph& compiledGraph)
{
    compiledGraph.nativeAliasGroups.clear();
    std::unordered_map<std::uint64_t, std::uint64_t> poolPhysicalBytes;
    std::unordered_set<std::string> nativeAllocations;
    bool allActiveResourcesNative = true;

    for (const auto& [name, transient] : description.transientBuffers)
    {
        (void)transient;
        const auto buffer = executionState.bufferResources.find(name);
        const auto compiled = compiledGraph.compiledResources.find(name);
        if (buffer == executionState.bufferResources.end()
            || compiled == compiledGraph.compiledResources.end()
            || buffer->second.buffer == nullptr)
        {
            if (compiled != compiledGraph.compiledResources.end()
                && compiled->second.active)
            {
                allActiveResourcesNative = false;
            }
            continue;
        }

        const RHI::TransientBufferAllocationInfo* allocation =
            buffer->second.buffer->GetTransientAllocationInfo();
        if (allocation == nullptr)
        {
            if (compiled->second.active)
            {
                allActiveResourcesNative = false;
            }
            continue;
        }

        const std::string key =
            std::to_string(allocation->poolId)
            + ':' + std::to_string(allocation->allocationIndex);
        compiledGraph.nativeAliasGroups[key].push_back(name);
        nativeAllocations.emplace(key);
        poolPhysicalBytes.try_emplace(
            allocation->poolId,
            allocation->poolPhysicalBytes);
        compiledGraph.summary.nativeTransientLogicalBytes +=
            allocation->logicalBytes;
    }

    for (const auto& [name, transient] : description.transientTextures)
    {
        (void)transient;
        const auto texture = executionState.textureResources.find(name);
        const auto compiled = compiledGraph.compiledResources.find(name);
        if (texture == executionState.textureResources.end()
            || compiled == compiledGraph.compiledResources.end()
            || texture->second.texture == nullptr)
        {
            if (compiled != compiledGraph.compiledResources.end()
                && compiled->second.active)
            {
                allActiveResourcesNative = false;
            }
            continue;
        }

        const RHI::TransientTextureAllocationInfo* allocation =
            texture->second.texture->GetTransientAllocationInfo();
        if (allocation == nullptr)
        {
            if (compiled->second.active)
            {
                allActiveResourcesNative = false;
            }
            continue;
        }

        const std::string key =
            std::to_string(allocation->poolId)
            + ':' + std::to_string(allocation->allocationIndex);
        compiledGraph.nativeAliasGroups[key].push_back(name);
        nativeAllocations.emplace(key);
        poolPhysicalBytes.try_emplace(
            allocation->poolId,
            allocation->poolPhysicalBytes);
        compiledGraph.summary.nativeTransientLogicalBytes +=
            allocation->logicalBytes;
    }

    GraphCompilationSummary& summary = compiledGraph.summary;
    summary.nativeTransientAllocationCount = nativeAllocations.size();
    for (const auto& [poolId, bytes] : poolPhysicalBytes)
    {
        (void)poolId;
        summary.nativeTransientPhysicalBytes += bytes;
    }
    summary.nativeTransientAliasedBytes =
        summary.nativeTransientLogicalBytes
            >= summary.nativeTransientPhysicalBytes
        ? summary.nativeTransientLogicalBytes
            - summary.nativeTransientPhysicalBytes
        : 0;

    bool lifetimesValid = true;
    for (auto& [key, names] : compiledGraph.nativeAliasGroups)
    {
        (void)key;
        std::ranges::sort(
            names,
            [&](const std::string& left, const std::string& right)
            {
                const GraphCompiledResource& leftResource =
                    compiledGraph.compiledResources.at(left);
                const GraphCompiledResource& rightResource =
                    compiledGraph.compiledResources.at(right);
                if (leftResource.firstUse != rightResource.firstUse)
                {
                    return leftResource.firstUse < rightResource.firstUse;
                }
                return left < right;
            });

        std::vector<const GraphCompiledResource*> active;
        for (const std::string& name : names)
        {
            const GraphCompiledResource& resource =
                compiledGraph.compiledResources.at(name);
            if (resource.active)
            {
                active.push_back(&resource);
            }
        }
        for (std::size_t index = 1; index < active.size(); ++index)
        {
            if (active[index - 1]->lastUse >= active[index]->firstUse)
            {
                lifetimesValid = false;
            }
        }
        if (active.size() > 1)
        {
            summary.expectedAliasingBarrierCount += active.size();
        }
    }

    summary.nativeTransientAliasingPlanValid =
        allActiveResourcesNative && lifetimesValid;
    summary.nativeTransientAliasingApplied =
        allActiveResourcesNative
        && lifetimesValid
        && summary.nativeTransientAliasedBytes > 0;
    Core::Check(
        lifetimesValid,
        "Native transient resources with overlapping lifetimes share one allocation.");
}

void BuildAliasingDependencies(
    GraphDescription& description,
    const CompiledGraph& compiledGraph)
{
    std::unordered_map<
        std::string,
        std::vector<const GraphCompiledResource*>> aliasGroups;
    for (const auto& [name, resource] : compiledGraph.compiledResources)
    {
        if (!resource.active
            || !resource.transient
            || resource.physicalAllocation == InvalidIndex)
        {
            continue;
        }
        const std::string key =
            resource.nativePoolId != 0
            ? "native:" + std::to_string(resource.nativePoolId)
                + ':' + std::to_string(resource.nativeAllocation)
            : "logical:" + std::to_string(resource.physicalAllocation);
        aliasGroups[key].push_back(&resource);
    }

    const auto findPassByExecutionIndex =
        [&](const std::size_t executionIndex) -> GraphPassDeclaration*
        {
            const auto found = std::ranges::find(
                description.passes,
                executionIndex,
                &GraphPassDeclaration::executionIndex);
            return found == description.passes.end() ? nullptr : &*found;
        };

    for (auto& [key, resources] : aliasGroups)
    {
        (void)key;
        std::ranges::sort(
            resources,
            [](const GraphCompiledResource* left,
               const GraphCompiledResource* right)
            {
                if (left->firstUse != right->firstUse)
                {
                    return left->firstUse < right->firstUse;
                }
                return left->name < right->name;
            });
        for (std::size_t index = 1; index < resources.size(); ++index)
        {
            GraphPassDeclaration* producer = findPassByExecutionIndex(
                resources[index - 1]->lastUse);
            GraphPassDeclaration* consumer = findPassByExecutionIndex(
                resources[index]->firstUse);
            Core::Check(
                producer != nullptr && consumer != nullptr,
                "RenderGraph aliasing dependency references an invalid pass.");
            if (producer->originalIndex != consumer->originalIndex)
            {
                AddUniqueDependency(
                    consumer->executionDependencies,
                    producer->originalIndex);
                std::ranges::sort(consumer->executionDependencies);
            }
        }
    }
}
} // namespace

void TransientAliasPlanner::Build(
    GraphDescription& description,
    const GraphExecutionState& executionState,
    CompiledGraph& compiledGraph)
{
    BuildLogicalPlan(description, compiledGraph);
    BuildNativePlan(description, executionState, compiledGraph);
    BuildAliasingDependencies(description, compiledGraph);
}
} // namespace Prism::Renderer
