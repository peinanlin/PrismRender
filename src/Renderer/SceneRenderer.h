#pragma once

#include "Asset/ShaderManager.h"
#include "Renderer/RenderGraph.h"
#include "Renderer/Features/ClusteredLighting.h"
#include "Renderer/Features/BuiltInRenderFeatures.h"
#include "RHI/Profiling/GpuProfiler.h"
#include "Renderer/Features/GpuDrivenVisibility.h"
#include "Renderer/Features/FftOcean.h"
#include "Renderer/Features/Ocean/LocalWaveGpuResources.h"
#include "Renderer/Features/Ocean/LocalWaveSimulation.h"
#include "Renderer/Features/Ocean/OceanQueryService.h"
#include "Renderer/Features/Ocean/OceanGpuQuery.h"
#include "Renderer/Features/Ocean/OceanSurfaceRenderer.h"
#include "Renderer/Features/Ocean/SpectralOceanSimulation.h"
#include "Renderer/Features/Ocean/WaterOpticsFeature.h"
#include "Renderer/Features/Fluid/FluidFeature.h"
#include "Renderer/Features/LocalLightShadows.h"
#include "Renderer/Features/PlanarReflections.h"
#include "Renderer/QueueSchedulingCostModel.h"
#include "Renderer/RenderFeatureRegistry.h"
#include "Renderer/Pipeline/PipelineCache.h"
#include "Renderer/Pipeline/ScenePipelinePlan.h"
#include "Renderer/RhiPasses.h"
#include "Renderer/RenderSettings.h"
#include "Renderer/RendererStatistics.h"
#include "Renderer/RenderCapture.h"
#include "Renderer/SharedRenderData.h"
#include "Renderer/SharedRenderGraphFrontend.h"
#include "Renderer/Features/ScreenSpaceEffects.h"
#include "Renderer/Features/SkyAtmosphere.h"
#include "Renderer/Features/TemporalAntiAliasing.h"
#include "Renderer/Features/VarianceShadowMaps.h"
#include "Renderer/Features/VirtualTextureCache.h"
#include "RHI/GraphicsResources.h"
#include "RHI/ShaderTypes.h"
#include "RHI/TransientResources.h"

#include <DirectXMath.h>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Prism::Asset
{
class Material;
class Mesh;
class Texture;
}

namespace Prism::Core
{
class ITaskExecutor;
}

namespace Prism::RHI
{
class IFrameContext;
class IGraphicsDevice;
class IRenderBackend;
}

namespace Prism::Scene
{
class RenderFramePacket;
class RenderScene;
class RenderSceneView;
struct RenderViewId;
}

namespace Prism::Renderer
{
class SceneRendererSharedResources;
struct RenderViewDiagnostics;

class SceneRenderer
{
public:
    SceneRenderer() = default;
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    void Initialize(
        RHI::IRenderBackend& backend,
        const std::filesystem::path& shaderPath,
        const Scene::RenderScene& scene,
        std::shared_ptr<Asset::Texture> environmentCubemap,
        bool renderToSwapChain = true,
        std::shared_ptr<SceneRendererSharedResources>
            sharedResources = nullptr,
        bool viewIndependentSimulationProducer = true,
        RenderViewId viewId = 1);
    void ReleaseSwapChainResources();
    void RecreateSwapChainResources(
        RHI::IRenderBackend& backend,
        bool resizeSharedFeatures = true);
    void Render(
        RHI::IRenderBackend& backend,
        std::shared_ptr<const Scene::RenderFramePacket> packet,
        Scene::RenderViewId viewId,
        std::shared_ptr<const Scene::RenderViewFeedback>
            visibilityFeedback = {});
    void SetSharedSimulationProducerForFrame(
        LogicalFrameId logicalFrameId,
        bool producer);
    void NotifySceneChanged(
        const Scene::RenderScene& scene,
        bool notifySharedFeatures = true);
    void RequestHistoryReset() noexcept;
    void SetFrameDiagnosticsEnabled(bool enabled) noexcept { m_frameDiagnosticsEnabled = enabled; }
    [[nodiscard]] bool IsFrameDiagnosticsEnabled() const noexcept
    {
        return m_frameDiagnosticsEnabled;
    }
    void SetProfilingMode(
        RHI::GpuProfiler::SamplingMode mode) noexcept;
    void SetTaskExecutor(Core::ITaskExecutor& taskExecutor) noexcept;
    [[nodiscard]] RHI::GpuProfiler::SamplingMode
        GetProfilingMode() const noexcept;
    [[nodiscard]] RenderViewDiagnostics BuildFrameDiagnostics(RHI::GraphicsApi api) const;
    const RenderGraph& GetRenderGraph() const;
    const std::vector<RHI::GpuProfiler::Timing>& GetGpuTimings() const;
    [[nodiscard]] std::uint64_t GetGpuTimingGeneration() const noexcept;
    [[nodiscard]] std::uint32_t GetGpuTimingFrameSlot() const noexcept;
    const RHI::GpuProfiler::TimelineMetadata&
        GetGpuTimelineMetadata() const;
    void ResolveGpuTimings(
        RHI::IRenderBackend& backend);
    RenderSettings& GetSettings();
    const RenderSettings& GetSettings() const;
    // View-level policy: Game/Final Output must never render editor ground
    // grids even when the shared scene setting enables one for Scene View.
    void SetEditorGridAllowed(bool allowed);
    [[nodiscard]] bool SupportsGpuDrivenRendering() const;
    const RendererStatistics& GetStatistics() const;
    [[nodiscard]] bool QueueOceanDisplacementQuery(
        std::uint64_t sequenceId,
        double simulationTimeSeconds,
        std::span<const OceanQueryPoint> points);
    const std::vector<Scene::GpuVisibilityReason>&
        GetGpuVisibilityReasons() const;
    const Scene::Camera*
        GetGpuVisibilityCamera() const;
    [[nodiscard]] const std::shared_ptr<const Scene::RenderViewFeedback>&
        GetGpuVisibilityFeedback() const noexcept;
    const std::shared_ptr<RHI::ITextureView>&
        GetFinalOutputSampledView() const;
    const std::shared_ptr<RHI::ITexture>&
        GetFinalOutputTexture() const;
    std::uint32_t GetFinalOutputWidth() const;
    std::uint32_t GetFinalOutputHeight() const;

private:
    bool m_frameDiagnosticsEnabled = false;
    std::vector<std::string> m_diagnosticRecordedPasses;
    static constexpr std::uint32_t ShadowMapResolution = SharedShadowMapResolution;
    static constexpr std::uint32_t ShadowCascadeCount = SharedShadowCascadeCount;
    static constexpr std::uint32_t GBufferCount = 4;
    static constexpr std::size_t ObjectConstantStride = 512;
    static constexpr std::size_t ShadowPassConstantStride = 256;

    using FrameConstants = SharedFrameConstants;
    using ObjectConstants = SharedObjectConstants;
    using MaterialConstants = SharedMaterialConstants;
    using PostProcessConstants = SharedPostProcessConstants;
    using ShadowPassConstants = SharedShadowPassConstants;

    enum class OpaqueGeometrySelection
    {
        All,
        ExcludeOcean,
        OceanOnly
    };

    struct SceneMaterialResources
    {
        std::shared_ptr<RHI::IBuffer> materialBuffer;
        std::vector<std::shared_ptr<RHI::IDescriptorSet>>
            descriptorSets;
        std::vector<std::shared_ptr<RHI::IDescriptorSet>>
            oceanDescriptorSets;
        std::vector<std::shared_ptr<RHI::IDescriptorSet>>
            shadowDescriptorSets;
    };

    struct SceneObjectResources
    {
        std::shared_ptr<SceneMaterialResources>
            materialResources;
    };

    struct SceneObjectBinding
    {
        const Asset::Mesh* mesh = nullptr;
        std::uint64_t materialIdentity = 0;
        std::uint64_t materialOverrideSignature = 0;
    };

    struct QueueTimingObservation
    {
        QueueSchedulingGraphProfile profile;
        bool nativeMultiQueue = false;
        bool valid = false;
    };

    struct InstanceBatch
    {
        std::size_t representativeObjectIndex = 0;
        std::uint32_t firstInstance = 0;
        std::uint32_t instanceCount = 0;
    };

    void ObserveCompletedQueueTimings();
    [[nodiscard]] Asset::ShaderManager&
        GetShaderManager();
    [[nodiscard]] PipelineCache& GetPipelineCache();
    void CreateDescriptorResources(
        RHI::IGraphicsDevice& device,
        RHI::ShaderBinaryFormat shaderFormat);
    void EnsureSceneResources(
        RHI::IGraphicsDevice& device,
        const Scene::RenderSceneView& scene);
    void CreateSceneResources(
        RHI::IGraphicsDevice& device,
        const Scene::RenderSceneView& scene);
    [[nodiscard]] std::shared_ptr<
        SceneMaterialResources>
        CreateMaterialResources(
            RHI::IGraphicsDevice& device,
            const Scene::RenderSceneView& scene,
            std::size_t objectIndex);
    void CreateShadowResources(
        RHI::IGraphicsDevice& device);
    void CreateSizeDependentResources(
        RHI::IRenderBackend& backend);
    void CreatePassDescriptorSets(
        RHI::IGraphicsDevice& device);
    void UpdatePostProcessInputBindings();
    void UpdateFeatureTextureBindings();
    void ReleaseSizeDependentResources();
    void CreatePipeline(
        RHI::IRenderBackend& backend);
    void UpdateConstants(
        RHI::IFrameContext& frame,
        const Scene::RenderSceneView& scene,
        double simulationTimeSeconds);
    void UpdateHistoryValidity(
        const Scene::Camera& camera);
    void BuildInstanceBatches(
        const Scene::RenderSceneView& scene);
    [[nodiscard]] std::vector<IndexedGeometryDraw>
        BuildOpaqueGeometryDraws(
            const Scene::RenderSceneView& scene,
            const RHI::IGraphicsPipeline&
                doubleSidedPipeline,
            const RHI::IGraphicsPipeline* oceanPipeline = nullptr,
            const RHI::IGraphicsPipeline* oceanDoubleSidedPipeline = nullptr,
            const RHI::IGraphicsPipeline* oceanWireframePipeline = nullptr,
            const RHI::IGraphicsPipeline* oceanWireframeDoubleSidedPipeline = nullptr,
            const RHI::IGraphicsPipeline* oceanTessellationPipeline = nullptr,
            const RHI::IGraphicsPipeline* oceanTessellationDoubleSidedPipeline = nullptr,
            const RHI::IGraphicsPipeline* oceanTessellationWireframePipeline = nullptr,
            OpaqueGeometrySelection selection =
                OpaqueGeometrySelection::All) const;
    void RenderWaterDepthCopyPass(
        RHI::ICommandContext& commandContext,
        std::uint32_t width,
        std::uint32_t height);
    void RenderWaterVisibilityPass(
        RHI::ICommandContext& commandContext,
        std::uint32_t width,
        std::uint32_t height,
        const Scene::RenderSceneView& scene);
    void RenderGBufferPass(
        RHI::ICommandContext& commandContext,
        const RHI::ITextureView& depthView,
        std::uint32_t width,
        std::uint32_t height,
        const Scene::RenderSceneView& scene);
    void RenderForwardPass(
        RHI::ICommandContext& commandContext,
        const RHI::ITextureView& depthView,
        std::uint32_t width,
        std::uint32_t height,
        const Scene::RenderSceneView& scene);
    void RenderTransparentPass(
        RHI::ICommandContext& commandContext,
        const RHI::ITextureView& depthView,
        std::uint32_t width,
        std::uint32_t height,
        const Scene::RenderSceneView& scene);
    void RenderShadowPass(RHI::ICommandContext& commandContext, const Scene::RenderSceneView& scene);
    void RenderSceneView(
        RHI::IRenderBackend& backend,
        std::shared_ptr<const Scene::RenderSceneView> scene,
        double simulationTimeSeconds,
        LogicalFrameId logicalFrameId);
    void RenderDeferredPass(RHI::ICommandContext& commandContext, std::uint32_t width, std::uint32_t height);
    void RenderBloomExtractPass(
        RHI::ICommandContext& commandContext);
    void RenderBloomHorizontalPass(
        RHI::ICommandContext& commandContext);
    void RenderBloomVerticalPass(
        RHI::ICommandContext& commandContext);
    void RenderHiZMip(
        RHI::ICommandContext& commandContext,
        std::uint32_t mipIndex,
        std::uint32_t width,
        std::uint32_t height);
    void RenderTonemapPass(
        RHI::ICommandContext& commandContext,
        const RHI::ITextureView& backBufferView,
        std::uint32_t width,
        std::uint32_t height);
    RHI::RenderingInfo CreateColorRenderingInfo(
        const RHI::ITextureView& target,
        std::uint32_t width,
        std::uint32_t height,
        RHI::ResourceState stateBefore,
        RHI::ResourceState stateAfter) const;

    std::filesystem::path m_shaderPath;
    RHI::ShaderBinaryFormat m_shaderFormat =
        RHI::ShaderBinaryFormat::Dxil;
    std::shared_ptr<RHI::IGraphicsPipeline> m_gbufferPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_gbufferDoubleSidedPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline> m_oceanGBufferPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_oceanGBufferDoubleSidedPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline> m_oceanGBufferWireframePipeline;
    std::shared_ptr<RHI::IGraphicsPipeline> m_oceanGBufferTessellationPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_oceanGBufferTessellationDoubleSidedPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_oceanGBufferTessellationWireframePipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_gbufferWireframePipeline;
    std::shared_ptr<RHI::IGraphicsPipeline> m_forwardPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_forwardDoubleSidedPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline> m_oceanForwardPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_oceanForwardDoubleSidedPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline> m_oceanForwardWireframePipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_oceanForwardTessellationPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_oceanForwardTessellationDoubleSidedPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_oceanForwardTessellationWireframePipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_forwardWireframePipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_transparentPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_transparentDoubleSidedPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_transparentWireframePipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_gbufferDebugLinePipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_forwardDebugLinePipeline;
    std::shared_ptr<RHI::IGraphicsPipeline> m_skyPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline> m_shadowPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline>
        m_shadowDoubleSidedPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline> m_deferredPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline> m_tonemapPipeline;
    std::shared_ptr<RHI::IGraphicsPipeline> m_shadowDebugPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_brightExtractComputePipeline;
    std::shared_ptr<RHI::IComputePipeline> m_blurHorizontalComputePipeline;
    std::shared_ptr<RHI::IComputePipeline> m_blurVerticalComputePipeline;
    std::shared_ptr<RHI::IComputePipeline> m_hiZCopyPipeline;
    std::shared_ptr<RHI::IComputePipeline> m_hiZDownsamplePipeline;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_descriptorSetLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_oceanDescriptorSetLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_shadowDescriptorSetLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_deferredDescriptorSetLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_postProcessDescriptorSetLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_bloomComputeDescriptorSetLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_hiZComputeDescriptorSetLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> m_shadowDebugDescriptorSetLayout;
    std::shared_ptr<RHI::ISampler> m_sampler;
    std::shared_ptr<RHI::ISampler> m_linearClampSampler;
    std::shared_ptr<RHI::ISampler> m_shadowSampler;
    std::shared_ptr<RHI::ITexture> m_shadowTexture;
    std::array<std::shared_ptr<RHI::ITextureView>, ShadowCascadeCount> m_shadowDepthViews;
    std::shared_ptr<RHI::ITextureView> m_shadowSampledView;
    std::shared_ptr<RHI::ITransientTexturePool>
        m_transientTexturePool;
    std::array<std::shared_ptr<RHI::ITexture>, GBufferCount> m_gbufferTextures;
    std::array<std::shared_ptr<RHI::ITextureView>, GBufferCount> m_gbufferRenderTargetViews;
    std::array<std::shared_ptr<RHI::ITextureView>, GBufferCount> m_gbufferSampledViews;
    std::shared_ptr<RHI::ITexture> m_hdrTexture;
    std::shared_ptr<RHI::ITextureView> m_hdrRenderTargetView;
    std::shared_ptr<RHI::ITextureView> m_hdrSampledView;
    std::shared_ptr<RHI::ITexture> m_bloomTextureA;
    std::shared_ptr<RHI::ITextureView> m_bloomRenderTargetViewA;
    std::shared_ptr<RHI::ITextureView> m_bloomSampledViewA;
    std::shared_ptr<RHI::ITextureView> m_bloomStorageViewA;
    std::shared_ptr<RHI::ITexture> m_bloomTextureB;
    std::shared_ptr<RHI::ITextureView> m_bloomRenderTargetViewB;
    std::shared_ptr<RHI::ITextureView> m_bloomSampledViewB;
    std::shared_ptr<RHI::ITextureView> m_bloomStorageViewB;
    std::shared_ptr<RHI::ITextureView> m_depthSampledView;
    std::shared_ptr<RHI::ITexture> m_depthTexture;
    std::shared_ptr<RHI::ITextureView> m_depthStencilView;
    std::shared_ptr<RHI::ITexture> m_finalOutputTexture;
    std::shared_ptr<RHI::ITextureView>
        m_finalOutputRenderTargetView;
    std::shared_ptr<RHI::ITextureView>
        m_finalOutputSampledView;
    std::shared_ptr<RHI::ITexture> m_hiZTexture;
    std::vector<std::shared_ptr<RHI::ITextureView>>
        m_hiZSampledViews;
    std::vector<std::shared_ptr<RHI::ITextureView>>
        m_hiZStorageViews;
    std::vector<std::shared_ptr<RHI::IDescriptorSet>>
        m_hiZDescriptorSets;
    std::vector<std::shared_ptr<RHI::IBuffer>> m_frameBuffers;
    std::vector<std::shared_ptr<RHI::IBuffer>> m_shadowPassBuffers;
    std::vector<std::shared_ptr<RHI::IBuffer>> m_objectBuffers;
    // Structured object data stays tightly packed. The dynamic object CBV
    // above uses a larger backend-aligned stride and cannot be indexed with
    // the shader's natural ObjectData stride on every graphics API.
    std::vector<std::shared_ptr<RHI::IBuffer>>
        m_indexedObjectBuffers;
    std::vector<std::shared_ptr<RHI::IBuffer>>
        m_instanceObjectIndexBuffers;
    std::vector<std::shared_ptr<RHI::IBuffer>> m_postProcessBuffers;
    std::vector<std::shared_ptr<RHI::IDescriptorSet>> m_deferredDescriptorSets;
    std::vector<std::shared_ptr<RHI::IDescriptorSet>> m_brightExtractComputeDescriptorSets;
    std::vector<std::shared_ptr<RHI::IDescriptorSet>> m_blurHorizontalComputeDescriptorSets;
    std::vector<std::shared_ptr<RHI::IDescriptorSet>> m_blurVerticalComputeDescriptorSets;
    std::vector<std::shared_ptr<RHI::IDescriptorSet>> m_tonemapDescriptorSets;
    std::vector<std::shared_ptr<RHI::IDescriptorSet>> m_skyDescriptorSets;
    std::vector<std::shared_ptr<RHI::IDescriptorSet>> m_shadowDebugDescriptorSets;
    std::shared_ptr<Asset::Texture> m_environmentCubemap;
    std::shared_ptr<Asset::Texture> m_irradianceCubemap;
    std::shared_ptr<Asset::Texture> m_prefilteredSpecularCubemapArray;
    std::shared_ptr<Asset::Texture> m_brdfLutTexture;
    std::shared_ptr<SceneRendererSharedResources>
        m_sharedResources;
    std::vector<SceneObjectResources> m_objectResources;
    std::vector<InstanceBatch> m_instanceBatches;
    std::vector<SceneObjectBinding>
        m_sceneObjectBindings;
    std::uint32_t m_activeFrame = 0;
    std::uint64_t m_oceanFrameSerial = 0u;
    std::uint32_t m_framesInFlight = 0;
    bool m_shadowInitialized = false;
    RHI::ResourceState m_depthState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState m_hiZState =
        RHI::ResourceState::Undefined;
    std::array<
        RHI::ResourceState,
        GBufferCount> m_gbufferStates{};
    RHI::ResourceState m_hdrState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState m_bloomAState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState m_bloomBState =
        RHI::ResourceState::Undefined;
    RHI::ResourceState m_finalOutputState =
        RHI::ResourceState::Undefined;
    bool m_hiZValid = false;
    Scene::Camera m_historyCamera{};
    bool m_historyCameraValid = false;
    RenderCaptureStage m_captureStage = RenderCaptureStage::Tonemap;
    RenderSettings m_settings{};
    bool m_editorGridAllowed = true;
    RendererStatistics m_statistics{};
    std::optional<ScenePipelinePlan> m_lastPipelinePlan;
    ScenePipelineFrameState m_lastPipelineFrameState{};
    RenderGraph m_renderGraph;
    Core::ITaskExecutor* m_taskExecutor = nullptr;
    RenderFeatureRegistry m_featureRegistry;
    RHI::GpuProfiler m_gpuProfiler;
    QueueSchedulingCostModel m_queueCostModel;
    ClusteredLighting m_clusteredLighting;
    LocalLightShadows m_localLightShadows;
    PlanarReflections m_planarReflections;
    VarianceShadowMaps m_varianceShadowMaps;
    GpuDrivenVisibility m_gpuDrivenVisibility;
    FftOcean m_fftOcean;
    // The expensive, view-independent ocean resources live in the shared
    // composition root. Only one renderer advances them; secondary views
    // sample the already-published maps.
    SpectralOceanSimulation* m_spectralOcean = nullptr;
    LocalWaveGpuResources* m_localWaveGpuResources = nullptr;
    bool m_viewIndependentSimulationProducer = true;
    bool m_sharedSimulationProducerThisFrame = true;
    LogicalFrameId m_scheduledLogicalFrameId = 0;
    LogicalFrameId m_lastRenderedLogicalFrameId = 0;
    LogicalFrameId m_logicalFrameId = 0;
    RenderViewId m_viewId = 1;
    LocalWaveSimulation m_localWaveSimulation;
    OceanQueryService m_oceanQueryService;
    OceanGpuQuery m_oceanGpuQuery;
    OceanSurfaceRenderer m_oceanSurfaceRenderer;
    WaterOpticsFeature m_waterOpticsFeature;
    struct PendingOceanQuery
    {
        std::uint64_t sequenceId = 0u;
        double simulationTimeSeconds = 0.0;
        std::vector<OceanQueryPoint> points;
        bool cameraMediumQuery = false;
    };
    std::optional<PendingOceanQuery> m_pendingOceanQuery;
    std::shared_ptr<RHI::IBuffer> m_oceanQueryConstants;
    std::shared_ptr<RHI::IBuffer> m_oceanQueryPoints;
    std::shared_ptr<RHI::IBuffer> m_oceanQueryResults;
    std::shared_ptr<RHI::IBuffer> m_oceanQueryReadback;
    std::shared_ptr<RHI::IBuffer> m_oceanGeometryInstanceBuffer;
    // A single zeroed record keeps the reflected adaptive-ocean binding valid
    // for legacy/clipmap draws and for frames with no selected quadtree leaf.
    std::shared_ptr<RHI::IBuffer> m_oceanGeometryInstanceFallbackBuffer;
    std::uint32_t m_oceanGeometryInstanceCount = 0u;
    std::uint32_t m_oceanGeometryNodeCount = 0u;
    std::uint32_t m_oceanVisibleGeometryNodeCount = 0u;
    std::uint32_t m_oceanGeometryRefinementIterations = 0u;
    std::uint32_t m_oceanGeometryMaxLod = 0u;
    float m_oceanGeometryMaxTessellationFactor = 1.0f;
    bool m_oceanTessellationActive = false;
    std::shared_ptr<RHI::ITexture> m_oceanQueryDummyLocalTexture;
    std::shared_ptr<RHI::ITextureView> m_oceanQueryDummyLocalView;
    std::shared_ptr<RHI::IDescriptorSet> m_oceanQueryDescriptorSet;
    std::uint32_t m_oceanQueryCount = 0u;
    std::uint64_t m_oceanQuerySubmittedFrame = 0u;
    std::uint64_t m_oceanQuerySequenceId = 0u;
    bool m_oceanQueryIsCameraMedium = false;
    DirectX::XMFLOAT2 m_cameraQuerySubmittedPosition{};
    std::uint64_t m_cameraQuerySubmittedVersion = 0u;
    std::uint64_t m_waterCameraCutVersion = 0u;
    std::uint64_t m_waterSurfaceHistoryVersion = 0u;
    std::uint64_t m_sceneRevision = 0u;
    struct CameraWaterHeight
    {
        DirectX::XMFLOAT2 position{};
        float displacement = 0.0f;
        std::uint64_t frame = 0u;
        std::uint64_t version = 0u;
        bool valid = false;
    };
    CameraWaterHeight m_cameraWaterHeight{};
    float m_oceanCpuMillisecondsFiltered = 0.0f;
    OceanSettings m_previousOceanSettings{};
    bool m_previousOceanSettingsValid = false;
    OceanDirtyScope m_pendingOceanDirtyScopes = OceanDirtyScope::None;
    std::uint64_t m_oceanRequestedSettingsVersion = 0u;
    std::uint64_t m_oceanActiveSettingsVersion = 0u;
    std::uint32_t m_previousOceanFullResetSerial = 0u;
    std::uint32_t m_previousOceanHistoryResetSerial = 0u;
    std::uint32_t m_previousOceanLocalResetSerial = 0u;
    std::uint32_t m_previousOceanManualDisturbanceSerial = 0u;
    bool m_oceanResetSerialsValid = false;
    double m_lastOceanSimulationTime = 0.0;
    bool m_oceanSimulationTimeValid = false;
    bool m_localWaveDemoActive = false;
    FluidFeature m_fluidFeature;
    VirtualTextureCache m_virtualTextureCache;
    ScreenSpaceEffects m_screenSpaceEffects;
    SkyAtmosphere m_skyAtmosphere;
    TemporalAntiAliasing m_temporalAntiAliasing;
    std::vector<DirectX::XMFLOAT4X4>
        m_previousWorldViewProjections;
    std::vector<DirectX::XMFLOAT4X4>
        m_currentWorldViewProjections;
    std::uint64_t m_temporalSampleIndex = 0;
    std::uint32_t m_finalOutputWidth = 0;
    std::uint32_t m_finalOutputHeight = 0;
    bool m_renderToSwapChain = true;
    bool m_gpuDrivenSupported = false;
    bool m_indexedObjectDrawingSupported = false;
    bool m_gpuDrivenActiveThisFrame = false;
    std::vector<QueueTimingObservation>
        m_queueTimingObservations;
    std::uint64_t
        m_observedTimingGeneration = 0;
    RhiGeometryRenderingPass m_geometryRenderingPass;
    RhiFullscreenRenderingPass m_fullscreenRenderingPass;
    RhiShadowPass m_shadowPass;
};
} // namespace Prism::Renderer
