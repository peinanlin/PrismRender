#pragma once

#include "Renderer/RenderGraph.h"
#include "RHI/GraphicsResources.h"
#include "RHI/ShaderTypes.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace Prism::Asset { class ShaderManager; }
namespace Prism::RHI { class IGraphicsDevice; class IComputePipeline; }
namespace Prism::Renderer
{
class PipelineCache;

// Small per-frame tile counts, not a full-resolution synchronous image readback.
class WaterCoverageReadback
{
public:
    void Initialize(RHI::IGraphicsDevice& device, Asset::ShaderManager& shaders,
        PipelineCache& cache, const std::filesystem::path& directory,
        RHI::ShaderBinaryFormat format, std::uint32_t frames);
    void Prepare(RHI::IGraphicsDevice& device, std::uint32_t frameIndex,
        std::uint32_t width, std::uint32_t height,
        const std::shared_ptr<RHI::ITextureView>& mask);
    void AddPasses(RenderGraph& graph, TextureHandle mask) const;
    void Retire(std::uint64_t frameSerial, bool immediate = false);
    void Collect(std::uint64_t frameSerial);
    [[nodiscard]] float Coverage() const noexcept { return m_coverage; }
    [[nodiscard]] bool Available() const noexcept { return m_available; }
    [[nodiscard]] float Megabytes() const noexcept;

private:
    struct Slot
    {
        std::shared_ptr<RHI::IBuffer> counts;
        std::shared_ptr<RHI::IBuffer> readback;
        std::shared_ptr<RHI::IDescriptorSet> descriptors;
        std::uint32_t width = 0u;
        std::uint32_t height = 0u;
        mutable bool submitted = false;
    };
    std::shared_ptr<RHI::IComputePipeline> m_pipeline;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_layout;
    std::vector<Slot> m_slots;
    struct RetiredSlots
    {
        std::uint64_t releaseFrame = 0u;
        std::vector<Slot> slots;
    };
    std::vector<RetiredSlots> m_retired;
    std::uint32_t m_frameIndex = 0u;
    float m_coverage = 0.0f;
    bool m_available = false;
};
}
