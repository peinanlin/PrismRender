#pragma once

#include "Asset/ShaderManager.h"
#include "RHI/ICommandContext.h"
#include "Renderer/RenderGraph.h"

#include <DirectXMath.h>

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

struct TemporalAntiAliasingFeatureSlot
{
};

struct TemporalAntiAliasingGraphContribution
{
    RHI::ITexture* motionVectors = nullptr;
    RHI::ITexture* resolved = nullptr;
    RHI::ITexture* historyRead = nullptr;
    RHI::ITexture* historyWrite = nullptr;
    RHI::ResourceState motionInitialState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState resolvedInitialState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState historyReadInitialState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState historyWriteInitialState =
        RHI::ResourceState::Undefined;
    RenderGraph::ParameterExecuteCallback execute;
};

class TemporalAntiAliasing
{
public:
    void Initialize(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderPath,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight);
    void Resize(
        std::uint32_t width,
        std::uint32_t height,
        std::shared_ptr<RHI::ITexture> currentColor);
    void Update(
        std::uint32_t frameIndex,
        bool enabled);
    void SetCurrentColor(
        std::uint32_t frameIndex,
        std::shared_ptr<RHI::ITexture> currentColor);
    void Execute(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void EndFrame();
    void NotifyMotionFinalState(RHI::ResourceState state) noexcept { m_motionState = state; }
    void ResetHistory();
    [[nodiscard]] std::uint64_t GetHistoryResetCallCount() const noexcept { return m_historyResetCallCount; }
    [[nodiscard]] bool IsHistoryValid() const noexcept { return m_historyValid; }
    [[nodiscard]] std::uint32_t GetHistoryReadIndex() const noexcept { return m_historyReadIndex; }
    [[nodiscard]] TemporalAntiAliasingGraphContribution
        CreateRenderGraphContribution(std::uint32_t frameIndex);
    static TextureHandle AddPasses(
        RenderGraph& graph,
        TextureHandle currentColor,
        TextureHandle motionVectors,
        TextureHandle historyRead,
        TextureHandle& resolved,
        TextureHandle& historyWrite,
        RenderGraph::ParameterExecuteCallback execute);

    [[nodiscard]] bool IsInitialized() const;
    [[nodiscard]] DirectX::XMFLOAT2
        CalculateJitterNdc(
            std::uint64_t sampleIndex) const;

    [[nodiscard]] RHI::ITexture&
        GetMotionVectorTexture() const;
    [[nodiscard]] std::shared_ptr<RHI::ITexture>
        GetMotionVectorTextureShared() const;
    [[nodiscard]] RHI::ITextureView&
        GetMotionVectorRenderTargetView() const;
    [[nodiscard]] std::shared_ptr<RHI::ITexture>
        GetResolvedTexture() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetResolvedSampledView() const;
    [[nodiscard]] RHI::ITexture&
        GetHistoryReadTexture() const;
    [[nodiscard]] RHI::ITexture&
        GetHistoryWriteTexture() const;

    [[nodiscard]] RHI::ResourceState
        GetMotionInitialState() const;
    [[nodiscard]] RHI::ResourceState
        GetResolvedInitialState() const;
    [[nodiscard]] RHI::ResourceState
        GetHistoryReadInitialState() const;
    [[nodiscard]] RHI::ResourceState
        GetHistoryWriteInitialState() const;

private:
    struct alignas(16) Constants
    {
        DirectX::XMFLOAT2 resolution{};
        DirectX::XMFLOAT2 inverseResolution{};
        float feedback = 0.9f;
        std::uint32_t historyValid = 0;
        std::uint32_t enabled = 1;
        std::uint32_t padding = 0;
    };

    struct FrameResources
    {
        std::shared_ptr<RHI::IBuffer> constants;
        std::array<
            std::shared_ptr<RHI::IDescriptorSet>,
            2>
            descriptorSets;
    };

    void RebuildDescriptorSets();

    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::IDescriptorSetLayout>
        m_descriptorSetLayout;
    std::shared_ptr<RHI::IComputePipeline>
        m_pipeline;
    std::vector<FrameResources> m_frames;
    std::shared_ptr<RHI::ITexture> m_currentColor;
    std::shared_ptr<RHI::ITexture> m_motionVectors;
    std::shared_ptr<RHI::ITextureView>
        m_motionVectorRenderTargetView;
    std::shared_ptr<RHI::ITexture> m_resolved;
    std::shared_ptr<RHI::ITextureView>
        m_resolvedSampledView;
    std::shared_ptr<RHI::ITextureView>
        m_resolvedStorageView;
    std::array<std::shared_ptr<RHI::ITexture>, 2>
        m_history;
    std::array<std::shared_ptr<RHI::ITextureView>, 2>
        m_historyStorageViews;
    std::array<RHI::ResourceState, 2>
        m_historyStates{
            RHI::ResourceState::Undefined,
            RHI::ResourceState::Undefined};
    RHI::ResourceState m_motionState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState m_resolvedState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState m_createdInitialState =
        RHI::ResourceState::Undefined;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_historyReadIndex = 0;
    bool m_historyValid = false;
    std::uint64_t m_historyResetCallCount = 0;
};
} // namespace Prism::Renderer
