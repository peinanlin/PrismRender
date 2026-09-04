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
}

namespace Prism::Scene
{
class RenderSceneView;
}

namespace Prism::Renderer
{
class PipelineCache;

struct ClusteredLightingFeatureSlot
{
};

struct ClusteredLightingGraphContribution
{
    RHI::IBuffer* clusterConstants = nullptr;
    RHI::IBuffer* pointLights = nullptr;
    RHI::IBuffer* clusterLightCounts = nullptr;
    RHI::IBuffer* clusterLightIndices = nullptr;
    RHI::ResourceState clusterCountInitialState =
        RHI::ResourceState::UnorderedAccess;
    RHI::ResourceState clusterIndexInitialState =
        RHI::ResourceState::UnorderedAccess;
    RenderGraph::ParameterExecuteCallback execute;
};

class ClusteredLighting
{
public:
    static constexpr std::uint32_t TileSize = 16;
    static constexpr std::uint32_t DepthSliceCount = 16;
    static constexpr std::uint32_t MaxLightsPerCluster = 64;
    static constexpr std::uint32_t MaxLightCount = 128;

    void Initialize(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderPath,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight);
    void Resize(
        std::uint32_t width,
        std::uint32_t height);
    void Update(
        const Scene::RenderSceneView& scene,
        std::uint32_t frameIndex,
        bool enabled);
    void Execute(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex) const;
    void EndFrame(
        std::uint32_t frameIndex,
        bool executed);
    [[nodiscard]] ClusteredLightingGraphContribution
        CreateRenderGraphContribution(std::uint32_t frameIndex);
    static void AddPasses(
        RenderGraph& graph,
        BufferHandle clusterConstants,
        BufferHandle pointLights,
        BufferHandle& clusterLightCounts,
        BufferHandle& clusterLightIndices,
        RenderGraph::ParameterExecuteCallback execute);

    [[nodiscard]] RHI::IBuffer&
        GetConstantsBuffer(
            std::uint32_t frameIndex) const;
    [[nodiscard]] std::shared_ptr<RHI::IBuffer>
        GetConstantsBufferShared(
            std::uint32_t frameIndex) const;
    [[nodiscard]] RHI::IBuffer&
        GetLightBuffer(
            std::uint32_t frameIndex) const;
    [[nodiscard]] std::shared_ptr<RHI::IBuffer>
        GetLightBufferShared(
            std::uint32_t frameIndex) const;
    [[nodiscard]] RHI::IBuffer&
        GetClusterCountBuffer(
            std::uint32_t frameIndex) const;
    [[nodiscard]] std::shared_ptr<RHI::IBuffer>
        GetClusterCountBufferShared(
            std::uint32_t frameIndex) const;
    [[nodiscard]] RHI::IBuffer&
        GetClusterIndexBuffer(
            std::uint32_t frameIndex) const;
    [[nodiscard]] std::shared_ptr<RHI::IBuffer>
        GetClusterIndexBufferShared(
            std::uint32_t frameIndex) const;
    [[nodiscard]] RHI::ResourceState
        GetClusterCountInitialState(
            std::uint32_t frameIndex) const;
    [[nodiscard]] RHI::ResourceState
        GetClusterIndexInitialState(
            std::uint32_t frameIndex) const;
    [[nodiscard]] std::uint32_t
        GetClusterCount() const;

private:
    struct alignas(16) GpuPointLight
    {
        DirectX::XMFLOAT3 position{};
        float range = 0.0f;
        DirectX::XMFLOAT3 color{};
        float intensity = 0.0f;
    };

    struct alignas(16) Constants
    {
        DirectX::XMFLOAT4X4 worldToView{};
        DirectX::XMFLOAT4 viewportProjection{};
        DirectX::XMFLOAT4 depthParameters{};
        std::uint32_t tileCountX = 0;
        std::uint32_t tileCountY = 0;
        std::uint32_t depthSliceCount =
            DepthSliceCount;
        std::uint32_t lightCount = 0;
    };

    struct FrameResources
    {
        std::shared_ptr<RHI::IBuffer> constants;
        std::shared_ptr<RHI::IBuffer> lights;
        std::shared_ptr<RHI::IBuffer>
            clusterCounts;
        std::shared_ptr<RHI::IBuffer>
            clusterIndices;
        std::shared_ptr<RHI::IDescriptorSet>
            descriptorSet;
        RHI::ResourceState countState =
            RHI::ResourceState::UnorderedAccess;
        RHI::ResourceState indexState =
            RHI::ResourceState::UnorderedAccess;
    };

    void RebuildFrameResources();

    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::IDescriptorSetLayout>
        m_descriptorSetLayout;
    std::shared_ptr<RHI::IComputePipeline>
        m_pipeline;
    std::vector<FrameResources> m_frames;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_tileCountX = 0;
    std::uint32_t m_tileCountY = 0;
    std::uint32_t m_clusterCount = 0;
};
} // namespace Prism::Renderer
