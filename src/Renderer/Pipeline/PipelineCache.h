#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Prism::RHI
{
class IComputePipeline;
class IGraphicsDevice;
class IGraphicsPipeline;
struct ComputePipelineDescription;
struct GraphicsPipelineDescription;
}

namespace Prism::Renderer
{
// Owns device-specific pipeline objects for the lifetime of one renderer.
// Callers provide stable semantic keys because hashing complete reflected
// pipeline descriptions would duplicate backend pipeline-cache work.
class PipelineCache
{
public:
    [[nodiscard]] static std::string SerializeGraphicsIdentity(
        const RHI::GraphicsPipelineDescription& description);
    std::shared_ptr<RHI::IGraphicsPipeline>
        GetOrCreateGraphics(
            RHI::IGraphicsDevice& device,
            std::string_view key,
            const RHI::GraphicsPipelineDescription& description);
    std::shared_ptr<RHI::IComputePipeline>
        GetOrCreateCompute(
            RHI::IGraphicsDevice& device,
            std::string_view key,
            const RHI::ComputePipelineDescription& description);

    [[nodiscard]] std::size_t GetPipelineCount() const;
    void Clear();

private:
    std::unordered_map<
        std::string,
        std::shared_ptr<RHI::IGraphicsPipeline>>
        m_graphicsPipelines;
    std::unordered_map<
        std::string,
        std::shared_ptr<RHI::IComputePipeline>>
        m_computePipelines;
};
} // namespace Prism::Renderer
