#pragma once

#include "Asset/ShaderManager.h"
#include "RHI/ICommandContext.h"
#include "Renderer/RenderGraph.h"
#include "Renderer/SharedRenderData.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace Prism::RHI
{
class IBuffer;
class IDescriptorSet;
class IDescriptorSetLayout;
class IGraphicsDevice;
class IGraphicsPipeline;
class ISampler;
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

struct PlanarReflectionsFeatureSlot
{
};

struct PlanarReflectionsGraphContribution
{
    RHI::ITexture* color = nullptr;
    RHI::ITexture* depth = nullptr;
    RHI::ResourceState colorInitialState =
        RHI::ResourceState::ShaderResource;
    RHI::ResourceState depthInitialState =
        RHI::ResourceState::DepthWrite;
    RenderGraph::ParameterExecuteCallback execute;
};

class PlanarReflections
{
public:
    void Initialize(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderPath,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight,
        std::uint32_t maxRenderObjects);
    void Resize(std::uint32_t width, std::uint32_t height);
    void Update(
        const Scene::RenderSceneView& scene,
        std::uint32_t frameIndex,
        float planeHeight,
        bool enabled,
        const DirectX::XMFLOAT3& clearColor);
    void Execute(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex,
        const Scene::RenderSceneView& scene) const;
    void EndFrame();
    [[nodiscard]] PlanarReflectionsGraphContribution
        CreateRenderGraphContribution(
        std::uint32_t frameIndex,
        std::shared_ptr<const Scene::RenderSceneView> scene);
    static void AddPasses(
        RenderGraph& graph,
        TextureHandle& color,
        TextureHandle& depth,
        RenderGraph::ParameterExecuteCallback execute,
        TextureHandle environment = {});

    [[nodiscard]] RHI::ITexture& GetColorTexture() const;
    [[nodiscard]] RHI::ITexture& GetDepthTexture() const;
    [[nodiscard]] std::shared_ptr<RHI::ITexture>
        GetColorTextureShared() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetColorSampledView() const;
    [[nodiscard]] RHI::ResourceState GetColorInitialState() const;
    [[nodiscard]] RHI::ResourceState GetDepthInitialState() const;
    [[nodiscard]] const DirectX::XMMATRIX&
        GetReflectedViewProjection() const;
    [[nodiscard]] float GetPlaneHeight() const;

private:
    static constexpr std::uint32_t ConstantStride = 256;

    struct alignas(16) FrameConstants
    {
        DirectX::XMFLOAT4X4 reflectedViewProjection{};
        DirectX::XMFLOAT3 cameraPosition{};
        float planeHeight = 0.0f;
        DirectX::XMFLOAT3 lightDirection{};
        float lightIntensity = 1.0f;
        DirectX::XMFLOAT3 lightColor{1.0f, 1.0f, 1.0f};
        float enabled = 0.0f;
    };

    struct alignas(16) ObjectConstants
    {
        DirectX::XMFLOAT4X4 world{};
    };

    struct FrameResources
    {
        std::shared_ptr<RHI::IBuffer> frameConstants;
        std::shared_ptr<RHI::IBuffer> objectConstants;
        std::shared_ptr<RHI::IBuffer> materialConstants;
        std::vector<std::shared_ptr<RHI::IDescriptorSet>> descriptorSets;
    };

    void EnsureSceneDescriptors(const Scene::RenderSceneView& scene);

    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_layout;
    std::shared_ptr<RHI::IGraphicsPipeline> m_pipeline;
    std::shared_ptr<RHI::ISampler> m_sampler;
    std::vector<FrameResources> m_frames;
    std::vector<std::uint64_t> m_materialIdentities;
    std::shared_ptr<RHI::ITexture> m_colorTexture;
    std::shared_ptr<RHI::ITexture> m_depthTexture;
    std::shared_ptr<RHI::ITextureView> m_colorTargetView;
    std::shared_ptr<RHI::ITextureView> m_colorSampledView;
    std::shared_ptr<RHI::ITextureView> m_depthView;
    DirectX::XMMATRIX m_reflectedViewProjection =
        DirectX::XMMatrixIdentity();
    DirectX::XMFLOAT3 m_clearColor{};
    RHI::ResourceState m_colorState = RHI::ResourceState::Undefined;
    RHI::ResourceState m_depthState = RHI::ResourceState::Undefined;
    RHI::ResourceState m_createdInitialState = RHI::ResourceState::Undefined;
    std::uint32_t m_maxRenderObjects = 0;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    float m_planeHeight = 0.0f;
    bool m_enabled = false;
};
} // namespace Prism::Renderer
