#pragma once

#include "Asset/ShaderManager.h"
#include "RHI/ICommandContext.h"
#include "Renderer/RenderGraph.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace Prism::RHI
{
class IBuffer;
class IComputePipeline;
class IDescriptorSet;
class IDescriptorSetLayout;
class IGraphicsDevice;
class ITexture;
class ITextureView;
}

namespace Prism::Renderer
{
class PipelineCache;

struct VarianceShadowMapsFeatureSlot
{
};

struct VarianceShadowMapsGraphContribution
{
    RHI::ITexture* moments = nullptr;
    RHI::ITexture* scratch = nullptr;
    RHI::ResourceState momentsInitialState =
        RHI::ResourceState::ShaderResource;
    RHI::ResourceState scratchInitialState =
        RHI::ResourceState::ShaderResource;
    RenderGraph::ParameterExecuteCallback convert;
    RenderGraph::ParameterExecuteCallback horizontal;
    RenderGraph::ParameterExecuteCallback vertical;
};

class VarianceShadowMaps
{
public:
    static constexpr std::uint32_t Resolution = 1024;
    static constexpr std::uint32_t CascadeCount = 3;

    void Initialize(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderPath,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight,
        std::shared_ptr<RHI::ITexture> shadowDepth);
    void Update(std::uint32_t frameIndex, bool exponential);
    void SetShadowDepth(std::shared_ptr<RHI::ITexture> shadowDepth);
    void ExecuteConvert(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteHorizontalBlur(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteVerticalBlur(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void EndFrame(bool enabled);
    [[nodiscard]] VarianceShadowMapsGraphContribution
        CreateRenderGraphContribution(std::uint32_t frameIndex);
    static void AddPasses(
        RenderGraph& graph,
        TextureHandle shadowMap,
        TextureHandle& shadowMoments,
        TextureHandle& shadowMomentsScratch,
        RenderGraph::ParameterExecuteCallback convert,
        RenderGraph::ParameterExecuteCallback horizontal,
        RenderGraph::ParameterExecuteCallback vertical);

    [[nodiscard]] RHI::ITexture& GetMomentsTexture() const;
    [[nodiscard]] RHI::ITexture& GetScratchTexture() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetMomentsSampledView() const;
    [[nodiscard]] RHI::ResourceState GetMomentsInitialState() const;
    [[nodiscard]] RHI::ResourceState GetScratchInitialState() const;

private:
    struct alignas(16) Constants
    {
        std::uint32_t directionX = 0;
        std::uint32_t directionY = 0;
        std::uint32_t exponential = 0;
        std::uint32_t padding = 0;
    };

    struct FrameResources
    {
        std::array<std::shared_ptr<RHI::IBuffer>, 3> constants;
        std::array<std::shared_ptr<RHI::IDescriptorSet>, 3> descriptorSets;
    };

    void Dispatch(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex,
        std::uint32_t passIndex,
        const std::shared_ptr<RHI::IComputePipeline>& pipeline) const;

    std::shared_ptr<RHI::IDescriptorSetLayout> m_layout;
    std::shared_ptr<RHI::IComputePipeline> m_convertPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_blurPipeline;
    std::vector<FrameResources> m_frames;
    std::shared_ptr<RHI::ITexture> m_moments;
    std::shared_ptr<RHI::ITexture> m_scratch;
    std::shared_ptr<RHI::ITextureView> m_momentsSampledView;
    std::shared_ptr<RHI::ITextureView> m_momentsStorageView;
    std::shared_ptr<RHI::ITextureView> m_scratchSampledView;
    std::shared_ptr<RHI::ITextureView> m_scratchStorageView;
    RHI::ResourceState m_momentsState = RHI::ResourceState::Undefined;
    RHI::ResourceState m_scratchState = RHI::ResourceState::Undefined;
};
} // namespace Prism::Renderer
