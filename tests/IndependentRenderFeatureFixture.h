#pragma once

#include "Renderer/RenderFeatureRegistry.h"

#include <cstdint>
#include <string_view>

namespace Prism::Renderer::Tests
{
struct IndependentPostProcessInputSlot
{
};

struct IndependentPostProcessOutputSlot
{
};

struct IndependentPostProcessInput
{
    std::uint32_t resourceToken = 0;
};

struct IndependentPostProcessOutput
{
    std::uint32_t resourceToken = 0;
};

// Test-only extension fixture. It proves a Feature can join an existing stage
// through typed graph contracts without expanding shared frontend structs.
class IndependentRenderFeatureFixture
{
public:
    static constexpr std::string_view Id =
        "test.independent-post-process";
    static constexpr std::string_view InputResource =
        "IndependentPostProcess.InputColor";

    void SetEnabled(const bool enabled) noexcept
    {
        m_enabled = enabled;
    }

    [[nodiscard]] std::uint32_t BuildCount() const noexcept
    {
        return m_buildCount;
    }

    [[nodiscard]] RenderFeatureRegistration Registration()
    {
        RenderFeatureRegistration registration{};
        registration.id = std::string(Id);
        registration.scope = RenderFeatureScope::ViewLocal;
        registration.stages = {RenderFeatureStage::PostProcess};
        registration.requiredOperations = {
            RenderFeatureOperation::BuildGraph};
        registration.operations.buildGraph =
            [this](RenderFeatureGraphContext& context)
        {
            if (!m_enabled)
            {
                return;
            }

            const std::uint32_t generation =
                static_cast<std::uint32_t>(context.logicalFrameId);
            const auto& input = context.graph.GetBlackboard().Require<
                IndependentPostProcessInputSlot,
                IndependentPostProcessInput>({
                    std::string(Id),
                    context.viewId,
                    generation,
                    1u,
                    std::string(InputResource)});
            context.graph.GetBlackboard().Publish<
                IndependentPostProcessOutputSlot>(
                    IndependentPostProcessOutput{
                        input.resourceToken + 1u},
                    {std::string(Id),
                     RenderGraphBlackboardValueScope::ViewLocal,
                     context.viewId,
                     generation,
                     1u});
            context.graph.AddPass(
                "TestIndependentPostProcess",
                {InputResource},
                {"IndependentPostProcess.OutputColor"},
                []() {});
            ++m_buildCount;
        };
        return registration;
    }

private:
    bool m_enabled = true;
    std::uint32_t m_buildCount = 0;
};
} // namespace Prism::Renderer::Tests
