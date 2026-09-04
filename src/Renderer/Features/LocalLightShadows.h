#pragma once

#include "Asset/ShaderManager.h"
#include "RHI/ICommandContext.h"
#include "Renderer/RenderGraph.h"

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Prism::RHI
{
class IBuffer;
class IDescriptorSet;
class IDescriptorSetLayout;
class IGraphicsDevice;
class IGraphicsPipeline;
class ITexture;
class ITextureView;
}

namespace Prism::Scene
{
class RenderSceneView;
}

namespace Prism::Renderer
{
class PipelineCache;

struct LocalLightShadowsFeatureSlot
{
};

struct LocalLightShadowsGraphContribution
{
    RHI::IBuffer* lightingConstants = nullptr;
    RHI::IBuffer* renderConstants = nullptr;
    RHI::ITexture* spotShadowMap = nullptr;
    RHI::ITexture* pointShadowMap = nullptr;
    RHI::ResourceState spotShadowInitialState =
        RHI::ResourceState::ShaderResource;
    RHI::ResourceState pointShadowInitialState =
        RHI::ResourceState::ShaderResource;
    RenderGraph::ParameterExecuteCallback spotExecute;
    RenderGraph::ParameterExecuteCallback pointExecute;
};

struct LocalShadowStatistics
{
    std::uint32_t drawCalls = 0;
    std::uint32_t renderedLayers = 0;
    std::uint32_t cachedLayers = 0;
    std::uint32_t casterCandidates = 0;
    std::uint32_t culledCasters = 0;
};

class LocalLightShadows
{
public:
    static constexpr std::uint32_t
        MaxShadowedSpotLights = 4;
    static constexpr std::uint32_t
        MaxShadowedPointLights = 4;
    static constexpr std::uint32_t
        PointFaceCount = 6;
    static constexpr std::uint32_t
        ShadowResolution = 1024;
    static constexpr std::uint32_t
        TotalShadowLayerCount =
            MaxShadowedSpotLights
            + MaxShadowedPointLights
                * PointFaceCount;

    void Initialize(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderPath,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight,
        std::uint32_t maxRenderObjects);
    void Update(
        const Scene::RenderSceneView& scene,
        std::uint32_t frameIndex,
        bool spotLightsEnabled,
        bool shadowsEnabled);
    void ExecuteSpotShadows(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex,
        const Scene::RenderSceneView& scene);
    void ExecutePointShadows(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex,
        const Scene::RenderSceneView& scene);
    void EndFrame(bool executed);
    [[nodiscard]] LocalLightShadowsGraphContribution
        CreateRenderGraphContribution(
        std::uint32_t frameIndex,
        std::shared_ptr<const Scene::RenderSceneView> scene);
    static void AddPasses(
        RenderGraph& graph,
        BufferHandle renderConstants,
        TextureHandle& spotShadowMap,
        TextureHandle& pointShadowMap,
        bool spotEnabled,
        bool pointEnabled,
        RenderGraph::ParameterExecuteCallback spotExecute,
        RenderGraph::ParameterExecuteCallback pointExecute);

    [[nodiscard]] std::shared_ptr<RHI::IBuffer>
        GetLightingConstants(
            std::uint32_t frameIndex) const;
    [[nodiscard]] std::shared_ptr<RHI::IBuffer>
        GetRenderConstants(
            std::uint32_t frameIndex) const;
    [[nodiscard]] RHI::ITexture&
        GetSpotShadowTexture() const;
    [[nodiscard]] RHI::ITexture&
        GetPointShadowTexture() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetSpotShadowSampledView() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetPointShadowSampledView() const;
    [[nodiscard]] RHI::ResourceState
        GetSpotShadowInitialState() const;
    [[nodiscard]] RHI::ResourceState
        GetPointShadowInitialState() const;
    [[nodiscard]] const LocalShadowStatistics&
        GetStatistics() const;

private:
    struct alignas(16) SpotLightData
    {
        DirectX::XMFLOAT4X4 viewProjection{};
        DirectX::XMFLOAT4 positionRange{};
        DirectX::XMFLOAT4 directionOuterCos{};
        DirectX::XMFLOAT4 colorIntensity{};
        DirectX::XMFLOAT4 parameters{};
    };

    struct alignas(16) PointShadowData
    {
        DirectX::XMFLOAT4 positionRange{};
        DirectX::XMFLOAT4 colorIntensity{};
        DirectX::XMFLOAT4 parameters{};
    };

    struct alignas(16) LightingConstants
    {
        std::array<
            SpotLightData,
            MaxShadowedSpotLights>
            spotLights{};
        std::array<
            PointShadowData,
            MaxShadowedPointLights>
            pointShadows{};
        DirectX::XMFLOAT4 countsAndNear{};
    };

    struct alignas(16) RenderConstants
    {
        DirectX::XMFLOAT4X4
            worldViewProjection{};
    };

    struct FrameResources
    {
        std::shared_ptr<RHI::IBuffer>
            lightingConstants;
        std::shared_ptr<RHI::IBuffer>
            renderConstants;
        std::shared_ptr<RHI::IDescriptorSet>
            renderDescriptorSet;
    };

    [[nodiscard]] std::size_t
        GetRenderConstantOffset(
            std::uint32_t passIndex,
            std::uint32_t objectIndex) const;
    void ExecuteShadowLayers(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex,
        const Scene::RenderSceneView& scene,
        std::uint32_t firstPass,
        std::span<const bool> activeLayers,
        std::span<const std::shared_ptr<
            RHI::ITextureView>> depthViews,
        std::string_view labelPrefix);

    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::IDescriptorSetLayout>
        m_renderLayout;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_pipeline;
    std::vector<FrameResources> m_frames;
    std::shared_ptr<RHI::ITexture>
        m_spotShadowTexture;
    std::shared_ptr<RHI::ITexture>
        m_pointShadowTexture;
    std::shared_ptr<RHI::ITextureView>
        m_spotShadowSampledView;
    std::shared_ptr<RHI::ITextureView>
        m_pointShadowSampledView;
    std::array<
        std::shared_ptr<RHI::ITextureView>,
        MaxShadowedSpotLights>
        m_spotDepthViews;
    std::array<
        std::shared_ptr<RHI::ITextureView>,
        MaxShadowedPointLights
            * PointFaceCount>
        m_pointDepthViews;
    std::array<bool, MaxShadowedSpotLights>
        m_activeSpotLayers{};
    std::array<
        bool,
        MaxShadowedPointLights
            * PointFaceCount>
        m_activePointLayers{};
    std::array<
        std::vector<std::uint32_t>,
        TotalShadowLayerCount>
        m_shadowCasters;
    std::array<std::uint64_t,
               TotalShadowLayerCount>
        m_cachedSignatures{};
    std::array<std::uint64_t,
               TotalShadowLayerCount>
        m_pendingSignatures{};
    std::array<bool, TotalShadowLayerCount>
        m_cacheValid{};
    std::array<bool, TotalShadowLayerCount>
        m_renderRequired{};
    LocalShadowStatistics m_statistics{};
    RHI::ResourceState m_spotState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState m_pointState =
        RHI::ResourceState::Undefined;
    std::uint32_t m_maxRenderObjects = 0;
    std::uint32_t m_alignedRenderConstantSize =
        256;
};
} // namespace Prism::Renderer
