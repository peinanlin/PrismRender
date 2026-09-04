#pragma once

#include "Renderer/Graph/CompiledGraph.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Prism::Renderer
{
struct GraphTextureExecutionResource
{
    RHI::ITexture* texture = nullptr;
    RHI::ResourceState state = RHI::ResourceState::Undefined;
    std::vector<RHI::ResourceState> subresourceStates;
};

struct GraphBufferStateRange
{
    std::size_t offset = 0;
    std::size_t size = 0;
    RHI::ResourceState state = RHI::ResourceState::Undefined;
};

struct GraphBufferExecutionResource
{
    RHI::IBuffer* buffer = nullptr;
    RHI::BufferDescription description;
    std::vector<GraphBufferStateRange> states;
};

struct GraphExecutionState
{
    std::unordered_map<std::string, GraphTextureExecutionResource>
        textureResources;
    std::unordered_map<std::string, GraphBufferExecutionResource>
        bufferResources;
    std::vector<GraphPassInfo> passInfos;
    std::uint64_t executionGeneration = 0;
    bool executing = false;
    bool hasCompletedSummary = false;
    GraphCompilationSummary compilationBaseline;
    GraphCompilationSummary completedSummary;

    void BeginExecution(const GraphCompilationSummary& compiledSummary)
    {
        ++executionGeneration;
        executing = true;
        passInfos.clear();
        compilationBaseline = compiledSummary;
    }

    void EndExecution(
        GraphCompilationSummary& activeSummary,
        const bool completed) noexcept
    {
        if (completed)
        {
            completedSummary = activeSummary;
            hasCompletedSummary = true;
        }
        else
        {
            passInfos.clear();
        }
        activeSummary = compilationBaseline;
        executing = false;
    }

    void InvalidateCompiledPlan() noexcept
    {
        hasCompletedSummary = false;
        passInfos.clear();
    }

    [[nodiscard]] const GraphCompilationSummary& GetVisibleSummary(
        const GraphCompilationSummary& compiledSummary) const noexcept
    {
        if (executing || !hasCompletedSummary)
        {
            return compiledSummary;
        }
        return completedSummary;
    }
};
} // namespace Prism::Renderer
