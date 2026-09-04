#pragma once

#include "Asset/ShaderManager.h"
#include "RHI/GraphicsResources.h"
#include "Renderer/RenderGraph.h"

#include <DirectXMath.h>

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
class ICommandContext;
class ITexture;
class ITextureView;
}

namespace Prism::Renderer
{
class PipelineCache;

// Receiver-space image-space refracted-photon gather for the particle-fluid
// surface. This is deliberately not the reference implementation's light-space
// photon VBO/splat path: receiver pixels gather a bounded set of nearby fluid
// samples and test their refracted rays. The feature produces an additive HDR
// texture so the caller can compose it with the opaque receiver lighting.
class FluidCaustics
{
public:
    static constexpr std::uint32_t ThreadGroupSize = 8;
    static constexpr std::uint32_t MaximumBlurRadius = 12;

    struct Parameters
    {
        DirectX::XMMATRIX projection =
            DirectX::XMMatrixIdentity();
        DirectX::XMFLOAT3 lightDirectionView{
            0.0f, 0.0f, 1.0f};
        float indexOfRefraction = 1.333f;
        DirectX::XMFLOAT3 receiverUpDirectionView{
            0.0f, 1.0f, 0.0f};
        bool enabled = false;
        float intensity = 1.35f;
        float refractionScalePixels = 14.0f;
        float depthAttenuation = 0.12f;
        float focusStrength = 5.0f;
        float focusPower = 1.5f;
        float depthBias = 0.0005f;
        std::uint32_t blurRadius = 4;
        float blurSigma = 2.0f;
        DirectX::XMFLOAT3 tint{0.62f, 0.88f, 1.0f};
    };

    void Initialize(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderPath,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight);

    // Input contract:
    // - fluidDepth contains positive view-space linear depth; zero means that
    //   no fluid covers the pixel.
    // - sceneDepth contains standard device depth (zero near, one far); one
    //   means that no scene receiver covers the pixel. The fluid surface pass
    //   already rejects particles occluded by this scene depth.
    // - fluidNormal contains a signed, normalized view-space normal.
    // A sampled view is accepted instead of a texture so depth-aspect views
    // and a Hi-Z mip-zero view can be supplied without feature-specific code.
    void Resize(
        std::uint32_t width,
        std::uint32_t height,
        std::shared_ptr<RHI::ITextureView> fluidDepth,
        std::shared_ptr<RHI::ITextureView> fluidNormal,
        std::shared_ptr<RHI::ITextureView> sceneDepth);

    void Update(
        std::uint32_t frameIndex,
        const Parameters& parameters);

    void ExecuteGenerate(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteBlurHorizontal(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteBlurVertical(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;

    // Adds three separately profiled passes. The final `composite` handle is
    // an additive HDR caustics texture (black when the feature is disabled).
    static void AddPasses(
        RenderGraph& graph,
        TextureHandle fluidDepth,
        TextureHandle fluidNormal,
        TextureHandle sceneDepth,
        TextureHandle& raw,
        TextureHandle& blurPing,
        TextureHandle& composite,
        RenderGraph::ParameterExecuteCallback generate,
        RenderGraph::ParameterExecuteCallback blurHorizontal,
        RenderGraph::ParameterExecuteCallback blurVertical,
        RenderGraph::QueueClass queue =
            RenderGraph::QueueClass::Compute);

    // `passesExecuted` tracks the generation/blur chain. `compositeRead`
    // separately tracks the statically bound fallback SRV used when that chain
    // is pruned, so its persistent state remains correct on the next frame.
    void EndFrame(bool passesExecuted, bool compositeRead);

    [[nodiscard]] std::shared_ptr<RHI::ITexture>
        GetRawTexture() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetRawSampledView() const;
    [[nodiscard]] std::shared_ptr<RHI::ITexture>
        GetBlurPingTexture() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetBlurPingSampledView() const;
    [[nodiscard]] std::shared_ptr<RHI::ITexture>
        GetCompositeTexture() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetCompositeSampledView() const;

    [[nodiscard]] RHI::ResourceState
        GetRawInitialState() const;
    [[nodiscard]] RHI::ResourceState
        GetBlurPingInitialState() const;
    [[nodiscard]] RHI::ResourceState
        GetCompositeInitialState() const;
    [[nodiscard]] bool IsInitialized() const;
    [[nodiscard]] bool IsReady() const;

private:
    struct alignas(16) Constants
    {
        DirectX::XMFLOAT4X4 inverseProjection{};
        DirectX::XMFLOAT2 resolution{};
        DirectX::XMFLOAT2 inverseResolution{};
        DirectX::XMFLOAT3 tint{0.62f, 0.88f, 1.0f};
        float intensity = 1.35f;
        DirectX::XMFLOAT3 lightDirectionView{
            0.0f, 0.0f, 1.0f};
        float indexOfRefraction = 1.333f;
        DirectX::XMFLOAT3 receiverUpDirectionView{
            0.0f, 1.0f, 0.0f};
        float receiverPlanePadding = 0.0f;
        float refractionScalePixels = 14.0f;
        float depthAttenuation = 0.12f;
        float focusStrength = 5.0f;
        float focusPower = 1.5f;
        float depthBias = 0.0005f;
        float blurSigma = 2.0f;
        std::uint32_t blurRadius = 4;
        std::uint32_t enabled = 0;
    };
    static_assert(
        sizeof(Constants) == 160,
        "Fluid caustics constants must match the HLSL cbuffer layout.");

    struct FrameResources
    {
        std::shared_ptr<RHI::IBuffer> constants;
        std::shared_ptr<RHI::IDescriptorSet> generateSet;
        std::shared_ptr<RHI::IDescriptorSet> blurHorizontalSet;
        std::shared_ptr<RHI::IDescriptorSet> blurVerticalSet;
    };

    void RebuildDescriptorSets();
    void Dispatch(
        RHI::ICommandContext& commandContext,
        const RHI::IComputePipeline& pipeline,
        const RHI::IDescriptorSet& descriptorSet) const;

    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_generateLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_blurHorizontalLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_blurVerticalLayout;
    std::shared_ptr<RHI::IComputePipeline> m_generatePipeline;
    std::shared_ptr<RHI::IComputePipeline> m_blurHorizontalPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_blurVerticalPipeline;
    std::vector<FrameResources> m_frames;

    std::shared_ptr<RHI::ITextureView> m_fluidDepth;
    std::shared_ptr<RHI::ITextureView> m_fluidNormal;
    std::shared_ptr<RHI::ITextureView> m_sceneDepth;

    std::shared_ptr<RHI::ITexture> m_raw;
    std::shared_ptr<RHI::ITextureView> m_rawSampledView;
    std::shared_ptr<RHI::ITextureView> m_rawStorageView;
    std::shared_ptr<RHI::ITexture> m_blurPing;
    std::shared_ptr<RHI::ITextureView> m_blurPingSampledView;
    std::shared_ptr<RHI::ITextureView> m_blurPingStorageView;
    std::shared_ptr<RHI::ITexture> m_composite;
    std::shared_ptr<RHI::ITextureView> m_compositeSampledView;
    std::shared_ptr<RHI::ITextureView> m_compositeStorageView;

    RHI::ResourceState m_rawState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState m_blurPingState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState m_compositeState =
        RHI::ResourceState::Undefined;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
};
} // namespace Prism::Renderer
