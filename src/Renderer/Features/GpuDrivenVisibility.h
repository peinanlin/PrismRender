#pragma once

#include "Asset/ShaderManager.h"
#include "RHI/ICommandContext.h"
#include "Renderer/RenderGraph.h"
#include "Scene/Camera.h"
#include "Scene/RenderObject.h"
#include "Scene/RenderViewFeedback.h"

#include <DirectXMath.h>

#include <cstddef>
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
}

namespace Prism::Scene
{
class Camera;
class RenderSceneView;
}

namespace Prism::Renderer
{
class PipelineCache;

struct GpuDrivenVisibilityFeatureSlot
{
};

struct GpuDrivenVisibilityGraphContribution
{
    RHI::IBuffer* objectRecords = nullptr;
    RHI::IBuffer* indirectArguments = nullptr;
    RHI::IBuffer* indirectDrawCounts = nullptr;
    RHI::ResourceState objectInitialState =
        RHI::ResourceState::UnorderedAccess;
    RHI::ResourceState indirectInitialState =
        RHI::ResourceState::UnorderedAccess;
    RHI::ResourceState countInitialState =
        RHI::ResourceState::UnorderedAccess;
    RenderGraph::ParameterExecuteCallback execute;
};

struct GpuVisibilityStatistics
{
    std::uint32_t candidateObjects = 0;
    std::uint32_t visibleObjects = 0;
    std::uint32_t lodRejectedObjects = 0;
    std::uint32_t frustumCulledObjects = 0;
    std::uint32_t occlusionCulledObjects = 0;
    std::uint32_t disabledObjects = 0;
    bool valid = false;
};

struct GpuDrawBatch
{
    std::uint32_t representativeObjectIndex = 0;
    std::uint32_t firstArgument = 0;
    std::uint32_t maxDrawCount = 0;
};

class GpuDrivenVisibility
{
public:
    void Initialize(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderPath,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight,
        std::uint32_t maxObjectCount);
    void Update(
        const Scene::RenderSceneView& scene,
        const Scene::Camera& cullingCamera,
        std::uint32_t frameIndex,
        bool frustumCullingEnabled,
        bool occlusionCullingEnabled,
        bool hiZValid,
        bool diagnosticsReadbackEnabled,
        std::uint32_t viewportWidth,
        std::uint32_t viewportHeight,
        float oceanDisplacementMargin = 0.0f);
    void SetOcclusionTexture(
        std::shared_ptr<RHI::ITexture> texture);
    void NotifySceneChanged();
    void Execute(
        RHI::ICommandContext& commandContext,
        std::uint32_t frameIndex);
    [[nodiscard]] GpuDrivenVisibilityGraphContribution
        CreateRenderGraphContribution(std::uint32_t frameIndex);
    static void AddPasses(
        RenderGraph& graph,
        BufferHandle gpuObjectRecords,
        TextureHandle hiZ,
        BufferHandle& indirectArguments,
        BufferHandle& indirectDrawCounts,
        RenderGraph::ParameterExecuteCallback execute);

    [[nodiscard]] bool IsInitialized() const;
    [[nodiscard]] std::uint32_t
        GetObjectCount() const;
    [[nodiscard]] RHI::IBuffer&
        GetObjectBuffer() const;
    [[nodiscard]] RHI::IBuffer&
        GetArgumentBuffer(
            std::uint32_t frameIndex) const;
    [[nodiscard]] RHI::IBuffer&
        GetCountBuffer(
            std::uint32_t frameIndex) const;
    [[nodiscard]] RHI::ResourceState
        GetArgumentInitialState(
            std::uint32_t frameIndex) const;
    [[nodiscard]] RHI::ResourceState
        GetCountInitialState(
            std::uint32_t frameIndex) const;
    [[nodiscard]] const std::vector<GpuDrawBatch>&
        GetDrawBatches() const;
    [[nodiscard]] const GpuDrawBatch&
        GetDrawBatchForObject(
            std::size_t objectIndex) const;
    [[nodiscard]] const GpuVisibilityStatistics&
        GetStatistics() const;
    [[nodiscard]] const std::vector<
        Scene::GpuVisibilityReason>&
        GetResolvedReasons() const;
    [[nodiscard]] const Scene::Camera*
        GetResolvedCullingCamera() const;
    [[nodiscard]] const std::shared_ptr<const Scene::RenderViewFeedback>&
        GetResolvedFeedback() const noexcept;
    [[nodiscard]] static constexpr std::size_t
        GetArgumentStride()
    {
        return sizeof(
            RHI::DrawIndexedIndirectArguments);
    }

private:
    struct alignas(16) ObjectRecord
    {
        DirectX::XMFLOAT4 centerRadius{};
        std::uint32_t indexCount = 0;
        std::uint32_t firstIndex = 0;
        std::int32_t vertexOffset = 0;
        std::uint32_t firstInstance = 0;
        std::uint32_t enabled = 0;
        std::uint32_t drawBatchIndex = 0;
        std::uint32_t drawArgumentOffset = 0;
        std::uint32_t padding = 0;
        DirectX::XMFLOAT4 lodData{};
        std::uint32_t parentObjectIndex =
            0xffffffffu;
        std::uint32_t hierarchyEnabled = 0;
        std::uint32_t hierarchyPadding[2]{};
    };

    struct alignas(16) CullingConstants
    {
        DirectX::XMFLOAT4 frustumPlanes[6]{};
        DirectX::XMFLOAT4X4 worldToView{};
        DirectX::XMFLOAT4X4 viewProjection{};
        DirectX::XMFLOAT4 viewportProjection{};
        std::uint32_t objectCount = 0;
        std::uint32_t frustumCullingEnabled = 0;
        std::uint32_t occlusionCullingEnabled = 0;
        float occlusionDepthBias = 0.0015f;
        std::uint32_t drawBatchCount = 0;
        std::uint32_t padding[3]{};
    };

    static_assert(
        sizeof(ObjectRecord) == 80);

    struct FrameResources
    {
        std::shared_ptr<RHI::IBuffer>
            constants;
        std::shared_ptr<RHI::IBuffer>
            arguments;
        std::shared_ptr<RHI::IBuffer>
            visibilityResults;
        std::shared_ptr<RHI::IBuffer>
            drawCounts;
        std::shared_ptr<RHI::IBuffer>
            visibilityReadback;
        std::shared_ptr<RHI::IDescriptorSet>
            descriptorSet;
        bool visibilityResultsCopied = false;
        bool readbackPending = false;
        std::uint32_t readbackObjectCount = 0;
        Scene::Camera cullingCameraSnapshot{};
        bool cullingCameraSnapshotValid = false;
        Scene::RenderViewFeedbackIdentity feedbackIdentity{};
        bool feedbackIdentityValid = false;
    };

    void RebuildSceneResources(
        std::vector<ObjectRecord> records);
    void ResolveReadback(
        std::uint32_t frameIndex);

    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::IDescriptorSetLayout>
        m_descriptorSetLayout;
    std::shared_ptr<RHI::IComputePipeline>
        m_pipeline;
    std::shared_ptr<RHI::IComputePipeline>
        m_clearCountsPipeline;
    std::shared_ptr<RHI::IBuffer> m_objectBuffer;
    std::shared_ptr<RHI::ITexture>
        m_occlusionTexture;
    std::vector<FrameResources> m_frames;
    mutable std::vector<bool>
        m_frameArgumentsExecuted;
    mutable std::vector<bool>
        m_frameCountsExecuted;
    std::vector<ObjectRecord> m_records;
    std::vector<GpuDrawBatch> m_drawBatches;
    std::vector<std::uint32_t>
        m_objectBatchIndices;
    std::vector<Scene::GpuVisibilityReason>
        m_resolvedReasons;
    GpuVisibilityStatistics m_statistics{};
    Scene::Camera m_resolvedCullingCamera{};
    bool m_resolvedCullingCameraValid = false;
    std::shared_ptr<const Scene::RenderViewFeedback>
        m_resolvedFeedback;
    std::uint32_t m_maxObjectCount = 0;
    std::uint32_t m_objectCount = 0;
    bool m_diagnosticsReadbackEnabled = false;
};
} // namespace Prism::Renderer
