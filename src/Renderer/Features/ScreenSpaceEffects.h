#pragma once

#include "Asset/ShaderManager.h"
#include "RHI/ICommandContext.h"
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
class ITexture;
class ITextureView;
}

namespace Prism::Renderer
{
class PipelineCache;

struct ScreenSpaceEffectsFeatureSlot
{
};

struct ScreenSpaceEffectsGraphContribution
{
    RHI::ITexture* ambientOcclusion = nullptr;
    RHI::ITexture* composite = nullptr;
    RHI::ResourceState ambientOcclusionInitialState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState compositeInitialState =
        RHI::ResourceState::Undefined;
    RenderGraph::ParameterExecuteCallback gtao;
    RenderGraph::ParameterExecuteCallback reflections;
};

class ScreenSpaceEffects
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
        std::shared_ptr<RHI::ITexture> positionRoughness,
        std::shared_ptr<RHI::ITexture> normalMetallic,
        std::shared_ptr<RHI::ITexture> hdrColor,
        std::shared_ptr<RHI::ITexture> hiZ,
        std::shared_ptr<RHI::ITexture> planarReflection);
    void Update(
        std::uint32_t frameIndex,
        const DirectX::XMMATRIX& viewProjection,
        const DirectX::XMMATRIX& planarViewProjection,
        const DirectX::XMFLOAT3& cameraPosition,
        bool deferredEnabled,
        bool gtaoEnabled,
        bool reflectionsEnabled,
        bool planarReflectionsEnabled,
        float reflectionPlaneHeight,
        float planarReflectionIntensity);
    void ExecuteGtao(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void ExecuteReflections(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void EndFrame(
        bool gtaoExecuted,
        bool compositeExecuted);
    [[nodiscard]] ScreenSpaceEffectsGraphContribution
        CreateRenderGraphContribution(std::uint32_t frameIndex);
    static void AddGtaoPasses(
        RenderGraph& graph,
        TextureHandle positionRoughness,
        TextureHandle normalMetallic,
        TextureHandle& ambientOcclusion,
        RenderGraph::ParameterExecuteCallback execute);
    static void AddCompositePasses(
        RenderGraph& graph,
        TextureHandle hdrColor,
        TextureHandle hiZ,
        TextureHandle positionRoughness,
        TextureHandle normalMetallic,
        TextureHandle ambientOcclusion,
        TextureHandle planarReflection,
        bool readAmbientOcclusion,
        bool readPlanarReflection,
        TextureHandle& output,
        RenderGraph::ParameterExecuteCallback execute);

    [[nodiscard]] RHI::ITexture&
        GetAmbientOcclusionTexture() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetAmbientOcclusionSampledView(bool enabled) const;
    [[nodiscard]] std::shared_ptr<RHI::ITexture>
        GetCompositeTexture() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetCompositeSampledView() const;
    [[nodiscard]] RHI::ResourceState
        GetAmbientOcclusionInitialState() const;
    [[nodiscard]] RHI::ResourceState
        GetCompositeInitialState() const;

private:
    struct alignas(16) Constants
    {
        DirectX::XMFLOAT4X4 viewProjection{};
        DirectX::XMFLOAT4X4 planarViewProjection{};
        DirectX::XMFLOAT3 cameraPosition{};
        std::uint32_t deferredEnabled = 1;
        DirectX::XMFLOAT2 resolution{};
        DirectX::XMFLOAT2 inverseResolution{};
        float gtaoRadiusPixels = 14.0f;
        float gtaoStrength = 1.25f;
        float ssrMaxDistance = 24.0f;
        float ssrThickness = 0.45f;
        float ssrStride = 0.45f;
        std::uint32_t gtaoEnabled = 1;
        std::uint32_t reflectionsEnabled = 1;
        std::uint32_t planarReflectionsEnabled = 0;
        float reflectionPlaneHeight = 0.0f;
        float planarReflectionIntensity = 0.8f;
        DirectX::XMFLOAT2 padding{};
    };

    struct FrameResources
    {
        std::shared_ptr<RHI::IBuffer> constants;
        std::shared_ptr<RHI::IDescriptorSet>
            gtaoDescriptorSet;
        std::shared_ptr<RHI::IDescriptorSet>
            reflectionDescriptorSet;
    };

    void RebuildDescriptorSets();

    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::IDescriptorSetLayout>
        m_gtaoLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout>
        m_reflectionLayout;
    std::shared_ptr<RHI::IComputePipeline>
        m_gtaoPipeline;
    std::shared_ptr<RHI::IComputePipeline>
        m_reflectionPipeline;
    std::vector<FrameResources> m_frames;
    std::shared_ptr<RHI::ITexture>
        m_positionRoughness;
    std::shared_ptr<RHI::ITexture>
        m_normalMetallic;
    std::shared_ptr<RHI::ITexture> m_hdrColor;
    std::shared_ptr<RHI::ITexture> m_hiZ;
    std::shared_ptr<RHI::ITexture> m_planarReflection;
    std::shared_ptr<RHI::ITexture> m_disabledPlanarReflection;
    std::shared_ptr<RHI::ITextureView> m_disabledAmbientOcclusion;
    std::shared_ptr<RHI::ITextureView>
        m_hiZFullSampledView;
    std::shared_ptr<RHI::ITexture>
        m_ambientOcclusion;
    std::shared_ptr<RHI::ITextureView>
        m_ambientOcclusionSampledView;
    std::shared_ptr<RHI::ITextureView>
        m_ambientOcclusionStorageView;
    std::shared_ptr<RHI::ITexture> m_composite;
    std::shared_ptr<RHI::ITextureView>
        m_compositeSampledView;
    std::shared_ptr<RHI::ITextureView>
        m_compositeStorageView;
    RHI::ResourceState m_ambientOcclusionState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState m_compositeState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState m_createdInitialState =
        RHI::ResourceState::Undefined;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
};
} // namespace Prism::Renderer
