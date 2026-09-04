#include "Renderer/Graph/ResourceStateTracker.h"

#include "Core/Assert.h"
#include "RHI/ICommandContext.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <unordered_set>
#include <vector>

namespace Prism::Renderer
{
namespace
{
bool PassAccessesResource(
    const GraphPassDeclaration& pass,
    const std::string_view resourceName)
{
    return std::ranges::find(pass.reads, resourceName) != pass.reads.end()
        || std::ranges::find(pass.writes, resourceName) != pass.writes.end();
}
} // namespace

void ResourceStateTracker::PreparePassBarriers(
    const GraphPassDeclaration& pass,
    GraphExecutionState& executionState,
    CompiledGraph& compiledGraph,
    RHI::ICommandContext& commandContext)
{
    std::vector<const GraphTrackedResourceAccess*> accesses;
    for (const GraphTrackedResourceAccess& access : pass.resourceReads)
    {
        accesses.push_back(&access);
    }
    for (const GraphTrackedResourceAccess& access : pass.resourceWrites)
    {
        accesses.push_back(&access);
    }
    for (std::size_t left = 0; left < accesses.size(); ++left)
    {
        for (std::size_t right = left + 1; right < accesses.size(); ++right)
        {
            const GraphTrackedResourceAccess& a = *accesses[left];
            const GraphTrackedResourceAccess& b = *accesses[right];
            if (a.name != b.name
                || a.state == b.state
                || a.state == RHI::ResourceState::Undefined
                || b.state == RHI::ResourceState::Undefined)
            {
                continue;
            }
            if (executionState.textureResources.contains(a.name))
            {
                const RHI::TextureDescription& description =
                    executionState.textureResources.at(a.name)
                        .texture->GetDescription();
                const auto endMip =
                    [&](const GraphTrackedResourceAccess& access)
                    {
                        return access.textureRange.baseMipLevel
                            + (access.textureRange.mipLevelCount == 0
                                   ? description.mipLevels
                                       - access.textureRange.baseMipLevel
                                   : access.textureRange.mipLevelCount);
                    };
                const auto endLayer =
                    [&](const GraphTrackedResourceAccess& access)
                    {
                        return access.textureRange.baseArrayLayer
                            + (access.textureRange.arrayLayerCount == 0
                                   ? description.arrayLayers
                                       - access.textureRange.baseArrayLayer
                                   : access.textureRange.arrayLayerCount);
                    };
                const bool overlaps =
                    a.textureRange.baseMipLevel < endMip(b)
                    && b.textureRange.baseMipLevel < endMip(a)
                    && a.textureRange.baseArrayLayer < endLayer(b)
                    && b.textureRange.baseArrayLayer < endLayer(a);
                Core::Check(
                    !overlaps,
                    "A RenderGraph pass requests conflicting states for overlapping texture subresources.");
            }
            else if (executionState.bufferResources.contains(a.name))
            {
                const std::size_t capacity =
                    executionState.bufferResources.at(a.name).description.size;
                const auto end =
                    [&](const GraphTrackedResourceAccess& access)
                    {
                        return access.bufferRange.offset
                            + (access.bufferRange.size == 0
                                   ? capacity - access.bufferRange.offset
                                   : access.bufferRange.size);
                    };
                const bool overlaps =
                    a.bufferRange.offset < end(b)
                    && b.bufferRange.offset < end(a);
                Core::Check(
                    !overlaps,
                    "A RenderGraph pass requests conflicting states for overlapping buffer ranges.");
            }
        }
    }

    const auto prepare =
        [&](const GraphTrackedResourceAccess& access, const bool write)
        {
            if (access.state == RHI::ResourceState::Undefined)
            {
                return;
            }
            const bool texture =
                access.texture
                || executionState.textureResources.contains(access.name);
            const bool buffer =
                access.buffer
                || executionState.bufferResources.contains(access.name);
            Core::Check(
                texture != buffer,
                "A stateful RenderGraph access must reference exactly one declared texture or buffer.");
            if (texture)
            {
                PrepareTextureAccessBarrier(
                    access,
                    write,
                    executionState,
                    compiledGraph,
                    commandContext);
            }
            else
            {
                PrepareBufferAccessBarrier(
                    access,
                    write,
                    executionState,
                    compiledGraph,
                    commandContext);
            }
        };
    for (const GraphTrackedResourceAccess& access : pass.resourceReads)
    {
        prepare(access, false);
    }
    for (const GraphTrackedResourceAccess& access : pass.resourceWrites)
    {
        prepare(access, true);
    }
}

void ResourceStateTracker::PrepareTextureAccessBarrier(
    const GraphTrackedResourceAccess& access,
    const bool write,
    GraphExecutionState& executionState,
    CompiledGraph& compiledGraph,
    RHI::ICommandContext& commandContext)
{
    const auto found = executionState.textureResources.find(access.name);
    Core::Check(
        found != executionState.textureResources.end(),
        "A stateful RenderGraph texture access references an undeclared texture.");
    GraphTextureExecutionResource& resource = found->second;
    Core::Check(
        resource.texture != nullptr,
        "A RenderGraph texture has no realized RHI resource.");
    const RHI::TextureDescription& description =
        resource.texture->GetDescription();
    Core::Check(
        access.textureRange.baseMipLevel < description.mipLevels
            && access.textureRange.baseArrayLayer < description.arrayLayers,
        "A RenderGraph texture access starts outside its subresources.");
    const std::uint32_t mipCount =
        access.textureRange.mipLevelCount == 0
        ? description.mipLevels - access.textureRange.baseMipLevel
        : access.textureRange.mipLevelCount;
    const std::uint32_t layerCount =
        access.textureRange.arrayLayerCount == 0
        ? description.arrayLayers - access.textureRange.baseArrayLayer
        : access.textureRange.arrayLayerCount;
    Core::Check(
        access.textureRange.baseMipLevel + mipCount <= description.mipLevels
            && access.textureRange.baseArrayLayer + layerCount
                <= description.arrayLayers,
        "A RenderGraph texture access exceeds its subresources.");
    Core::Check(
        resource.subresourceStates.size()
            == static_cast<std::size_t>(description.mipLevels)
                * description.arrayLayers,
        "A RenderGraph texture state table is invalid.");

    const bool wholeResource =
        access.textureRange.baseMipLevel == 0
        && mipCount == description.mipLevels
        && access.textureRange.baseArrayLayer == 0
        && layerCount == description.arrayLayers;
    const bool uniform = std::ranges::all_of(
        resource.subresourceStates,
        [&](const RHI::ResourceState state)
        {
            return state == resource.subresourceStates.front();
        });
    if (wholeResource && uniform)
    {
        const RHI::ResourceState before = resource.subresourceStates.front();
        if (before != access.state
            || (write
                && RHI::HasAnyFlag(
                    access.state,
                    RHI::ResourceState::UnorderedAccess)))
        {
            commandContext.TextureBarrier({
                resource.texture,
                before,
                access.state});
        }
        std::ranges::fill(resource.subresourceStates, access.state);
    }
    else
    {
        for (std::uint32_t layer = 0; layer < layerCount; ++layer)
        {
            for (std::uint32_t mip = 0; mip < mipCount; ++mip)
            {
                const std::uint32_t actualMip =
                    access.textureRange.baseMipLevel + mip;
                const std::uint32_t actualLayer =
                    access.textureRange.baseArrayLayer + layer;
                const std::size_t subresource =
                    static_cast<std::size_t>(actualLayer)
                        * description.mipLevels
                    + actualMip;
                const RHI::ResourceState before =
                    resource.subresourceStates[subresource];
                if (before != access.state
                    || (write
                        && RHI::HasAnyFlag(
                            access.state,
                            RHI::ResourceState::UnorderedAccess)))
                {
                    commandContext.TextureBarrier({
                        resource.texture,
                        before,
                        access.state,
                        actualMip,
                        1,
                        actualLayer,
                        1});
                }
                resource.subresourceStates[subresource] = access.state;
            }
        }
    }
    resource.state = resource.subresourceStates.front();
    const auto compiled = compiledGraph.compiledResources.find(access.name);
    if (compiled != compiledGraph.compiledResources.end())
    {
        compiled->second.state = resource.state;
    }
}

void ResourceStateTracker::PrepareBufferAccessBarrier(
    const GraphTrackedResourceAccess& access,
    const bool write,
    GraphExecutionState& executionState,
    CompiledGraph& compiledGraph,
    RHI::ICommandContext& commandContext)
{
    const auto found = executionState.bufferResources.find(access.name);
    Core::Check(
        found != executionState.bufferResources.end(),
        "A stateful RenderGraph buffer access references an undeclared buffer.");
    GraphBufferExecutionResource& resource = found->second;
    Core::Check(
        resource.buffer != nullptr,
        "A RenderGraph buffer has no realized RHI resource.");
    const std::size_t offset = access.bufferRange.offset;
    Core::Check(
        offset <= resource.description.size,
        "A RenderGraph buffer access starts outside its allocation.");
    const std::size_t size =
        access.bufferRange.size == 0
        ? resource.description.size - offset
        : access.bufferRange.size;
    Core::Check(
        offset + size <= resource.description.size,
        "A RenderGraph buffer access exceeds its allocation.");
    const std::size_t end = offset + size;

    if (!commandContext.SupportsBufferRangeBarriers())
    {
        Core::Check(
            !resource.states.empty(),
            "A RenderGraph whole-buffer state table is empty.");
        const RHI::ResourceState before = resource.states.front().state;
        if (before != access.state
            || (write
                && RHI::HasAnyFlag(
                    access.state,
                    RHI::ResourceState::UnorderedAccess)))
        {
            commandContext.BufferBarrier({
                resource.buffer,
                before,
                access.state,
                0,
                resource.description.size});
        }
        resource.states = {{0, resource.description.size, access.state}};
        const auto compiled = compiledGraph.compiledResources.find(access.name);
        if (compiled != compiledGraph.compiledResources.end())
        {
            compiled->second.state = access.state;
        }
        return;
    }

    std::vector<GraphBufferStateRange> updated;
    updated.reserve(resource.states.size() + 2);
    for (const GraphBufferStateRange& segment : resource.states)
    {
        const std::size_t segmentEnd = segment.offset + segment.size;
        const std::size_t overlapBegin = std::max(segment.offset, offset);
        const std::size_t overlapEnd = std::min(segmentEnd, end);
        if (overlapBegin >= overlapEnd)
        {
            updated.push_back(segment);
            continue;
        }
        if (segment.offset < overlapBegin)
        {
            updated.push_back({
                segment.offset,
                overlapBegin - segment.offset,
                segment.state});
        }
        if (segment.state != access.state
            || (write
                && RHI::HasAnyFlag(
                    access.state,
                    RHI::ResourceState::UnorderedAccess)))
        {
            commandContext.BufferBarrier({
                resource.buffer,
                segment.state,
                access.state,
                overlapBegin,
                overlapEnd - overlapBegin});
        }
        updated.push_back({
            overlapBegin,
            overlapEnd - overlapBegin,
            access.state});
        if (overlapEnd < segmentEnd)
        {
            updated.push_back({
                overlapEnd,
                segmentEnd - overlapEnd,
                segment.state});
        }
    }
    std::vector<GraphBufferStateRange> merged;
    for (const GraphBufferStateRange& segment : updated)
    {
        if (!merged.empty()
            && merged.back().offset + merged.back().size == segment.offset
            && merged.back().state == segment.state)
        {
            merged.back().size += segment.size;
        }
        else
        {
            merged.push_back(segment);
        }
    }
    resource.states = std::move(merged);
    const auto compiled = compiledGraph.compiledResources.find(access.name);
    if (compiled != compiledGraph.compiledResources.end()
        && !resource.states.empty())
    {
        compiled->second.state = resource.states.front().state;
    }
}

void ResourceStateTracker::TransitionResourceToCommon(
    const std::string_view resourceName,
    GraphExecutionState& executionState,
    CompiledGraph& compiledGraph,
    RHI::ICommandContext& commandContext)
{
    const std::string name(resourceName);
    const auto texture = executionState.textureResources.find(name);
    if (texture != executionState.textureResources.end()
        && texture->second.texture != nullptr)
    {
        GraphTextureExecutionResource& resource = texture->second;
        const RHI::TextureDescription& description =
            resource.texture->GetDescription();
        const bool uniform =
            !resource.subresourceStates.empty()
            && std::ranges::all_of(
                resource.subresourceStates,
                [&](const RHI::ResourceState state)
                {
                    return state == resource.subresourceStates.front();
                });
        if (uniform)
        {
            const RHI::ResourceState before = resource.subresourceStates.front();
            if (before != RHI::ResourceState::Undefined
                && before != RHI::ResourceState::Common)
            {
                commandContext.TextureBarrier({
                    resource.texture,
                    before,
                    RHI::ResourceState::Common});
            }
        }
        else
        {
            for (std::uint32_t layer = 0; layer < description.arrayLayers; ++layer)
            {
                for (std::uint32_t mip = 0; mip < description.mipLevels; ++mip)
                {
                    const std::size_t subresource =
                        static_cast<std::size_t>(layer)
                            * description.mipLevels
                        + mip;
                    const RHI::ResourceState before =
                        resource.subresourceStates[subresource];
                    if (before == RHI::ResourceState::Undefined
                        || before == RHI::ResourceState::Common)
                    {
                        continue;
                    }
                    commandContext.TextureBarrier({
                        resource.texture,
                        before,
                        RHI::ResourceState::Common,
                        mip,
                        1,
                        layer,
                        1});
                }
            }
        }
        for (RHI::ResourceState& state : resource.subresourceStates)
        {
            if (state != RHI::ResourceState::Undefined)
            {
                state = RHI::ResourceState::Common;
            }
        }
        resource.state =
            resource.subresourceStates.empty()
            ? RHI::ResourceState::Undefined
            : resource.subresourceStates.front();
    }

    const auto buffer = executionState.bufferResources.find(name);
    if (buffer != executionState.bufferResources.end()
        && buffer->second.buffer != nullptr)
    {
        for (GraphBufferStateRange& range : buffer->second.states)
        {
            if (range.state == RHI::ResourceState::Undefined
                || range.state == RHI::ResourceState::Common)
            {
                continue;
            }
            commandContext.BufferBarrier({
                buffer->second.buffer,
                range.state,
                RHI::ResourceState::Common,
                range.offset,
                range.size});
            range.state = RHI::ResourceState::Common;
        }
    }

    const auto compiled = compiledGraph.compiledResources.find(name);
    if (compiled != compiledGraph.compiledResources.end())
    {
        compiled->second.state = RHI::ResourceState::Common;
    }
}

void ResourceStateTracker::PrepareAliasingBarriers(
    const GraphPassDeclaration& pass,
    GraphExecutionState& executionState,
    CompiledGraph& compiledGraph,
    RHI::ICommandContext& commandContext)
{
    std::unordered_set<std::string> prepared;
    const auto prepare =
        [&](const GraphTrackedResourceAccess& access)
        {
            if (!prepared.emplace(access.name).second)
            {
                return;
            }
            const auto compiled = compiledGraph.compiledResources.find(access.name);
            const auto texture = executionState.textureResources.find(access.name);
            const auto buffer = executionState.bufferResources.find(access.name);
            const bool validTexture =
                texture != executionState.textureResources.end()
                && texture->second.texture != nullptr;
            const bool validBuffer =
                buffer != executionState.bufferResources.end()
                && buffer->second.buffer != nullptr;
            if (compiled == compiledGraph.compiledResources.end()
                || validTexture == validBuffer
                || compiled->second.firstUse != pass.executionIndex
                || compiled->second.nativePoolId == 0)
            {
                return;
            }

            const std::string key =
                std::to_string(compiled->second.nativePoolId)
                + ':' + std::to_string(compiled->second.nativeAllocation);
            const auto group = compiledGraph.nativeAliasGroups.find(key);
            if (group == compiledGraph.nativeAliasGroups.end())
            {
                return;
            }

            std::size_t activeCount = 0;
            const RHI::ITexture* previousTexture = nullptr;
            const RHI::IBuffer* previousBuffer = nullptr;
            std::size_t previousLastUse = 0;
            bool foundPrevious = false;
            for (const std::string& name : group->second)
            {
                const GraphCompiledResource& candidate =
                    compiledGraph.compiledResources.at(name);
                if (!candidate.active)
                {
                    continue;
                }
                ++activeCount;
                if (candidate.lastUse < compiled->second.firstUse
                    && (!foundPrevious || candidate.lastUse > previousLastUse))
                {
                    previousLastUse = candidate.lastUse;
                    if (validTexture)
                    {
                        previousTexture =
                            executionState.textureResources.at(name).texture;
                    }
                    else
                    {
                        previousBuffer =
                            executionState.bufferResources.at(name).buffer;
                    }
                    foundPrevious = true;
                }
            }
            if (activeCount <= 1)
            {
                return;
            }

            if (validTexture)
            {
                commandContext.TextureAliasingBarrier(
                    previousTexture,
                    *texture->second.texture);
                texture->second.state = RHI::ResourceState::Undefined;
                std::ranges::fill(
                    texture->second.subresourceStates,
                    RHI::ResourceState::Undefined);
            }
            else
            {
                commandContext.BufferAliasingBarrier(
                    previousBuffer,
                    *buffer->second.buffer);
                for (GraphBufferStateRange& stateRange : buffer->second.states)
                {
                    stateRange.state = RHI::ResourceState::Undefined;
                }
            }
            compiled->second.state = RHI::ResourceState::Undefined;
            ++compiledGraph.summary.executedAliasingBarrierCount;
        };

    for (const GraphTrackedResourceAccess& access : pass.resourceReads)
    {
        prepare(access);
    }
    for (const GraphTrackedResourceAccess& access : pass.resourceWrites)
    {
        prepare(access);
    }
}

void ResourceStateTracker::PrepareQueueHandoff(
    const GraphQueueClass sourceQueue,
    const GraphQueueClass destinationQueue,
    const std::size_t destinationExecutionIndex,
    const GraphDescription& description,
    GraphExecutionState& executionState,
    CompiledGraph& compiledGraph,
    RHI::ICommandContext& commandContext)
{
    for (const auto& [name, resource] : compiledGraph.compiledResources)
    {
        if (!resource.texture && !resource.buffer)
        {
            continue;
        }

        const GraphPassDeclaration* lastAccessBefore = nullptr;
        const GraphPassDeclaration* firstAccessAfter = nullptr;
        for (const GraphPassDeclaration& pass : description.passes)
        {
            if (!pass.active || !PassAccessesResource(pass, name))
            {
                continue;
            }
            if (pass.executionIndex < destinationExecutionIndex)
            {
                if (lastAccessBefore == nullptr
                    || pass.executionIndex > lastAccessBefore->executionIndex)
                {
                    lastAccessBefore = &pass;
                }
            }
            else if (firstAccessAfter == nullptr
                     || pass.executionIndex < firstAccessAfter->executionIndex)
            {
                firstAccessAfter = &pass;
            }
        }

        if (lastAccessBefore == nullptr
            || firstAccessAfter == nullptr
            || lastAccessBefore->options.queue != sourceQueue
            || firstAccessAfter->options.queue != destinationQueue)
        {
            continue;
        }
        TransitionResourceToCommon(
            name,
            executionState,
            compiledGraph,
            commandContext);
    }
}

void ResourceStateTracker::PrepareBatchQueueHandoffs(
    const GraphQueueBatchDescription& batch,
    const GraphDescription& description,
    GraphExecutionState& executionState,
    CompiledGraph& compiledGraph,
    RHI::ICommandContext& commandContext)
{
    // Reduced fence edges do not replace the full consecutive-access relation.
    for (const auto& [name, resource] : compiledGraph.compiledResources)
    {
        if (!resource.active || (!resource.texture && !resource.buffer))
        {
            continue;
        }
        const GraphPassDeclaration* lastInBatch = nullptr;
        for (const GraphPassDeclaration& pass : description.passes)
        {
            if (!pass.active
                || compiledGraph.passToQueueBatch[pass.originalIndex]
                    != batch.batchIndex
                || !PassAccessesResource(pass, name))
            {
                continue;
            }
            if (lastInBatch == nullptr
                || pass.executionIndex > lastInBatch->executionIndex)
            {
                lastInBatch = &pass;
            }
        }
        if (lastInBatch == nullptr)
        {
            continue;
        }
        const GraphPassDeclaration* next = nullptr;
        for (const GraphPassDeclaration& pass : description.passes)
        {
            if (!pass.active
                || pass.executionIndex <= lastInBatch->executionIndex
                || !PassAccessesResource(pass, name))
            {
                continue;
            }
            if (next == nullptr || pass.executionIndex < next->executionIndex)
            {
                next = &pass;
            }
        }
        if ((next != nullptr && next->options.queue != batch.queue)
            || (next == nullptr && batch.queue == GraphQueueClass::Compute))
        {
            TransitionResourceToCommon(
                name,
                executionState,
                compiledGraph,
                commandContext);
        }
    }
}
} // namespace Prism::Renderer
