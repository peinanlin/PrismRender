#include "Renderer/SceneRenderer.h"

#include "Asset/Material.h"
#include "Renderer/MaterialParameterResolver.h"
#include "Renderer/Pipeline/ScenePipelinePlan.h"
#include "Asset/Mesh.h"
#include "Asset/ShaderManager.h"
#include "Asset/Texture.h"
#include "Core/Assert.h"
#include "Core/CpuTrace.h"
#include "Core/DiagnosticLog.h"
#include "Core/Math/Double3.h"
#include "Renderer/DeferredTransientLayout.h"
#include "Renderer/Features/InteractiveTerrain.h"
#include "Renderer/Features/Ocean/OceanQuadtree.h"
#include "Renderer/SceneRendererSharedResources.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ICommandContext.h"
#include "RHI/ShaderLayoutBuilder.h"
#include "RHI/IFrameContext.h"
#include "RHI/IRenderBackend.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderSceneView.h"
#include "Renderer/ShadowCascades.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <chrono>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Prism::Renderer
{
using namespace DirectX;

namespace
{
struct RenderExtent
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
}

SceneRenderer::~SceneRenderer()
{
    try
    {
        m_featureRegistry.ShutdownLifecycleFeatures();
    }
    catch (...)
    {
        // Destruction must continue so the concrete Feature owners and GPU
        // resources can still be released.
    }
    ReleaseSwapChainResources();
}

void SceneRenderer::SetTaskExecutor(
    Core::ITaskExecutor& taskExecutor) noexcept
{
    m_taskExecutor = &taskExecutor;
}

Asset::ShaderManager& SceneRenderer::GetShaderManager()
{
    Core::Check(
        m_sharedResources != nullptr,
        "SceneRenderer shared resources are unavailable.");
    return m_sharedResources->GetShaderManager();
}

PipelineCache& SceneRenderer::GetPipelineCache()
{
    Core::Check(
        m_sharedResources != nullptr,
        "SceneRenderer shared resources are unavailable.");
    return m_sharedResources->GetPipelineCache();
}

void SceneRenderer::Initialize(RHI::IRenderBackend& backend,
    const std::filesystem::path& shaderPath,
    const Scene::RenderScene& scene,
    std::shared_ptr<Asset::Texture> environmentCubemap,
    const bool renderToSwapChain,
    std::shared_ptr<SceneRendererSharedResources> sharedResources,
    const bool viewIndependentSimulationProducer,
    const RenderViewId viewId)
{
    RHI::IGraphicsDevice& device = backend.GetGraphicsDevice();
    m_viewIndependentSimulationProducer = viewIndependentSimulationProducer;
    m_sharedSimulationProducerThisFrame =
        viewIndependentSimulationProducer;
    m_viewId = viewId;
    const RHI::IFrameContext& frame = backend.GetFrameContext();
    m_framesInFlight = frame.GetFramesInFlight();
    Core::Check(m_framesInFlight > 0,
        "SceneRenderer requires at least one frame in flight.");
    m_frameBuffers.resize(m_framesInFlight);
    m_shadowPassBuffers.resize(m_framesInFlight);
    m_objectBuffers.resize(m_framesInFlight);
    m_indexedObjectBuffers.resize(m_framesInFlight);
    m_instanceObjectIndexBuffers.resize(m_framesInFlight);
    m_postProcessBuffers.resize(m_framesInFlight);
    m_deferredDescriptorSets.resize(m_framesInFlight);
    m_brightExtractComputeDescriptorSets.resize(m_framesInFlight);
    m_blurHorizontalComputeDescriptorSets.resize(m_framesInFlight);
    m_blurVerticalComputeDescriptorSets.resize(m_framesInFlight);
    m_tonemapDescriptorSets.resize(m_framesInFlight);
    m_skyDescriptorSets.resize(m_framesInFlight);
    m_shadowDebugDescriptorSets.resize(m_framesInFlight);
    m_queueTimingObservations.resize(m_framesInFlight);
    m_shaderFormat = device.GetPreferredShaderBinaryFormat();
    // Shader Model 6.0 does not expose StartInstanceLocation to the vertex
    // shader. Until the D3D12 indirect command signature carries an explicit
    // object index, keep that backend on the dynamic-object-CBV path.
    m_indexedObjectDrawingSupported =
        frame.GetGraphicsApi() == RHI::GraphicsApi::Vulkan;
    m_gpuDrivenSupported =
        device.GetCapabilities().features.drawIndirectCount &&
        m_indexedObjectDrawingSupported;
    m_shaderPath = shaderPath;
    m_renderToSwapChain = renderToSwapChain;
    m_captureStage = ReadRenderCaptureStage();
    if (IsDeterministicRenderCaptureEnabled())
    {
        m_settings.gpuInstancingEnabled = false;
        m_settings.frustumCullingEnabled = false;
        m_settings.gpuDrivenEnabled = false;
    }
    if (ReadRenderEnvironmentVariable("PRISM_RENDER_GPU_DRIVEN_OVERRIDE") ==
        "1")
    {
        m_settings.gpuDrivenEnabled = true;
        m_settings.frustumCullingEnabled = true;
        m_settings.gpuInstancingEnabled = false;
    }
    if (sharedResources == nullptr)
    {
        sharedResources = std::make_shared<SceneRendererSharedResources>();
    }
    sharedResources->Initialize(device, std::move(environmentCubemap));
    Core::Check(sharedResources->IsInitialized(),
        "SceneRenderer shared resources are not initialized.");
    m_sharedResources = std::move(sharedResources);
    m_sharedResources->ConfigureInteractiveTerrain(device,
        shaderPath.parent_path() / "InteractiveTerrain.hlsl",
        m_shaderFormat,
        m_framesInFlight);
    m_sharedResources->InitializeOceanSimulation(
        device, shaderPath.parent_path(), m_shaderFormat, m_framesInFlight);
    m_spectralOcean = &m_sharedResources->GetSpectralOcean();
    m_localWaveGpuResources = &m_sharedResources->GetLocalWaveGpuResources();
    if (m_sharedSimulationProducerThisFrame)
    {
        const bool nativeAsyncOcean =
            m_settings.ocean.asyncComputeEnabled &&
            device.GetCapabilities().features.computeQueue;
        m_spectralOcean->SetAsyncComputeEnabled(nativeAsyncOcean);
        m_localWaveGpuResources->SetAsyncComputeEnabled(nativeAsyncOcean);
    }
    m_environmentCubemap = m_sharedResources->GetEnvironmentCubemap();
    m_irradianceCubemap = m_sharedResources->GetIrradianceCubemap();
    m_prefilteredSpecularCubemapArray =
        m_sharedResources->GetPrefilteredSpecularCubemapArray();
    m_brdfLutTexture = m_sharedResources->GetBrdfLutTexture();
    CreateDescriptorResources(device, m_shaderFormat);
    m_waterOpticsFeature.Initialize(device,
        GetShaderManager(),
        GetPipelineCache(),
        shaderPath.parent_path(),
        m_shaderFormat,
        m_framesInFlight,
        m_oceanDescriptorSetLayout,
        m_indexedObjectDrawingSupported);
    CreateShadowResources(device);
    m_varianceShadowMaps.Initialize(device,
        GetShaderManager(),
        GetPipelineCache(),
        shaderPath.parent_path() / "VarianceShadow.hlsl",
        m_shaderFormat,
        m_framesInFlight,
        m_shadowTexture);
    m_clusteredLighting.Initialize(device,
        GetShaderManager(),
        GetPipelineCache(),
        shaderPath.parent_path() / "ClusteredLighting.hlsl",
        m_shaderFormat,
        m_framesInFlight);
    m_temporalAntiAliasing.Initialize(device,
        GetShaderManager(),
        GetPipelineCache(),
        shaderPath.parent_path() / "TemporalAA.hlsl",
        m_shaderFormat,
        m_framesInFlight);
    m_screenSpaceEffects.Initialize(device,
        GetShaderManager(),
        GetPipelineCache(),
        shaderPath.parent_path() / "ScreenSpaceEffects.hlsl",
        m_shaderFormat,
        m_framesInFlight);
    m_skyAtmosphere.Initialize(device,
        GetShaderManager(),
        GetPipelineCache(),
        shaderPath.parent_path() / "SkyAtmosphere.hlsl",
        m_shaderFormat,
        m_framesInFlight);
    m_virtualTextureCache.Initialize(device, m_framesInFlight);
    m_fftOcean.Initialize(device,
        GetShaderManager(),
        GetPipelineCache(),
        shaderPath.parent_path() / "FftOcean.hlsl",
        m_shaderFormat,
        m_framesInFlight);
    m_oceanGpuQuery.InitializeGpu(device,
        GetShaderManager(),
        GetPipelineCache(),
        shaderPath.parent_path() / "Ocean",
        m_shaderFormat);
    // Queue this immutable fallback before BeginFrame flushes initial uploads.
    // A first query can run mid-frame with local waves disabled; creating its
    // texture there would leave the upload pending while the query samples it.
    {
        RHI::TextureDescription dummy{};
        dummy.format = RHI::Format::Rgba8Unorm;
        dummy.usage = RHI::TextureUsage::ShaderResource;
        constexpr std::array<std::uint8_t, 4> zero{};
        const RHI::TextureInitialData data{zero.data(), sizeof(zero), sizeof(zero)};
        m_oceanQueryDummyLocalTexture = device.CreateTexture(dummy, &data);
        m_oceanQueryDummyLocalTexture->SetDebugName("OceanQuery.DisabledLocalDisplacement");
        RHI::TextureViewDescription view{};
        view.type = RHI::TextureViewType::Sampled;
        view.format = dummy.format;
        m_oceanQueryDummyLocalView =
            device.CreateTextureView(m_oceanQueryDummyLocalTexture, view);
    }
    m_oceanSurfaceRenderer.Initialize(device, m_framesInFlight);
    m_fluidFeature.Initialize(device,
        GetShaderManager(),
        GetPipelineCache(),
        shaderPath.parent_path() / "Fluid",
        m_shaderFormat,
        m_framesInFlight);
    m_localLightShadows.Initialize(device,
        GetShaderManager(),
        GetPipelineCache(),
        shaderPath.parent_path() / "LocalLightShadow.hlsl",
        m_shaderFormat,
        m_framesInFlight,
        static_cast<std::uint32_t>(
            std::max<std::size_t>(scene.GetRenderObjects().size(), 1u)));
    {
        Core::CpuTraceSpan span(
            "PlanarReflectionsInitialize", "initialization");
        m_planarReflections.Initialize(device,
            GetShaderManager(),
            GetPipelineCache(),
            shaderPath.parent_path() / "PlanarReflection.hlsl",
            m_shaderFormat,
            m_framesInFlight,
            128u);
    }
    {
        Core::CpuTraceSpan span(
            "SizeDependentResourcesCreate", "initialization");
        CreateSizeDependentResources(backend);
    }
    {
        Core::CpuTraceSpan span("SceneResourcesCreate", "initialization");
        const Scene::RenderSceneView initialScene(
            Scene::RenderSceneControlView,
            scene);
        CreateSceneResources(device, initialScene);
    }
    {
        Core::CpuTraceSpan span("PassDescriptorSetsCreate", "initialization");
        CreatePassDescriptorSets(device);
    }
    {
        Core::CpuTraceSpan span("ScenePipelineCreate", "initialization");
        CreatePipeline(backend);
    }
    {
        Core::CpuTraceSpan span("GpuDrivenInitialize", "initialization");
        m_gpuDrivenVisibility.Initialize(device,
            GetShaderManager(),
            GetPipelineCache(),
            shaderPath.parent_path() / "GpuCulling.hlsl",
            m_shaderFormat,
            m_framesInFlight,
            static_cast<std::uint32_t>(
                std::max<std::size_t>(scene.GetRenderObjects().size(), 1u)));
    }
    m_gpuDrivenVisibility.SetOcclusionTexture(m_hiZTexture);
    BuiltInRenderFeatures::RegisterGpuDrivenVisibility(
        m_featureRegistry,
        m_gpuDrivenVisibility,
        [this](const RenderFeatureFrameContext& context)
        {
            if (!m_gpuDrivenActiveThisFrame)
            {
                return;
            }
            m_gpuDrivenVisibility.Update(
                context.scene,
                context.scene.GetCamera(),
                context.frameIndex,
                m_settings.frustumCullingEnabled,
                m_settings.occlusionCullingEnabled,
                m_hiZValid,
                m_settings.gpuVisibilityReadbackEnabled,
                context.width,
                context.height,
                m_oceanQueryService.Bounds().radius);
            const GpuVisibilityStatistics& gpuStatistics =
                m_gpuDrivenVisibility.GetStatistics();
            m_statistics.gpuDrivenCandidateObjects =
                m_gpuDrivenVisibility.GetObjectCount();
            m_statistics.gpuVisibilityStatisticsValid =
                gpuStatistics.valid
                && gpuStatistics.candidateObjects
                    == context.scene.GetRenderObjects().size();
            m_statistics.gpuVisibleObjects =
                gpuStatistics.visibleObjects;
            m_statistics.gpuLodRejectedObjects =
                gpuStatistics.lodRejectedObjects;
            m_statistics.gpuFrustumCulledObjects =
                gpuStatistics.frustumCulledObjects;
            m_statistics.gpuOcclusionCulledObjects =
                gpuStatistics.occlusionCulledObjects;
            m_statistics.gpuDisabledObjects =
                gpuStatistics.disabledObjects;
            if (m_statistics.gpuVisibilityStatisticsValid)
            {
                m_statistics.visibleObjects =
                    gpuStatistics.visibleObjects;
                m_statistics.renderedObjects =
                    gpuStatistics.visibleObjects;
                m_statistics.culledObjects =
                    gpuStatistics.candidateObjects
                    - gpuStatistics.visibleObjects;
            }
        },
        [this](const RenderFeatureResizeContext& context)
        {
            Core::Check(
                context.gpuIdle,
                "GpuDrivenVisibility resize requires the renderer GPU-idle safe point.");
            m_gpuDrivenVisibility.SetOcclusionTexture(m_hiZTexture);
        });
    BuiltInRenderFeatures::RegisterSkyAtmosphere(
        m_featureRegistry,
        m_skyAtmosphere,
        [this](const RenderFeatureFrameContext& context)
        {
            const Scene::DirectionalLight& light =
                context.scene.GetDirectionalLight();
            const XMVECTOR worldSunDirection = XMVector3Normalize(
                XMVectorSet(
                    -light.direction.x,
                    -light.direction.y,
                    -light.direction.z,
                    0.0f));
            XMFLOAT3 sunDirection{};
            XMStoreFloat3(&sunDirection, worldSunDirection);
            m_skyAtmosphere.Update(
                context.frameIndex,
                m_settings,
                context.scene.GetCamera().GetPosition(),
                sunDirection,
                light.intensity);
        });
    BuiltInRenderFeatures::RegisterWaterOptics(
        m_featureRegistry,
        m_waterOpticsFeature,
        [this](const RenderFeatureFrameContext& context)
        {
            const bool enabled =
                m_settings.ocean.opticsModel == OceanOpticsModel::HpWater
                && m_settings.ocean.implementation
                    == OceanImplementation::SpectralOcean
                && m_settings.fftOceanEnabled
                && m_settings.ocean.debug.renderWater;
            m_waterOpticsFeature.PrepareFrame(
                m_settings.ocean.optics,
                context.logicalFrameId,
                enabled);
        },
        [this](const RenderFeatureGraphContext& context)
        {
            WaterOpticsPassCallbacks callbacks{};
            const std::uint32_t width = context.width;
            const std::uint32_t height = context.height;
            const std::uint32_t frameIndex = context.frameIndex;
            const std::shared_ptr<const Scene::RenderSceneView> activeScene =
                context.sceneLifetime;
            Core::Check(activeScene != nullptr,
                "Water optics requires a retained scene view.");
            callbacks.depthCopy =
                [this, width, height](
                    RHI::ICommandContext& commandContext,
                    const RenderGraphPassResources&)
            {
                RenderWaterDepthCopyPass(commandContext, width, height);
            };
            callbacks.visibility =
                [this, width, height, activeScene](
                    RHI::ICommandContext& commandContext,
                    const RenderGraphPassResources&)
            {
                RenderWaterVisibilityPass(
                    commandContext, width, height, *activeScene);
            };
            callbacks.refraction =
                [this, width, height, frameIndex](
                    RHI::ICommandContext& commandContext,
                    const RenderGraphPassResources&)
            {
                m_waterOpticsFeature.ExecuteRefraction(
                    commandContext, width, height, frameIndex);
            };
            callbacks.caustics =
                [this, frameIndex](
                    RHI::ICommandContext& commandContext,
                    const RenderGraphPassResources&)
            {
                m_waterOpticsFeature.ExecuteCaustics(
                    commandContext, frameIndex);
            };
            callbacks.volumetricAccumulate =
                [this, frameIndex](
                    RHI::ICommandContext& commandContext,
                    const RenderGraphPassResources&)
            {
                m_waterOpticsFeature.ExecuteVolumetricAccumulate(
                    commandContext, frameIndex);
            };
            callbacks.volumetricTemporal =
                [this, frameIndex](
                    RHI::ICommandContext& commandContext,
                    const RenderGraphPassResources&)
            {
                m_waterOpticsFeature.ExecuteVolumetricTemporal(
                    commandContext, frameIndex);
            };
            callbacks.volumetricReconstruct =
                [this, frameIndex](
                    RHI::ICommandContext& commandContext,
                    const RenderGraphPassResources&)
            {
                m_waterOpticsFeature.ExecuteVolumetricReconstruct(
                    commandContext, frameIndex);
            };
            callbacks.composite =
                [this, width, height, frameIndex](
                    RHI::ICommandContext& commandContext,
                    const RenderGraphPassResources&)
            {
                m_waterOpticsFeature.ExecuteComposite(
                    commandContext, width, height, frameIndex);
            };
            callbacks.publish =
                [this, width, height](
                    RHI::ICommandContext& commandContext,
                    const RenderGraphPassResources&)
            {
                m_waterOpticsFeature.ExecutePublish(
                    commandContext, width, height);
            };
            return callbacks;
        });
    BuiltInRenderFeatures::RegisterFluid(
        m_featureRegistry,
        m_fluidFeature);
    BuiltInRenderFeatures::RegisterClusteredLighting(
        m_featureRegistry,
        m_clusteredLighting);
    BuiltInRenderFeatures::RegisterLocalLightShadows(
        m_featureRegistry,
        m_localLightShadows);
    BuiltInRenderFeatures::RegisterVarianceShadowMaps(
        m_featureRegistry,
        m_varianceShadowMaps);
    BuiltInRenderFeatures::RegisterPlanarReflections(
        m_featureRegistry,
        m_planarReflections);
    BuiltInRenderFeatures::RegisterScreenSpaceEffects(
        m_featureRegistry,
        m_screenSpaceEffects,
        [this](const RenderFeatureResizeContext& context)
        {
            Core::Check(
                context.gpuIdle,
                "ScreenSpaceEffects resize requires the renderer GPU-idle safe point.");
            m_screenSpaceEffects.Resize(
                context.width,
                context.height,
                m_gbufferTextures[0],
                m_gbufferTextures[1],
                m_hdrTexture,
                m_hiZTexture,
                m_planarReflections.GetColorTextureShared());
        });
    BuiltInRenderFeatures::RegisterTemporalAntiAliasing(
        m_featureRegistry,
        m_temporalAntiAliasing,
        [this](const RenderFeatureResizeContext& context)
        {
            Core::Check(
                context.gpuIdle,
                "TemporalAntiAliasing resize requires the renderer GPU-idle safe point.");
            m_temporalAntiAliasing.Resize(
                context.width,
                context.height,
                m_screenSpaceEffects.GetCompositeTexture());
        });
    BuiltInRenderFeatures::RegisterFftOcean(
        m_featureRegistry,
        m_fftOcean,
        [this]()
        {
            return m_settings.ocean.implementation
                == OceanImplementation::LegacyFft;
        });
    BuiltInRenderFeatures::RegisterSpectralOcean(
        m_featureRegistry,
        *m_spectralOcean,
        [this]()
        {
            return m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean;
        });
    BuiltInRenderFeatures::RegisterLocalWave(
        m_featureRegistry,
        *m_localWaveGpuResources,
        [this]()
        {
            return m_settings.ocean.implementation
                    == OceanImplementation::SpectralOcean
                && m_settings.ocean.local.enabled;
        });
    BuiltInRenderFeatures::RegisterInteractiveTerrain(
        m_featureRegistry,
        m_sharedResources->GetInteractiveTerrain(),
        [this](const RenderFeatureFrameContext&)
        {
            m_sharedResources->GetInteractiveTerrain().UpdateCommand(
                {m_settings.terrainBrushCommandRevision,
                 m_settings.terrainBrushUv,
                 m_settings.terrainBrushRadius /
                     std::max(m_settings.terrainWorldSize, 1.0f),
                 m_settings.terrainBrushDelta,
                 m_settings.terrainBrushActive,
                 m_settings.terrainResetRequested,
                 m_settings.terrainFullUpdateRequested,
                 m_settings.terrainErosionEnabled});
        },
        [this](const RenderFeatureGraphContext& context)
        {
            return m_settings.interactiveTerrainEnabled
                && context.scene.GetFeatureUsage().hasTerrainSurface
                && m_sharedResources
                    ->IsInteractiveTerrainInitialized();
        });
    BuiltInRenderFeatures::RegisterVirtualTextureCache(
        m_featureRegistry,
        m_virtualTextureCache,
        [this](const RenderFeatureFrameContext& context)
        {
            m_virtualTextureCache.Update(
                context.frameIndex,
                context.scene.GetCamera().GetPosition(),
                m_settings.terrainWorldSize,
                m_settings.virtualTerrainEnabled
                    && m_settings.terrainVirtualTextureEnabled
                    && m_settings.terrainMaterialColorsEnabled);
        });
    m_featureRegistry.InitializeLifecycleFeatures(
        RenderFeatureInitializationContext{&device});
    {
        Core::CpuTraceSpan span("GpuProfilerInitialize", "initialization");
        m_gpuProfiler.Initialize(backend);
    }
    const std::string costModelPath =
        ReadRenderEnvironmentVariable("PRISM_RENDER_QUEUE_COST_MODEL_PATH");
    {
        Core::CpuTraceSpan span("QueueCostModelConfigure", "initialization");
        m_queueCostModel.Configure(
            {std::string(RHI::ToString(frame.GetGraphicsApi())),
             backend.GetAdapterName(), frame.GetFrameWidth(),
             frame.GetFrameHeight()},
            costModelPath.empty()
                ? std::filesystem::path(PRISM_RENDER_PROJECT_DIR) /
                      "automation/cache/rdg-queue-cost-model.json"
                : std::filesystem::path(costModelPath));
    }
}

void SceneRenderer::ReleaseSwapChainResources()
{
    m_blurVerticalComputePipeline.reset();
    m_blurHorizontalComputePipeline.reset();
    m_brightExtractComputePipeline.reset();
    m_shadowDebugPipeline.reset();
    m_tonemapPipeline.reset();
    m_deferredPipeline.reset();
    m_skyPipeline.reset();
    m_shadowPipeline.reset();
    m_shadowDoubleSidedPipeline.reset();
    m_forwardPipeline.reset();
    m_forwardDoubleSidedPipeline.reset();
    m_oceanForwardPipeline.reset();
    m_oceanForwardDoubleSidedPipeline.reset();
    m_oceanForwardWireframePipeline.reset();
    m_oceanForwardTessellationPipeline.reset();
    m_oceanForwardTessellationDoubleSidedPipeline.reset();
    m_oceanForwardTessellationWireframePipeline.reset();
    m_forwardWireframePipeline.reset();
    m_transparentPipeline.reset();
    m_transparentDoubleSidedPipeline.reset();
    m_transparentWireframePipeline.reset();
    m_forwardDebugLinePipeline.reset();
    m_gbufferDebugLinePipeline.reset();
    m_gbufferPipeline.reset();
    m_gbufferDoubleSidedPipeline.reset();
    m_oceanGBufferPipeline.reset();
    m_oceanGBufferDoubleSidedPipeline.reset();
    m_oceanGBufferWireframePipeline.reset();
    m_oceanGBufferTessellationPipeline.reset();
    m_oceanGBufferTessellationDoubleSidedPipeline.reset();
    m_oceanGBufferTessellationWireframePipeline.reset();
    m_gbufferWireframePipeline.reset();
    m_oceanQueryDescriptorSet.reset();
    m_oceanQueryConstants.reset();
    m_oceanQueryPoints.reset();
    m_oceanQueryResults.reset();
    m_oceanQueryReadback.reset();
    // Ocean instance buffers are scene/device resources, not swapchain-sized
    // resources.  Keeping them alive across a resize is required because
    // RecreateSwapChainResources() deliberately does not rebuild scene
    // material descriptor sets.  Clearing the fallback here left binding 42
    // null on the first frame after a window resize.
    m_oceanGeometryNodeCount = 0u;
    m_oceanVisibleGeometryNodeCount = 0u;
    m_oceanGeometryRefinementIterations = 0u;
    m_oceanGeometryMaxLod = 0u;
    m_oceanGeometryMaxTessellationFactor = 1.0f;
    m_oceanTessellationActive = false;
    m_oceanQuerySubmittedFrame = 0u;
    m_oceanQuerySequenceId = 0u;
    ReleaseSizeDependentResources();
}

void SceneRenderer::RecreateSwapChainResources(
    RHI::IRenderBackend& backend,
    const bool resizeSharedFeatures)
{
    const RHI::IFrameContext& frame = backend.GetFrameContext();
    ReleaseSwapChainResources();
    m_depthState = RHI::ResourceState::Undefined;
    m_hiZState = RHI::ResourceState::Undefined;
    m_hiZValid = false;
    CreateSizeDependentResources(backend);
    // Features with external size-dependent inputs are rebuilt only after
    // the new GBuffer/HiZ/HDR resources exist. Lifecycle dependencies keep
    // PlanarReflections -> ScreenSpaceEffects -> TAA ordering deterministic.
    const RenderFeatureResizeContext resizeContext{
        m_lastRenderedLogicalFrameId,
        m_viewId,
        frame.GetFrameWidth(),
        frame.GetFrameHeight(),
        true};
    m_featureRegistry.ResizeLifecycleFeatures(
        resizeContext,
        RenderFeatureScope::ViewLocal);
    if (resizeSharedFeatures)
    {
        m_featureRegistry.ResizeLifecycleFeatures(
            resizeContext,
            RenderFeatureScope::DeviceShared);
    }
    CreatePassDescriptorSets(
        backend.GetGraphicsDevice());
    CreatePipeline(backend);
    m_queueCostModel.UpdateDimensions(
        frame.GetFrameWidth(),
        frame.GetFrameHeight());
}

void SceneRenderer::Render(
    RHI::IRenderBackend& backend,
    std::shared_ptr<const Scene::RenderFramePacket> packet,
    const Scene::RenderViewId viewId,
    std::shared_ptr<const Scene::RenderViewFeedback> visibilityFeedback)
{
    Core::Check(packet != nullptr, "SceneRenderer requires a frame packet.");
    const LogicalFrameId logicalFrameId =
        packet->GetLogicalFrameId().value;
    const double simulationTimeSeconds =
        packet->GetSimulationTimeSeconds();
    RenderSceneView(
        backend,
        std::make_shared<const Scene::RenderSceneView>(
            std::move(packet),
            viewId,
            viewId == Scene::GameRenderViewId
                ? (m_settings.gpuDrivenEnabled && m_gpuDrivenSupported
                       ? Scene::RenderTerrainSelectionPolicy::PreserveHierarchy
                       : Scene::RenderTerrainSelectionPolicy::LeafOnly)
                : Scene::RenderTerrainSelectionPolicy::RootOnly,
            std::move(visibilityFeedback),
            Scene::GameRenderViewId),
        simulationTimeSeconds,
        logicalFrameId);
}

void SceneRenderer::RenderSceneView(
    RHI::IRenderBackend& backend,
    std::shared_ptr<const Scene::RenderSceneView> sceneInput,
    const double simulationTimeSeconds,
    LogicalFrameId logicalFrameId)
{
    Core::Check(sceneInput != nullptr,
        "SceneRenderer requires a retained scene view.");
    const Scene::RenderSceneView& scene = *sceneInput;
    const auto rendererCpuStart = std::chrono::steady_clock::now();
    Core::CpuTraceSpan rendererSpan("SceneRenderer", "renderer");
    m_diagnosticRecordedPasses.clear();
    RHI::IFrameContext& frame =
        backend.GetFrameContext();
    RHI::IGraphicsDevice& device =
        backend.GetGraphicsDevice();
    const auto oceanCpuStart = std::chrono::steady_clock::now();
    m_activeFrame = frame.GetCurrentFrameIndex();
    ++m_oceanFrameSerial;
    if (logicalFrameId == 0)
    {
        logicalFrameId = m_lastRenderedLogicalFrameId + 1u;
        m_sharedSimulationProducerThisFrame =
            m_viewIndependentSimulationProducer;
    }
    else
    {
        Core::Check(
            logicalFrameId == m_scheduledLogicalFrameId,
            "SceneRenderer requires a coordinator producer assignment for an explicit logical frame.");
    }
    Core::Check(
        logicalFrameId > m_lastRenderedLogicalFrameId,
        "SceneRenderer logical frame ids must increase for rendered frames.");
    m_logicalFrameId = logicalFrameId;
    m_lastRenderedLogicalFrameId = logicalFrameId;
    m_gpuDrivenActiveThisFrame =
        m_settings.gpuDrivenEnabled
        && m_gpuDrivenSupported
        && scene.GetFeatureUsage().hasGpuDrivenCandidate;
    if (m_oceanQueryDescriptorSet != nullptr
        && m_oceanQuerySubmittedFrame != 0u
        // The readback copy is recorded on the same frame as the query.  Do
        // not map it until every in-flight frame that could still reference
        // that resource has retired; this keeps the backend-neutral path
        // safe even when the native RHI does not expose a timeline fence.
        && m_oceanQuerySubmittedFrame + frame.GetFramesInFlight()
            <= m_oceanFrameSerial
        && m_oceanQueryReadback != nullptr)
    {
        std::vector<OceanDisplacementSample> samples(m_oceanQueryCount);
        std::vector<DirectX::XMFLOAT4> payload(m_oceanQueryCount);
        m_oceanQueryReadback->Read(payload.data(),
            payload.size() * sizeof(DirectX::XMFLOAT4));
        for (std::size_t index = 0u; index < payload.size(); ++index)
            samples[index].displacement = {payload[index].x,
                payload[index].y, payload[index].z};
        if (m_oceanQueryIsCameraMedium && !samples.empty())
        {
            m_cameraWaterHeight = {m_cameraQuerySubmittedPosition,
                samples[0].displacement.y, m_oceanFrameSerial,
                m_cameraQuerySubmittedVersion, true};
        }
        else
        {
            (void)m_oceanQueryService.CompleteGpuBatch(
                m_oceanQuerySequenceId, m_oceanFrameSerial, samples);
        }
        m_oceanQueryDescriptorSet.reset();
        m_oceanQuerySubmittedFrame = 0u;
    }
    Core::CpuTraceSpan sceneResourcesSpan(
        "EnsureSceneResources", "renderer");
    EnsureSceneResources(device, scene);
    sceneResourcesSpan.End();
    const Scene::Camera& visibilityCamera = scene.GetCamera();
    UpdateHistoryValidity(visibilityCamera);
    m_statistics = {};
    m_statistics.ocean.graphicsApi = frame.GetGraphicsApi();
    const bool adaptiveGeometryRequested =
        m_settings.ocean.implementation == OceanImplementation::SpectralOcean
        && m_settings.ocean.debug.renderWater
        && ShouldUseOceanTessellation(
            m_settings.ocean, device.GetCapabilities());
    const OceanDisplacementBounds geometryBounds =
        OceanQueryService::EstimateBounds(m_settings.ocean);
    const auto geometryStart = std::chrono::steady_clock::now();
    Core::CpuTraceSpan oceanGeometrySpan(
        "OceanAdaptiveGeometry", "renderer");
    m_oceanTessellationActive = adaptiveGeometryRequested
        && m_oceanSurfaceRenderer.UpdateAdaptiveGeometry(device,
            m_settings.ocean.geometry, visibilityCamera, m_activeFrame,
            frame.GetFrameHeight(), geometryBounds.radius);
    oceanGeometrySpan.End();
    m_oceanGeometryInstanceBuffer = m_oceanTessellationActive
        ? m_oceanSurfaceRenderer.GetInstanceBuffer(m_activeFrame)
        : nullptr;
    const OceanQuadtreeSelection& oceanSelection =
        m_oceanSurfaceRenderer.GetSelection();
    m_oceanGeometryInstanceCount = m_oceanTessellationActive
        ? static_cast<std::uint32_t>(oceanSelection.nodes.size()) : 0u;
    m_oceanGeometryNodeCount = m_oceanGeometryInstanceCount;
    m_oceanVisibleGeometryNodeCount = m_oceanGeometryInstanceCount;
    m_oceanGeometryRefinementIterations = m_oceanTessellationActive
        ? oceanSelection.refinementIterations : 0u;
    m_oceanGeometryMaxLod = 0u;
    if (m_oceanTessellationActive)
        for (const OceanQuadtreeNode& node : oceanSelection.nodes)
            m_oceanGeometryMaxLod = std::max(
                m_oceanGeometryMaxLod, node.lod);
    m_statistics.ocean.geometryMilliseconds = static_cast<float>(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - geometryStart).count());
    m_oceanGeometryMaxTessellationFactor = std::clamp(
        std::ceil(
            std::max(1.0f, m_settings.ocean.geometry.maximumEdgeLengthPixels) /
            4.0f),
        1.0f,
        64.0f);
    // The physical atlas and adaptive instance buffer can change during the
    // frame update. Rewrite only this frame's descriptors so in-flight views
    // keep their previous resource references.
    Core::CpuTraceSpan featureBindingsBeforeSimulationSpan(
        "FeatureBindingsBeforeSimulation", "renderer");
    UpdateFeatureTextureBindings();
    featureBindingsBeforeSimulationSpan.End();
    Core::CpuTraceSpan oceanSimulationCpuSpan(
        "OceanSimulationCpuUpdate", "renderer");
    if (m_sharedSimulationProducerThisFrame)
    {
        const bool firstOceanSettings = !m_previousOceanSettingsValid;
        m_pendingOceanDirtyScopes =
            m_previousOceanSettingsValid
                ? ClassifyOceanDirtyScopes(
                      m_previousOceanSettings, m_settings.ocean)
                : OceanDirtyScope::None;
        if (firstOceanSettings)
        {
            m_oceanRequestedSettingsVersion = 1u;
        }
        else if (m_pendingOceanDirtyScopes != OceanDirtyScope::None)
        {
            ++m_oceanRequestedSettingsVersion;
        }
        const OceanDebugSettings& oceanDebug = m_settings.ocean.debug;
        bool explicitFullReset = false;
        bool explicitHistoryReset = false;
        bool explicitLocalReset = false;
        bool explicitManualDisturbance = false;
        if (m_oceanResetSerialsValid)
        {
            explicitFullReset =
                oceanDebug.fullResetSerial != m_previousOceanFullResetSerial;
            explicitHistoryReset = oceanDebug.historyResetSerial !=
                                   m_previousOceanHistoryResetSerial;
            explicitLocalReset =
                oceanDebug.localResetSerial != m_previousOceanLocalResetSerial;
            explicitManualDisturbance =
                m_settings.ocean.local.manualDisturbanceSerial
                    != m_previousOceanManualDisturbanceSerial;
        }
        m_previousOceanFullResetSerial = oceanDebug.fullResetSerial;
        m_previousOceanHistoryResetSerial = oceanDebug.historyResetSerial;
        m_previousOceanLocalResetSerial = oceanDebug.localResetSerial;
        m_previousOceanManualDisturbanceSerial =
            m_settings.ocean.local.manualDisturbanceSerial;
        m_oceanResetSerialsValid = true;
        if (explicitFullReset || explicitHistoryReset)
        {
            ++m_waterSurfaceHistoryVersion;
            m_cameraWaterHeight.valid = false;
            m_pendingOceanDirtyScopes =
                m_pendingOceanDirtyScopes | OceanDirtyScope::HistoryReset;
        }
        if (explicitFullReset || explicitLocalReset)
        {
            m_pendingOceanDirtyScopes =
                m_pendingOceanDirtyScopes | OceanDirtyScope::LocalReset;
        }
        if ((explicitFullReset || explicitHistoryReset ||
                HasDirtyScope(m_pendingOceanDirtyScopes,
                    OceanDirtyScope::HistoryReset)) &&
            m_settings.ocean.implementation ==
                OceanImplementation::SpectralOcean)
        {
            m_spectralOcean->ResetFoamHistory();
        }
        if (explicitFullReset || explicitLocalReset ||
            HasDirtyScope(
                m_pendingOceanDirtyScopes, OceanDirtyScope::LocalReset))
        {
            m_localWaveSimulation.Reset();
            if (m_localWaveGpuResources->IsInitialized())
                frame.WaitForGpu();
            m_localWaveGpuResources->Reset();
        }
        if (explicitFullReset)
        {
            m_oceanQueryService.Reset();
            m_oceanSimulationTimeValid = false;
            m_localWaveDemoActive = false;
        }
        m_previousOceanSettings = m_settings.ocean;
        m_previousOceanSettingsValid = true;
        const float localWaveDeltaSeconds = static_cast<float>(
            std::clamp(m_oceanSimulationTimeValid
                           ? simulationTimeSeconds - m_lastOceanSimulationTime
                           : 1.0 / 60.0,
                0.0,
                0.25));
        if (m_settings.ocean.implementation ==
            OceanImplementation::SpectralOcean)
        {
            if (explicitManualDisturbance)
            {
                const LocalWaveDisturbance disturbance{
                    m_settings.ocean.local.manualPosition,
                    m_settings.ocean.local.manualRadiusMeters,
                    m_settings.ocean.local.manualStrength,
                    m_settings.ocean.local.manualVelocityDirection};
                m_localWaveSimulation.AddDisturbances(
                    std::span<const LocalWaveDisturbance>(&disturbance, 1u));
            }
            if (m_settings.ocean.local.enabled &&
                m_settings.ocean.debug.simulateWater)
            {
                const double delta =
                    m_oceanSimulationTimeValid
                        ? simulationTimeSeconds - m_lastOceanSimulationTime
                        : 1.0 / 60.0;
                if (delta < 0.0)
                {
                    m_localWaveSimulation.Reset();
                }
                m_localWaveSimulation.AdvanceEmitters(
                    static_cast<float>(std::clamp(delta, 0.0, 0.25)),
                    m_settings.ocean.local);
                m_localWaveDemoActive = true;
            }
            else
            {
                if (!m_settings.ocean.local.enabled && m_localWaveDemoActive)
                {
                    m_localWaveSimulation.Reset();
                }
                m_localWaveDemoActive = false;
            }
            m_lastOceanSimulationTime = simulationTimeSeconds;
            m_oceanSimulationTimeValid = true;
            m_oceanQueryService.Configure(m_settings.ocean);
            m_oceanQueryService.AdvanceFrame(m_oceanFrameSerial);
        }
        else
        {
            if (m_localWaveDemoActive)
            {
                m_localWaveSimulation.Reset();
            }
            m_localWaveDemoActive = false;
            m_oceanSimulationTimeValid = false;
            m_oceanQueryService.Reset();
        }
        if (m_settings.ocean.implementation ==
                OceanImplementation::SpectralOcean &&
            m_settings.ocean.local.enabled)
        {
            if (m_localWaveGpuResources->IsInitialized()
                && (m_localWaveGpuResources->GridSize() != m_settings.ocean.local.gridSize
                    || m_localWaveGpuResources->DomainSizeMeters() != m_settings.ocean.local.domainSizeMeters))
                frame.WaitForGpu();
            (void)m_localWaveGpuResources->Configure(
                device, m_settings.ocean.local);
            m_localWaveGpuResources->Update(m_activeFrame,
                m_settings.ocean.local.shareSpectralTime ? localWaveDeltaSeconds
                                                         : 1.0f / 60.0f,
                m_settings.ocean.local,
                !m_settings.ocean.debug.simulateWater ||
                    !m_settings.ocean.local.enabled,
                m_localWaveSimulation.LastFrameDisturbances());
        }
        else
        {
            if (m_localWaveGpuResources->IsInitialized())
                frame.WaitForGpu();
            m_localWaveGpuResources->Reset();
        }
    }
    oceanSimulationCpuSpan.End();
    m_statistics.gpuInstancingSupported = m_indexedObjectDrawingSupported;
    m_statistics.gpuDrivenSupported = m_gpuDrivenSupported;
    const auto& statisticsObjects = scene.GetRenderObjects();
    for (std::size_t objectIndex = 0;
         objectIndex < statisticsObjects.size(); ++objectIndex)
    {
        if (scene.IsObjectSelected(objectIndex)
            && statisticsObjects[objectIndex].visible)
        {
            ++m_statistics.visibleObjects;
        }
        else
        {
            ++m_statistics.culledObjects;
        }
    }
    m_statistics.renderedObjects = m_statistics.visibleObjects;
    {
        Core::CpuTraceSpan updateSpan("SceneConstantsUpdate", "renderer");
        UpdateConstants(frame, scene, simulationTimeSeconds);
        if (m_sharedSimulationProducerThisFrame)
        {
            // UpdateConstants commits one coherent constants/resource set for
            // this frame. Older frame descriptors retain their prior resource
            // generation until their fences retire.
            m_oceanActiveSettingsVersion =
                m_oceanRequestedSettingsVersion;
        }
    }
    Core::CpuTraceSpan gpuProfilerBeginSpan(
        "GpuProfilerBeginFrame", "renderer");
    m_gpuProfiler.BeginFrame(backend);
    gpuProfilerBeginSpan.End();
    ObserveCompletedQueueTimings();
    const RenderExtent renderExtent{
        frame.GetFrameWidth(), frame.GetFrameHeight()};
    RHI::ITexture* const outputTexture =
        m_renderToSwapChain ? frame.GetCurrentBackBufferTexture().get()
                            : m_finalOutputTexture.get();
    const RHI::ITextureView* const outputView =
        m_renderToSwapChain ? &frame.GetCurrentBackBufferView()
                            : m_finalOutputRenderTargetView.get();
    const std::shared_ptr<RHI::ITexture> depthTextureShared =
        m_renderToSwapChain ? frame.GetDepthStencilTexture()
                            : m_depthTexture;
    RHI::ITexture* const depthTexture = depthTextureShared.get();
    const RHI::ITextureView* const depthView =
        m_renderToSwapChain ? &frame.GetDepthStencilView()
                            : m_depthStencilView.get();
    Core::Check(outputTexture != nullptr && outputView != nullptr &&
                    depthTexture != nullptr && depthView != nullptr,
        "SceneRenderer output or depth target is unavailable.");
    const bool hpWaterVisibility =
        m_settings.ocean.opticsModel == OceanOpticsModel::HpWater
        && m_settings.ocean.implementation
            == OceanImplementation::SpectralOcean
        && m_settings.fftOceanEnabled
        && m_settings.ocean.debug.renderWater;
    if (m_settings.interactiveTerrainEnabled
        && scene.GetFeatureUsage().hasTerrainSurface)
    {
        m_sharedResources->EnsureInteractiveTerrainInitialized();
    }
    const RenderFeatureFrameContext featureFrameContext{
        m_logicalFrameId,
        m_viewId,
        m_activeFrame,
        frame.GetFrameWidth(),
        frame.GetFrameHeight(),
        scene};
    m_featureRegistry.PrepareLifecycleFeatures(
        featureFrameContext,
        RenderFeatureScope::ViewLocal);
    if (m_sharedSimulationProducerThisFrame)
    {
        m_featureRegistry.PrepareLifecycleFeatures(
            featureFrameContext,
            RenderFeatureScope::DeviceShared);
    }
    // Ocean quality and VT residency can both replace resources during frame
    // preparation. Rewrite only the recycled frame slot after all producers
    // have published their current generation.
    UpdateFeatureTextureBindings();
    const VirtualTextureStatistics& virtualTextureStatistics =
        m_virtualTextureCache.GetStatistics();
    m_statistics.virtualTextureResidentPages =
        virtualTextureStatistics.residentPages;
    m_statistics.virtualTextureRequestedPages =
        virtualTextureStatistics.requestedPages;
    m_statistics.virtualTexturePageHits = virtualTextureStatistics.pageHits;
    m_statistics.virtualTexturePageMisses = virtualTextureStatistics.pageMisses;
    m_statistics.virtualTextureEvictions = virtualTextureStatistics.evictions;
    if (hpWaterVisibility)
    {
        const Core::Double3& cameraWorldPosition =
            visibilityCamera.GetWorldPosition();
        WaterMediumSample mediumSample{};
        mediumSample.cameraHeightMeters =
            static_cast<float>(cameraWorldPosition.y);
        mediumSample.meanSeaLevelMeters = 0.0f;
        mediumSample.hysteresisMeters =
            m_settings.ocean.optics.volumetrics.surfaceHysteresisMeters;
        const DirectX::XMFLOAT2 cameraXZ{
            static_cast<float>(cameraWorldPosition.x),
            static_cast<float>(cameraWorldPosition.z)};
        const float queryDeltaX = cameraXZ.x - m_cameraWaterHeight.position.x;
        const float queryDeltaZ = cameraXZ.y - m_cameraWaterHeight.position.y;
        constexpr float CameraQueryPositionToleranceMeters = 0.5f;
        const std::uint64_t queryVersion = m_waterCameraCutVersion
            + m_waterSurfaceHistoryVersion + m_oceanActiveSettingsVersion;
        mediumSample.coherentQueryAvailable = m_cameraWaterHeight.valid
            && m_cameraWaterHeight.version == queryVersion
            && m_oceanFrameSerial - m_cameraWaterHeight.frame <= 30u
            && queryDeltaX * queryDeltaX + queryDeltaZ * queryDeltaZ
                <= CameraQueryPositionToleranceMeters
                    * CameraQueryPositionToleranceMeters;
        mediumSample.queriedDisplacementMeters =
            m_cameraWaterHeight.displacement;
        mediumSample.queryVersion = m_cameraWaterHeight.frame;
        (void)m_waterOpticsFeature.UpdateCameraMedium(mediumSample);
        if (m_sharedSimulationProducerThisFrame
            && !m_pendingOceanQuery.has_value()
            && m_oceanQueryDescriptorSet == nullptr
            && m_settings.ocean.query.enableGpuQueries)
        {
            PendingOceanQuery query{};
            query.sequenceId = m_oceanFrameSerial;
            query.simulationTimeSeconds = simulationTimeSeconds;
            query.points.push_back({cameraXZ});
            query.cameraMediumQuery = true;
            m_pendingOceanQuery = std::move(query);
        }
        WaterOpticsCompositeBindings compositeBindings{};
        compositeBindings.frameIndex = m_activeFrame;
        compositeBindings.localWavesEnabled = m_settings.ocean.local.enabled
            && m_localWaveGpuResources->IsGpuReady();
        compositeBindings.surfaceHistoryVersion =
            m_waterSurfaceHistoryVersion;
        compositeBindings.cameraCutVersion = m_waterCameraCutVersion;
        compositeBindings.frameConstants = &m_frameBuffers;
        compositeBindings.environment =
            m_environmentCubemap->GetRhiTexture();
        compositeBindings.shadowMap = m_shadowSampledView;
        compositeBindings.shadowMoments =
            m_varianceShadowMaps.GetMomentsSampledView();
        compositeBindings.atmosphereSkyView =
            m_skyAtmosphere.GetSkyViewSampledView(
                m_settings.physicalAtmosphereEnabled && m_settings.skyboxEnabled);
        compositeBindings.slopeMoments =
            m_spectralOcean->SlopeMomentMap().sampledArray;
        compositeBindings.spectralGradient =
            m_spectralOcean->GradientMap().sampledArray;
        compositeBindings.localGradient =
            compositeBindings.localWavesEnabled
                ? m_localWaveGpuResources->Gradient().sampled
                : m_virtualTextureCache.GetAtlasSampledView();
        compositeBindings.localDomainCenter =
            m_settings.ocean.local.domainCenter;
        compositeBindings.localDomainSizeMeters =
            m_settings.ocean.local.domainSizeMeters;
        compositeBindings.sceneMotion =
            m_temporalAntiAliasing.GetMotionVectorTextureShared();
        m_waterOpticsFeature.EnsureResources(
            device,
            frame.GetFrameWidth(),
            frame.GetFrameHeight(),
            depthTextureShared,
            m_depthSampledView,
            m_hdrTexture,
            m_hdrSampledView,
            m_settings.ocean.optics,
            m_oceanFrameSerial,
            &compositeBindings);
    }
    Core::CpuTraceSpan postProcessBindingsSpan(
        "PostProcessInputBindings", "renderer");
    UpdatePostProcessInputBindings();
    postProcessBindingsSpan.End();

    const auto graphBuildCpuStart = std::chrono::steady_clock::now();
    Core::CpuTraceSpan graphBuildSpan("RenderGraphBuild", "rdg");
    // Reset before Feature publication so typed Blackboard contributions and
    // the pipeline frontend share the same graph generation.
    m_renderGraph.Reset();
    SharedRenderGraphResources graphResources{};
    SharedRenderGraphCallbacks graphCallbacks{};
    graphResources.environment = m_environmentCubemap->GetRhiTexture().get();
    graphResources.frameConstants = m_frameBuffers[m_activeFrame].get();
    RenderFeatureGraphContext featureGraphContext{
        m_renderGraph,
        m_logicalFrameId,
        m_viewId,
        m_activeFrame,
        renderExtent.width,
        renderExtent.height,
        scene,
        m_gpuDrivenActiveThisFrame,
        sceneInput};
    m_featureRegistry.BuildLifecycleStage(
        RenderFeatureStage::FramePreparation,
        featureGraphContext);
    m_featureRegistry.BuildLifecycleStage(
        RenderFeatureStage::Shadows,
        featureGraphContext);
    m_featureRegistry.BuildLifecycleStage(
        RenderFeatureStage::Lighting,
        featureGraphContext);
    m_featureRegistry.BuildLifecycleStage(
        RenderFeatureStage::WaterOptics,
        featureGraphContext);
    m_featureRegistry.BuildLifecycleStage(
        RenderFeatureStage::PostProcess,
        featureGraphContext);
    m_featureRegistry.BuildLifecycleStage(
        RenderFeatureStage::Temporal,
        featureGraphContext);
    graphResources.shadowMap = m_shadowTexture.get();
    graphResources.depthBuffer = depthTexture;
    graphResources.hiZ = m_hiZTexture.get();
    graphResources.hdrColor = m_hdrTexture.get();
    graphResources.bloomA = m_bloomTextureA.get();
    graphResources.bloomB = m_bloomTextureB.get();
    graphResources.outputColor = outputTexture;
    if (m_sharedSimulationProducerThisFrame &&
        m_oceanQueryDescriptorSet == nullptr &&
        m_pendingOceanQuery.has_value() &&
        m_settings.ocean.implementation == OceanImplementation::SpectralOcean &&
        m_settings.fftOceanEnabled && m_settings.ocean.debug.renderWater &&
        m_spectralOcean->IsGpuReady())
    {
        const PendingOceanQuery& query = *m_pendingOceanQuery;
        m_oceanQueryCount = static_cast<std::uint32_t>(query.points.size());
        OceanGpuQueryConstants constants{};
        constants.queryCount = m_oceanQueryCount;
        constants.resolution = m_spectralOcean->Resolution();
        constants.localEnabled = m_settings.ocean.local.enabled &&
                                         m_localWaveGpuResources->IsGpuReady()
                                     ? 1u
                                     : 0u;
        constants.simulationTimeSeconds =
            static_cast<float>(query.simulationTimeSeconds);
        constants.domainSizeMeters = m_settings.ocean.local.domainSizeMeters;
        constants.domainCenter = m_settings.ocean.local.domainCenter;
        const auto& cascades = m_spectralOcean->SpectrumGenerator().GetCascades();
        constants.cascadePatchLengths = {
            cascades[0].patchLengthMeters, cascades[1].patchLengthMeters,
            cascades[2].patchLengthMeters, cascades[3].patchLengthMeters};
        constants.uvWarpParameters = {m_settings.ocean.shading.uvWarpingAmplitude,
            m_settings.ocean.shading.uvWarpingFrequency, 0.0f, 0.0f};
        RHI::BufferDescription constantDescription{};
        constantDescription.size = sizeof(constants);
        constantDescription.stride = sizeof(constants);
        constantDescription.usage = RHI::BufferUsage::Constant;
        constantDescription.memoryAccess = RHI::MemoryAccess::CpuToGpu;
        RHI::BufferDescription pointsDescription{};
        pointsDescription.size =
            query.points.size() * sizeof(DirectX::XMFLOAT2);
        pointsDescription.stride = sizeof(DirectX::XMFLOAT2);
        pointsDescription.usage = RHI::BufferUsage::ShaderResource;
        pointsDescription.memoryAccess = RHI::MemoryAccess::CpuToGpu;
        RHI::BufferDescription resultDescription{};
        resultDescription.size =
            query.points.size() * sizeof(DirectX::XMFLOAT4);
        resultDescription.stride = sizeof(DirectX::XMFLOAT4);
        resultDescription.usage =
            RHI::BufferUsage::Storage | RHI::BufferUsage::CopySource
                | RHI::BufferUsage::CopyDestination;
        RHI::BufferDescription readbackDescription = resultDescription;
        readbackDescription.usage = RHI::BufferUsage::CopyDestination;
        readbackDescription.memoryAccess = RHI::MemoryAccess::GpuToCpu;
        std::vector<DirectX::XMFLOAT2> pointPayload;
        pointPayload.reserve(query.points.size());
        for (const OceanQueryPoint& point : query.points)
            pointPayload.push_back(point.worldPosition);
        m_oceanQueryConstants =
            device.CreateBuffer(constantDescription, &constants);
        m_oceanQueryPoints =
            device.CreateBuffer(pointsDescription, pointPayload.data());
        m_oceanQueryResults = device.CreateBuffer(resultDescription);
        m_oceanQueryReadback = device.CreateBuffer(readbackDescription);
        const auto localView =
            m_settings.ocean.local.enabled &&
                    m_localWaveGpuResources->IsGpuReady()
                ? m_localWaveGpuResources->Displacement().sampled
                : m_oceanQueryDummyLocalView;
        m_oceanQueryDescriptorSet = m_oceanGpuQuery.Bind({m_oceanQueryConstants,
            m_oceanQueryPoints,
            m_oceanQueryResults,
            m_spectralOcean->DisplacementMap().sampledArray,
            localView});
        if (query.cameraMediumQuery
            || m_oceanQueryService.SubmitGpuBatch(query.sequenceId,
                query.simulationTimeSeconds,
                m_oceanFrameSerial,
                query.points))
        {
            m_oceanQueryIsCameraMedium = query.cameraMediumQuery;
            m_cameraQuerySubmittedPosition = query.points[0].worldPosition;
            m_cameraQuerySubmittedVersion = m_waterCameraCutVersion
                + m_waterSurfaceHistoryVersion + m_oceanActiveSettingsVersion;
            m_oceanQuerySequenceId = query.sequenceId;
            m_oceanQuerySubmittedFrame = m_oceanFrameSerial;
            m_pendingOceanQuery.reset();
        }
    }
    const bool recordOceanQuery = m_oceanQueryDescriptorSet != nullptr
        && m_oceanQuerySubmittedFrame == m_oceanFrameSerial;
    if (recordOceanQuery)
    {
        graphResources.oceanQueryConstants = m_oceanQueryConstants.get();
        graphResources.oceanQueryPoints = m_oceanQueryPoints.get();
        graphResources.oceanQueryResults = m_oceanQueryResults.get();
        graphResources.oceanQueryReadback = m_oceanQueryReadback.get();
        graphResources.oceanQueryCount = m_oceanQueryCount;
    }
    for (std::uint32_t index = 0; index < GBufferCount; ++index)
    {
        graphResources.gbuffer[index] = m_gbufferTextures[index].get();
    }
    graphResources.shadowInitialState = m_shadowInitialized
                                            ? RHI::ResourceState::ShaderResource
                                            : RHI::ResourceState::Undefined;
    graphResources.depthInitialState = m_depthState;
    graphResources.hiZInitialState = m_hiZState;
    graphResources.gbufferInitialStates = m_gbufferStates;
    graphResources.hdrInitialState = m_hdrState;
    graphResources.bloomAInitialState = m_bloomAState;
    graphResources.bloomBInitialState = m_bloomBState;
    graphResources.outputInitialState =
        m_renderToSwapChain ? RHI::ResourceState::Present : m_finalOutputState;

    graphCallbacks.shadow = [this, sceneInput](RHI::ICommandContext &commandContext,
                                           const RenderGraphPassResources &)
    { RenderShadowPass(commandContext, *sceneInput); };
    graphCallbacks.gbuffer =
        [this, &frame, sceneInput, depthView](RHI::ICommandContext &commandContext,
                                          const RenderGraphPassResources &)
    {
        RenderGBufferPass(commandContext, *depthView, frame.GetFrameWidth(),
                          frame.GetFrameHeight(), *sceneInput);
    };
    graphCallbacks.forwardGeometry =
        [this, &frame, sceneInput, depthView](RHI::ICommandContext &commandContext,
                                          const RenderGraphPassResources &)
    {
        RenderForwardPass(commandContext, *depthView, frame.GetFrameWidth(),
                          frame.GetFrameHeight(), *sceneInput);
    };
    const RHI::ITextureView* const transparencyDepthView =
        hpWaterVisibility
            ? &m_waterOpticsFeature.CompositeDepthView()
            : depthView;
    graphCallbacks.transparentGeometry =
        [this, &frame, sceneInput, transparencyDepthView](
            RHI::ICommandContext &commandContext,
            const RenderGraphPassResources &)
    {
        RenderTransparentPass(commandContext, *transparencyDepthView,
                              frame.GetFrameWidth(),
                              frame.GetFrameHeight(), *sceneInput);
    };
    graphCallbacks.deferredLighting =
        [this, renderExtent](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            RenderDeferredPass(
                commandContext,
                renderExtent.width,
                renderExtent.height);
        };
    graphCallbacks.hiZ = [this, renderExtent](
                             RHI::ICommandContext& commandContext,
                             const RenderGraphPassResources&,
                             const std::uint32_t mipIndex)
    {
        RenderHiZMip(
            commandContext, mipIndex, renderExtent.width, renderExtent.height);
    };
    graphCallbacks.bloomExtract = [this](RHI::ICommandContext& commandContext,
                                      const RenderGraphPassResources&)
    { RenderBloomExtractPass(commandContext); };
    graphCallbacks.bloomHorizontal = [this](
                                         RHI::ICommandContext& commandContext,
                                         const RenderGraphPassResources&)
    { RenderBloomHorizontalPass(commandContext); };
    graphCallbacks.bloomVertical = [this](RHI::ICommandContext& commandContext,
                                       const RenderGraphPassResources&)
    { RenderBloomVerticalPass(commandContext); };
    graphCallbacks.tonemap = [this, outputView, renderExtent](
                                 RHI::ICommandContext& commandContext,
                                 const RenderGraphPassResources&)
    {
        RenderTonemapPass(commandContext,
            *outputView,
            renderExtent.width,
            renderExtent.height);
    };
    if (recordOceanQuery)
    {
        graphCallbacks.oceanQuery = [this](RHI::ICommandContext& commandContext,
                                        const RenderGraphPassResources&)
        {
            m_oceanGpuQuery.Execute(
                commandContext, *m_oceanQueryDescriptorSet, m_oceanQueryCount);
            commandContext.BufferBarrier({m_oceanQueryResults.get(),
                RHI::ResourceState::UnorderedAccess,
                RHI::ResourceState::CopySource,
                0u,
                m_oceanQueryResults->GetDescription().size});
            commandContext.CopyBuffer(*m_oceanQueryResults,
                *m_oceanQueryReadback,
                m_oceanQueryResults->GetDescription().size);
            // The graph declares this pass's final result state as UAV.
            // Restore it before the graph emits any cross-queue handoff.
            commandContext.BufferBarrier({m_oceanQueryResults.get(),
                RHI::ResourceState::CopySource, RHI::ResourceState::UnorderedAccess,
                0u, m_oceanQueryResults->GetDescription().size});
        };
    }
    RenderSettings effectiveSettings = m_settings;
    ScenePipelineFrameState pipelineFrameState{};
    if (scene.GetPacket() != nullptr)
    {
        pipelineFrameState.usage =
            scene.GetPacket()->GetFeatureUsage();
    }
    pipelineFrameState.terrainPendingWork =
        m_sharedResources->GetInteractiveTerrain().HasPendingUpdate();
    pipelineFrameState.sharedSimulationProducer =
        m_sharedSimulationProducerThisFrame;
    pipelineFrameState.gpuDrivenSupported =
        m_gpuDrivenSupported;
    ScenePipelinePlan pipelinePlan = BuildScenePipelinePlan(effectiveSettings,
        pipelineFrameState,
        m_renderToSwapChain ? RHI::ResourceState::RenderTarget
                            : RHI::ResourceState::ShaderResource);
    m_lastPipelinePlan = pipelinePlan;
    m_lastPipelineFrameState = pipelineFrameState;
    pipelinePlan.graphOptions.viewId = m_viewId;
    if (!m_sharedSimulationProducerThisFrame)
    {
        // Secondary views import and sample the producer's published maps,
        // but must not schedule a second spectrum/FFT/foam/local-wave chain.
        pipelinePlan.graphOptions.oceanSimulationEnabled = false;
    }
    pipelinePlan.graphOptions.oceanQueriesEnabled =
        recordOceanQuery;
    (void)SharedRenderGraphFrontend::Build(m_renderGraph,
        graphResources,
        pipelinePlan.graphOptions,
        graphCallbacks);
    graphBuildSpan.End();
    const RenderGraph::QueueExecutionMode queueMode =
        RenderGraph::ParseQueueExecutionMode(
            ReadRenderEnvironmentVariable("PRISM_RENDER_RDG_QUEUE_MODE"));
    m_renderGraph.SetQueueExecutionMode(queueMode);
    m_renderGraph.Compile();
    m_queueCostModel.UpdateDimensions(renderExtent.width, renderExtent.height);
    QueueSchedulingDecision queueDecision{};
    if (queueMode == RenderGraph::QueueExecutionMode::Automatic)
    {
        queueDecision = m_queueCostModel.Evaluate(
            m_renderGraph.GetQueueSchedulingProfile());
    }
    else
    {
        queueDecision.selectNative =
            queueMode == RenderGraph::QueueExecutionMode::Native;
        queueDecision.reason =
            queueDecision.selectNative ? "forced_native" : "forced_serial";
    }
    m_renderGraph.SetAutomaticQueueDecision(std::move(queueDecision));
    m_statistics.renderGraphBuildCpuMs = static_cast<float>(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now()
            - graphBuildCpuStart).count());
    const auto graphExecuteCpuStart = std::chrono::steady_clock::now();
    Core::CpuTraceSpan graphExecuteSpan("RenderGraphExecute", "rdg");
    std::optional<Core::CpuTraceSpan> activePassSpan;
    const auto beginPass =
        [&](const std::string_view name)
        {
            activePassSpan.emplace(name, "rdg");
            m_gpuProfiler.BeginPass(backend, name);
        };
    const auto endPass =
        [&](const std::string_view name)
        {
            m_gpuProfiler.EndPass(backend);
            // Called on the replay lane after the pass body (including joined
            // parallel recordings), not by graph-building or recording workers.
            if (m_frameDiagnosticsEnabled)
                m_diagnosticRecordedPasses.emplace_back(name);
            activePassSpan.reset();
        };
    if (m_taskExecutor != nullptr)
    {
        m_renderGraph.Execute(
            backend.GetCommandContext(),
            *m_taskExecutor,
            beginPass,
            endPass);
    }
    else
    {
        m_renderGraph.Execute(
            backend.GetCommandContext(),
            beginPass,
            endPass);
    }
    graphExecuteSpan.End();
    m_statistics.renderGraphExecuteCpuMs = static_cast<float>(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now()
            - graphExecuteCpuStart).count());
    const RenderGraph::CompilationSummary& executionSummary =
        m_renderGraph.GetCompilationSummary();
    m_queueTimingObservations[m_activeFrame] = {
        m_renderGraph.GetQueueSchedulingProfile(),
        executionSummary.nativeMultiQueueSubmissionApplied,
        true};
    Core::CpuTraceSpan gpuProfilerEndSpan(
        "GpuProfilerEndFrame", "renderer");
    m_gpuProfiler.EndFrame(backend);
    gpuProfilerEndSpan.End();
    m_statistics.forwardDrawCalls = pipelinePlan.execution.forwardGeometry
                                        ? m_statistics.visibleObjects
                                        : 0u;
    m_statistics.directionalShadowDrawCalls =
        pipelinePlan.execution.shadows ? m_statistics.visibleObjects : 0u;
    const LocalShadowStatistics& localShadowStatistics =
        m_localLightShadows.GetStatistics();
    m_statistics.localShadowDrawCalls = pipelinePlan.execution.localLightShadows
                                            ? localShadowStatistics.drawCalls
                                            : 0u;
    m_statistics.shadowLayersRendered = localShadowStatistics.renderedLayers;
    m_statistics.shadowLayersCached = localShadowStatistics.cachedLayers;
    m_statistics.shadowCasterCandidates =
        localShadowStatistics.casterCandidates;
    m_statistics.shadowCastersCulled = localShadowStatistics.culledCasters;
    m_statistics.shadowDrawCalls = m_statistics.directionalShadowDrawCalls +
                                   m_statistics.localShadowDrawCalls;
    // Bloom is compute work; only fullscreen raster passes are draw calls.
    m_statistics.postProcessDrawCalls = 1u;
    m_statistics.indirectDrawCalls =
        pipelinePlan.execution.gpuDriven
            ? m_statistics.gpuDrivenCandidateObjects
            : 0u;
    const std::uint32_t geometryDrawCalls = pipelinePlan.execution.gpuDriven
                                                ? m_statistics.indirectDrawCalls
                                                : m_statistics.visibleObjects;
    m_statistics.drawCalls = geometryDrawCalls + m_statistics.shadowDrawCalls +
                             m_statistics.postProcessDrawCalls;
    m_statistics.cachedPsoCount =
        static_cast<std::uint32_t>(GetPipelineCache().GetPipelineCount());
    m_statistics.shadowPassGpuMs = m_gpuProfiler.GetMilliseconds("Shadow");
    m_statistics.geometryPassGpuMs = m_gpuProfiler.GetMilliseconds("GBuffer");
    m_statistics.bloomPassGpuMs = m_gpuProfiler.GetMilliseconds("BloomExtract");
    m_statistics.tonemapPassGpuMs = m_gpuProfiler.GetMilliseconds("Tonemap");
    m_statistics.totalRendererGpuMs = m_gpuProfiler.GetMilliseconds("Renderer");
    const auto& oceanTimeline = m_gpuProfiler.GetTimelineMetadata();
    const bool oceanGpuTimersAvailable =
        m_settings.ocean.query.enableGpuTimers &&
        (oceanTimeline.graphicsTimestampFrequency != 0u ||
            oceanTimeline.computeTimestampFrequency != 0u);
    const auto oceanGpuMilliseconds = [this, oceanGpuTimersAvailable](
                                          const std::string_view name)
    {
        return oceanGpuTimersAvailable ? m_gpuProfiler.GetMilliseconds(name)
                                       : 0.0f;
    };
    if (m_sharedSimulationProducerThisFrame)
    {
        m_spectralOcean->UpdateGpuTimings(
            oceanGpuMilliseconds("SpectralOcean.InitialSpectrum") +
                oceanGpuMilliseconds("SpectralOcean.Evolution"),
            oceanGpuMilliseconds("SpectralOcean.Fft.Horizontal"),
            oceanGpuMilliseconds("SpectralOcean.Fft.Vertical"),
            oceanGpuMilliseconds("SpectralOcean.OutputMaps"),
            oceanGpuMilliseconds("SpectralOcean.Foam"),
            oceanGpuMilliseconds("SpectralOcean.Mips"),
            oceanGpuTimersAvailable);
    }
    m_statistics.ocean = pipelinePlan.execution.spectralOcean
                             ? m_spectralOcean->GetStatistics()
                             : OceanStatistics{};
    m_statistics.ocean.geometryInstanceCount = m_oceanGeometryInstanceCount;
    m_statistics.ocean.geometryNodes = m_oceanGeometryNodeCount;
    m_statistics.ocean.visibleGeometryNodes = m_oceanVisibleGeometryNodeCount;
    m_statistics.ocean.geometryRefinementIterations =
        m_oceanGeometryRefinementIterations;
    m_statistics.ocean.geometryMaxLod = m_oceanGeometryMaxLod;
    m_statistics.ocean.geometryMaxTessellationFactor =
        m_oceanGeometryMaxTessellationFactor;
    m_statistics.ocean.tessellationActive = m_oceanTessellationActive;
    m_statistics.ocean.localWaveMilliseconds =
        oceanGpuMilliseconds("Ocean.LocalWave.Simulation");
    m_statistics.ocean.waterVisibilityMilliseconds =
        oceanGpuMilliseconds("WaterOptics.Visibility");
    m_statistics.ocean.waterVisibilityDrawCount =
        pipelinePlan.execution.waterVisibility
            ? std::max(m_oceanGeometryInstanceCount, 1u)
            : 0u;
    const float waterRefractionMilliseconds =
        oceanGpuMilliseconds("WaterOptics.Refraction.Approximate")
        + oceanGpuMilliseconds("WaterOptics.Refraction.RayMarch");
    m_waterOpticsFeature.UpdateGpuTiming(
        waterRefractionMilliseconds, oceanGpuTimersAvailable);
    const float waterCompositeMilliseconds =
        oceanGpuMilliseconds("WaterOptics.Composite");
    m_waterOpticsFeature.UpdateCompositeGpuTiming(
        waterCompositeMilliseconds, oceanGpuTimersAvailable);
    m_waterOpticsFeature.UpdateCausticsGpuTiming(
        oceanGpuMilliseconds("WaterOptics.Caustics.Accumulate"),
        oceanGpuTimersAvailable);
    m_waterOpticsFeature.UpdateVolumetricGpuTiming(
        oceanGpuMilliseconds("WaterOptics.Volume.Accumulate"),
        oceanGpuMilliseconds("WaterOptics.Volume.Temporal"),
        oceanGpuMilliseconds("WaterOptics.Volume.Reconstruct"),
        oceanGpuTimersAvailable);
    const WaterOpticsFeatureStatistics& waterStatistics =
        m_waterOpticsFeature.GetStatistics();
    m_statistics.ocean.waterRefractionMilliseconds =
        waterStatistics.refractionMilliseconds;
    m_statistics.ocean.waterPixelCoverage = waterStatistics.waterPixelCoverage;
    m_statistics.ocean.waterCoverageAvailable = waterStatistics.waterCoverageAvailable;
    m_statistics.ocean.waterCompositeMilliseconds =
        waterStatistics.compositeMilliseconds;
    m_statistics.ocean.waterOpticsDispatchCount =
        waterStatistics.refractionDispatchCount
        + waterStatistics.compositeDispatchCount
        + waterStatistics.causticsDispatchCount
        + waterStatistics.volumetricDispatchCount
        + (waterStatistics.compositeEnabled ? 1u : 0u);
    m_statistics.ocean.waterCausticsMilliseconds =
        waterStatistics.causticsMilliseconds;
    m_statistics.ocean.waterVolumetricsMilliseconds =
        waterStatistics.volumetricAccumulateMilliseconds
        + waterStatistics.volumetricTemporalMilliseconds;
    m_statistics.ocean.waterVolumetricReconstructionMilliseconds =
        waterStatistics.volumetricReconstructMilliseconds;
    m_statistics.ocean.waterRefractionEffectiveSamples =
        waterStatistics.effectiveRayMarchSamples;
    m_statistics.ocean.waterRefractionWidth = waterStatistics.effectiveRefractionWidth;
    m_statistics.ocean.waterRefractionHeight = waterStatistics.effectiveRefractionHeight;
    m_statistics.ocean.waterVolumetricWidth = waterStatistics.effectiveVolumetricWidth;
    m_statistics.ocean.waterVolumetricHeight = waterStatistics.effectiveVolumetricHeight;
    m_statistics.ocean.waterOpticsQuality = static_cast<std::uint32_t>(waterStatistics.effectiveQuality);
    m_statistics.ocean.waterMediumVersion = waterStatistics.mediumVersion;
    m_statistics.ocean.waterCameraUnderwater = waterStatistics.cameraUnderwater;
    m_statistics.ocean.waterMediumFallback = waterStatistics.mediumUsesMeanSeaLevelFallback;
    m_statistics.ocean.waterVolumetricHistoryValid = waterStatistics.volumetricHistoryValid;
    m_statistics.ocean.waterVolumetricsActive = waterStatistics.volumetricsEnabled;
    m_statistics.ocean.waterOpticsSurfaceHistoryVersion = m_waterSurfaceHistoryVersion;
    m_statistics.ocean.waterRefractionRayMarchActive =
        waterStatistics.rayMarchEnabled;
    m_statistics.ocean.waterAllocatedMegabytes =
        waterStatistics.allocatedMegabytes;
    m_statistics.ocean.waterOpticsHistoryVersion =
        waterStatistics.historyVersion;
    m_statistics.ocean.waterOpticsActive =
        pipelinePlan.execution.waterVisibility;
    m_statistics.ocean.waterOpticsHistoryValid =
        waterStatistics.historyValid;
    m_statistics.ocean.surfaceMilliseconds = oceanGpuMilliseconds(
        pipelinePlan.execution.forwardGeometry
            ? "ForwardGeometry" : "GBuffer");
    m_statistics.ocean.rendererGpuMilliseconds =
        m_statistics.totalRendererGpuMs;
    m_statistics.ocean.outputWidth = renderExtent.width;
    m_statistics.ocean.outputHeight = renderExtent.height;
    m_statistics.ocean.activeRenderedViews = 1u;
    m_statistics.ocean.quality =
        static_cast<std::uint32_t>(m_settings.ocean.quality);
    m_statistics.ocean.localWaveSharedTime =
        m_settings.ocean.local.shareSpectralTime;
    m_statistics.ocean.pendingDirtyScopes =
        static_cast<std::uint32_t>(m_pendingOceanDirtyScopes);
    m_statistics.ocean.requestedSettingsVersion =
        m_oceanRequestedSettingsVersion;
    m_statistics.ocean.activeSettingsVersion =
        m_oceanActiveSettingsVersion;
    m_statistics.ocean.dispatchCount +=
        m_localWaveGpuResources->DispatchCount();
    m_statistics.ocean.localWaveSimulationActive =
        m_localWaveGpuResources->DispatchCount() != 0u;
    m_statistics.ocean.localWaveEmitterActive =
        m_settings.ocean.local.demoEmittersEnabled
        && (m_settings.ocean.local.rainEmitterEnabled
            || m_settings.ocean.local.wakeEmitterEnabled);
    m_statistics.ocean.localWaveGridSize = m_localWaveGpuResources->GridSize();
    m_statistics.ocean.localWaveAllocatedMegabytes =
        m_localWaveGpuResources->AllocatedMegabytes();
    m_statistics.ocean.localWaveResourceGeneration =
        m_localWaveGpuResources->ResourceGeneration();
    m_statistics.ocean.conservativeMaxDisplacement =
        m_oceanQueryService.Bounds().radius;
    m_statistics.ocean.queryPending =
        static_cast<std::uint32_t>(m_oceanQueryService.PendingCount());
    m_statistics.ocean.queryCompleted =
        static_cast<std::uint32_t>(m_oceanQueryService.CompletedCount());
    m_statistics.ocean.queryExpired = m_oceanQueryService.ExpiredCount();
    m_statistics.ocean.readbackLatencyFrames =
        m_oceanQueryService.LastReadbackLatencyFrames();
    m_statistics.ocean.queryGpuBatches = m_oceanQueryService.GpuBatchCount();
    m_statistics.ocean.queryReadbackCopies =
        m_oceanQueryService.ReadbackCopyRequestCount();
    m_statistics.ocean.queryResourceAllocated =
        m_oceanQueryService.HasAllocatedQueryResource();
    const float oceanCpuSample =
        static_cast<float>(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - oceanCpuStart)
                .count());
    m_oceanCpuMillisecondsFiltered +=
        0.25f * (oceanCpuSample - m_oceanCpuMillisecondsFiltered);
    m_statistics.ocean.cpuTotalMilliseconds = m_oceanCpuMillisecondsFiltered;
    m_statistics.ocean.cpuTimersAvailable =
        m_settings.ocean.query.enableCpuTimers;
    if (m_sharedSimulationProducerThisFrame
        && m_settings.ocean.implementation == OceanImplementation::SpectralOcean
        && (m_oceanFrameSerial % 60u) == 0u)
    {
        Core::DiagnosticLog::Write("info", "ocean.performance",
            "Spectral ocean performance sample.",
            {{"quality", static_cast<std::uint32_t>(m_settings.ocean.quality)},
                {"resolution", m_statistics.ocean.cascadeResolution},
                {"gpuMilliseconds", m_statistics.ocean.gpuTotalMilliseconds},
                {"spectrumMilliseconds", m_statistics.ocean.spectrumMilliseconds},
                {"fftMilliseconds", m_statistics.ocean.fftMilliseconds},
                {"mapMilliseconds", m_statistics.ocean.mapMilliseconds},
                {"foamMilliseconds", m_statistics.ocean.foamMilliseconds},
                {"mipMilliseconds", m_statistics.ocean.mipMilliseconds},
                {"localWaveMilliseconds", m_statistics.ocean.localWaveMilliseconds},
                {"surfaceMilliseconds", m_statistics.ocean.surfaceMilliseconds},
                {"waterRefractionMilliseconds",
                    m_statistics.ocean.waterRefractionMilliseconds},
                {"waterCompositeMilliseconds",
                    m_statistics.ocean.waterCompositeMilliseconds},
                {"waterRefractionEffectiveSamples",
                    m_statistics.ocean.waterRefractionEffectiveSamples},
                {"waterRefractionRayMarchActive",
                    m_statistics.ocean.waterRefractionRayMarchActive},
                {"outputWidth", m_statistics.ocean.outputWidth},
                {"outputHeight", m_statistics.ocean.outputHeight},
                {"activeRenderedViews", m_statistics.ocean.activeRenderedViews},
                {"localWaveGrid", m_statistics.ocean.localWaveGridSize},
                {"localEmitterActive", m_statistics.ocean.localWaveEmitterActive},
                {"localSimulationActive", m_statistics.ocean.localWaveSimulationActive},
                {"cpuMilliseconds", m_statistics.ocean.cpuTotalMilliseconds},
                {"memoryMegabytes", m_statistics.ocean.allocatedMegabytes},
                {"dispatchCount", m_statistics.ocean.dispatchCount},
                {"visibleGeometryNodes", m_statistics.ocean.visibleGeometryNodes},
                {"nativeMultiQueue", executionSummary.nativeMultiQueueSubmissionApplied},
                {"estimatedOverlapMilliseconds",
                    executionSummary.automaticQueueDecision.estimatedOverlapMilliseconds},
                {"rendererGpuMilliseconds", m_statistics.totalRendererGpuMs}});
    }
    m_statistics.fluid = m_fluidFeature.GetStatistics();
    m_shadowInitialized = true; // Lighting always prepares its sampled descriptor.
    if (pipelinePlan.execution.deferredGeometry)
    {
        m_gbufferStates.fill(RHI::ResourceState::ShaderResource);
    }
    m_hdrState = RHI::ResourceState::ShaderResource;
    if (pipelinePlan.execution.bloom)
    {
        m_bloomAState = RHI::ResourceState::ShaderResource;
        m_bloomBState = RHI::ResourceState::ShaderResource;
    }
    if (!m_renderToSwapChain)
    {
        m_finalOutputState = RHI::ResourceState::ShaderResource;
    }
    m_depthState = pipelinePlan.execution.hiZ || pipelinePlan.execution.fluid
                       ? RHI::ResourceState::ShaderResource
                       : RHI::ResourceState::DepthWrite;
    if (pipelinePlan.execution.hiZ)
    {
        m_hiZState = RHI::ResourceState::ShaderResource;
        m_hiZValid = true;
    }
    m_waterOpticsFeature.EndFrame(
        m_oceanFrameSerial,
        pipelinePlan.execution.waterVisibility);
    m_clusteredLighting.EndFrame(
        m_activeFrame, pipelinePlan.execution.clusteredLighting);
    m_localLightShadows.EndFrame(pipelinePlan.execution.localLightShadows);
    m_planarReflections.EndFrame();
    m_varianceShadowMaps.EndFrame(pipelinePlan.execution.varianceShadows);
    m_screenSpaceEffects.EndFrame(pipelinePlan.execution.gtao,
        pipelinePlan.execution.screenSpaceComposite);
    m_skyAtmosphere.EndFrame(pipelinePlan.execution.skyAtmosphere);
    m_fftOcean.EndFrame(pipelinePlan.execution.fftOcean);
    if (m_sharedSimulationProducerThisFrame)
    {
        m_spectralOcean->EndFrame(
            pipelinePlan.execution.spectralOcean &&
            pipelinePlan.graphOptions.oceanSimulationEnabled);
        m_localWaveGpuResources->EndFrame(
            pipelinePlan.execution.spectralOcean &&
            pipelinePlan.graphOptions.localWaveEnabled);
    }
    m_sharedResources->GetInteractiveTerrain().EndFrame(
        pipelinePlan.graphOptions.interactiveTerrainUpdateEnabled);
    m_fluidFeature.EndFrame(pipelinePlan.execution.fluid);
    if (pipelinePlan.execution.temporalAntiAliasing)
    {
        m_temporalAntiAliasing.EndFrame();
    }
    else
    {
        m_temporalAntiAliasing.ResetHistory();
        m_temporalSampleIndex = 0;
        if (pipelinePlan.execution.waterVisibility)
            m_temporalAntiAliasing.NotifyMotionFinalState(RHI::ResourceState::UnorderedAccess);
        else if (pipelinePlan.execution.deferredGeometry)
            m_temporalAntiAliasing.NotifyMotionFinalState(RHI::ResourceState::RenderTarget);
    }
    m_statistics.totalRendererCpuMs = static_cast<float>(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now()
            - rendererCpuStart).count());
}

void SceneRenderer::SetSharedSimulationProducerForFrame(
    const LogicalFrameId logicalFrameId,
    const bool producer)
{
    Core::Check(
        logicalFrameId > 0,
        "Shared simulation producer selection requires a logical frame id.");
    Core::Check(
        logicalFrameId >= m_scheduledLogicalFrameId,
        "Shared simulation producer selection cannot move backwards.");
    if (logicalFrameId == m_scheduledLogicalFrameId)
    {
        Core::Check(
            producer == m_sharedSimulationProducerThisFrame,
            "A logical frame cannot change its shared simulation producer.");
        return;
    }
    m_scheduledLogicalFrameId = logicalFrameId;
    m_sharedSimulationProducerThisFrame = producer;
}

void SceneRenderer::NotifySceneChanged(
    const Scene::RenderScene& scene,
    const bool notifySharedFeatures)
{
    ++m_waterSurfaceHistoryVersion;
    ++m_sceneRevision;
    m_cameraWaterHeight.valid = false;
    const Scene::RenderSceneView sceneView(
        Scene::RenderSceneControlView,
        scene);
    const RenderFeatureSceneContext sceneContext{
        sceneView, m_sceneRevision};
    m_featureRegistry.NotifyLifecycleSceneChanged(
        sceneContext,
        RenderFeatureScope::ViewLocal);
    if (notifySharedFeatures)
    {
        m_featureRegistry.NotifyLifecycleSceneChanged(
            sceneContext,
            RenderFeatureScope::DeviceShared);
    }
    m_hiZValid = false;
    m_historyCameraValid = false;
    m_previousWorldViewProjections.clear();
    m_currentWorldViewProjections.clear();
    m_sceneObjectBindings.clear();
    m_temporalSampleIndex = 0;
    m_fluidFeature.RequestReset();
}

void SceneRenderer::SetProfilingMode(
    const RHI::GpuProfiler::SamplingMode mode) noexcept
{
    m_gpuProfiler.SetSamplingMode(mode);
    m_renderGraph.SetDetailedProfilingEnabled(
        mode == RHI::GpuProfiler::SamplingMode::Detailed);
}

void SceneRenderer::RequestHistoryReset() noexcept
{
    // The next rendered frame performs the normal camera-cut reset path.
    // Keeping this as a request avoids advancing temporal state for a hidden
    // view that has not consumed a frame yet.
    m_historyCameraValid = false;
}

RHI::GpuProfiler::SamplingMode
SceneRenderer::GetProfilingMode() const noexcept
{
    return m_gpuProfiler.GetSamplingMode();
}

void SceneRenderer::UpdateHistoryValidity(
    const Scene::Camera& camera)
{
    bool cameraCut = !m_historyCameraValid;
    if (m_historyCameraValid)
    {
        const Core::Double3 positionDelta =
            camera.GetWorldPosition()
            - m_historyCamera.GetWorldPosition();
        const double distanceSquared =
            positionDelta.x * positionDelta.x
            + positionDelta.y * positionDelta.y
            + positionDelta.z * positionDelta.z;
        const double cutDistance = std::max(
            5.0,
            static_cast<double>(camera.GetFarPlane())
                * 0.05);
        const DirectX::XMFLOAT3 currentForward =
            camera.GetForwardVector();
        const DirectX::XMFLOAT3 previousForward =
            m_historyCamera.GetForwardVector();
        const float forwardDot =
            currentForward.x * previousForward.x
            + currentForward.y * previousForward.y
            + currentForward.z * previousForward.z;
        constexpr float ProjectionEpsilon = 0.0001f;
        cameraCut =
            distanceSquared > cutDistance * cutDistance
            || forwardDot < 0.8660254f
            || std::abs(
                   camera.GetFieldOfViewYRadians()
                   - m_historyCamera.GetFieldOfViewYRadians())
                > ProjectionEpsilon
            || std::abs(
                   camera.GetAspectRatio()
                   - m_historyCamera.GetAspectRatio())
                > ProjectionEpsilon
            || std::abs(
                   camera.GetNearPlane()
                   - m_historyCamera.GetNearPlane())
                > ProjectionEpsilon
            || std::abs(
                   camera.GetFarPlane()
                   - m_historyCamera.GetFarPlane())
                > ProjectionEpsilon;
    }
    if (cameraCut)
    {
        ++m_waterCameraCutVersion;
        m_cameraWaterHeight.valid = false;
        m_hiZValid = false;
        m_temporalAntiAliasing.ResetHistory();
        m_previousWorldViewProjections.clear();
        m_temporalSampleIndex = 0;
    }
    m_historyCamera = camera;
    m_historyCameraValid = true;
}

const RenderGraph& SceneRenderer::GetRenderGraph() const
{
    return m_renderGraph;
}

const std::vector<RHI::GpuProfiler::Timing>&
SceneRenderer::GetGpuTimings() const
{
    return m_gpuProfiler.GetLatestTimings();
}

const RHI::GpuProfiler::TimelineMetadata&
SceneRenderer::GetGpuTimelineMetadata() const
{
    return m_gpuProfiler.GetTimelineMetadata();
}

std::uint64_t SceneRenderer::GetGpuTimingGeneration() const noexcept
{
    return m_gpuProfiler.GetLatestTimingGeneration();
}

std::uint32_t SceneRenderer::GetGpuTimingFrameSlot() const noexcept
{
    return m_gpuProfiler.GetLatestResolvedFrameIndex();
}

void SceneRenderer::ResolveGpuTimings(RHI::IRenderBackend& backend)
{
    m_gpuProfiler.ResolveSubmittedFrame(backend);
    ObserveCompletedQueueTimings();
}

void SceneRenderer::ObserveCompletedQueueTimings()
{
    const std::uint64_t generation = m_gpuProfiler.GetLatestTimingGeneration();
    if (generation == 0 || generation == m_observedTimingGeneration)
    {
        return;
    }
    m_observedTimingGeneration = generation;
    const std::uint32_t frameIndex =
        m_gpuProfiler.GetLatestResolvedFrameIndex();
    if (frameIndex >= m_queueTimingObservations.size())
    {
        return;
    }
    const QueueTimingObservation& observation =
        m_queueTimingObservations[frameIndex];
    if (!observation.valid)
    {
        return;
    }
    std::vector<QueueTimingSample> samples;
    samples.reserve(m_gpuProfiler.GetLatestTimings().size());
    for (const RHI::GpuProfiler::Timing& timing :
        m_gpuProfiler.GetLatestTimings())
    {
        samples.push_back({timing.name,
            timing.milliseconds,
            timing.queue,
            timing.startMilliseconds,
            timing.endMilliseconds,
            timing.calibrated});
    }
    m_queueCostModel.Observe(
        observation.profile, observation.nativeMultiQueue, samples);
}

RenderSettings& SceneRenderer::GetSettings()
{
    return m_settings;
}

const RenderSettings& SceneRenderer::GetSettings() const { return m_settings; }

void SceneRenderer::SetEditorGridAllowed(const bool allowed)
{
    m_editorGridAllowed = allowed;
}

bool SceneRenderer::SupportsGpuDrivenRendering() const
{
    return m_gpuDrivenSupported;
}

const RendererStatistics& SceneRenderer::GetStatistics() const
{
    return m_statistics;
}

bool SceneRenderer::QueueOceanDisplacementQuery(const std::uint64_t sequenceId,
    const double simulationTimeSeconds,
    const std::span<const OceanQueryPoint> points)
{
    constexpr std::size_t MaxBatch = 256u;
    if (!m_viewIndependentSimulationProducer || points.empty() ||
        points.size() > MaxBatch || sequenceId == 0u)
        return false;
    PendingOceanQuery query{};
    query.sequenceId = sequenceId;
    query.simulationTimeSeconds = simulationTimeSeconds;
    query.points.assign(points.begin(), points.end());
    m_pendingOceanQuery = std::move(query);
    return true;
}

const std::vector<Scene::GpuVisibilityReason>&
SceneRenderer::GetGpuVisibilityReasons() const
{
    return m_gpuDrivenVisibility.GetResolvedReasons();
}

const Scene::Camera* SceneRenderer::GetGpuVisibilityCamera() const
{
    return m_gpuDrivenVisibility.GetResolvedCullingCamera();
}

const std::shared_ptr<const Scene::RenderViewFeedback>&
SceneRenderer::GetGpuVisibilityFeedback() const noexcept
{
    return m_gpuDrivenVisibility.GetResolvedFeedback();
}

const std::shared_ptr<RHI::ITextureView>&
SceneRenderer::GetFinalOutputSampledView() const
{
    return m_finalOutputSampledView;
}

const std::shared_ptr<RHI::ITexture>&
SceneRenderer::GetFinalOutputTexture() const
{
    return m_finalOutputTexture;
}

std::uint32_t SceneRenderer::GetFinalOutputWidth() const
{
    return m_finalOutputWidth;
}

std::uint32_t SceneRenderer::GetFinalOutputHeight() const
{
    return m_finalOutputHeight;
}

void SceneRenderer::CreateDescriptorResources(
    RHI::IGraphicsDevice& device, const RHI::ShaderBinaryFormat shaderFormat)
{
    const std::filesystem::path shaderDirectory = m_shaderPath.parent_path();
    const auto load =
        [&](const std::filesystem::path& file,
            const char* entry,
            const RHI::ShaderStage stage) -> const RHI::ShaderBinary&
    { return GetShaderManager().LoadShader(file, entry, stage, shaderFormat); };
    const auto buildLayout =
        [&](const std::span<const RHI::ShaderLayoutStage> stages,
            const std::span<const std::uint32_t> dynamicBindings)
    {
        return device.CreateDescriptorSetLayout(
            RHI::BuildDescriptorSetLayout(stages, dynamicBindings));
    };

    const RHI::ShaderBinary& meshVertex =
        load(m_shaderPath, "VSMain", RHI::ShaderStage::Vertex);
    const RHI::ShaderBinary& meshIndexedVertex =
        load(m_shaderPath, "VSIndexedMain", RHI::ShaderStage::Vertex);
    const RHI::ShaderBinary& meshPixel =
        load(m_shaderPath, "PSMain", RHI::ShaderStage::Pixel);
    const RHI::ShaderBinary& meshGBufferPixel =
        load(m_shaderPath, "GBufferPS", RHI::ShaderStage::Pixel);
    const std::filesystem::path oceanSurfacePath =
        shaderDirectory / "Ocean" / "OceanSurface.slang";
    const RHI::ShaderBinary& oceanHull =
        load(oceanSurfacePath, "OceanHullMain", RHI::ShaderStage::Hull);
    const RHI::ShaderBinary& oceanDomain =
        load(oceanSurfacePath, "OceanDomainMain", RHI::ShaderStage::Domain);
    const RHI::ShaderBinary& oceanVertex =
        load(oceanSurfacePath, "OceanVSMain", RHI::ShaderStage::Vertex);
    const RHI::ShaderBinary& oceanIndexedVertex = load(
        oceanSurfacePath, "OceanVSIndexedMain", RHI::ShaderStage::Vertex);
    const RHI::ShaderBinary& oceanPixel =
        load(oceanSurfacePath, "OceanPSMain", RHI::ShaderStage::Pixel);
    const RHI::ShaderBinary& oceanGBufferPixel =
        load(oceanSurfacePath, "OceanGBufferPS", RHI::ShaderStage::Pixel);
    const RHI::ShaderBinary& waterVisibilityPixel =
        load(shaderDirectory / "Ocean" / "WaterVisibility.slang",
            "WaterVisibilityPS", RHI::ShaderStage::Pixel);
    const std::array meshStages = {
        RHI::ShaderLayoutStage{
            &meshVertex.reflection, RHI::ShaderStage::Vertex},
        RHI::ShaderLayoutStage{
            &meshIndexedVertex.reflection, RHI::ShaderStage::Vertex},
        RHI::ShaderLayoutStage{&meshPixel.reflection, RHI::ShaderStage::Pixel},
        RHI::ShaderLayoutStage{
            &meshGBufferPixel.reflection, RHI::ShaderStage::Pixel}};
    const std::array<std::uint32_t, 1> objectDynamicBinding = {1};
    m_descriptorSetLayout = buildLayout(meshStages, objectDynamicBinding);
    const std::array oceanStages = {
        RHI::ShaderLayoutStage{
            &oceanVertex.reflection, RHI::ShaderStage::Vertex},
        RHI::ShaderLayoutStage{
            &oceanIndexedVertex.reflection, RHI::ShaderStage::Vertex},
        RHI::ShaderLayoutStage{
            &oceanHull.reflection, RHI::ShaderStage::Hull},
        RHI::ShaderLayoutStage{
            &oceanDomain.reflection, RHI::ShaderStage::Domain},
        RHI::ShaderLayoutStage{
            &oceanPixel.reflection, RHI::ShaderStage::Pixel},
        RHI::ShaderLayoutStage{
            &oceanGBufferPixel.reflection, RHI::ShaderStage::Pixel},
        RHI::ShaderLayoutStage{
            &waterVisibilityPixel.reflection, RHI::ShaderStage::Pixel}};
    m_oceanDescriptorSetLayout =
        buildLayout(oceanStages, objectDynamicBinding);

    const std::filesystem::path shadowPath = shaderDirectory / "Shadow.hlsl";
    const RHI::ShaderBinary& shadowVertex = load(
        shadowPath, "VSMain", RHI::ShaderStage::Vertex);
    const RHI::ShaderBinary& shadowPixel = load(
        shadowPath, "PSMain", RHI::ShaderStage::Pixel);
    const std::array shadowStages = {
        RHI::ShaderLayoutStage{&shadowVertex.reflection, RHI::ShaderStage::Vertex},
        RHI::ShaderLayoutStage{&shadowPixel.reflection, RHI::ShaderStage::Pixel}};
    const std::array<std::uint32_t, 2> shadowDynamicBindings = {1, 3};
    m_shadowDescriptorSetLayout = buildLayout(shadowStages, shadowDynamicBindings);

    const std::filesystem::path deferredPath = shaderDirectory / "Deferred.hlsl";
    const RHI::ShaderBinary& deferredVertex = load(
        deferredPath, "FullscreenVS", RHI::ShaderStage::Vertex);
    const RHI::ShaderBinary& deferredPixel = load(
        deferredPath, "DeferredLightingPS", RHI::ShaderStage::Pixel);
    const std::array deferredStages = {
        RHI::ShaderLayoutStage{&deferredVertex.reflection, RHI::ShaderStage::Vertex},
        RHI::ShaderLayoutStage{&deferredPixel.reflection, RHI::ShaderStage::Pixel}};
    m_deferredDescriptorSetLayout = buildLayout(deferredStages, {});

    const std::filesystem::path postProcessPath = shaderDirectory / "PostProcess.hlsl";
    const RHI::ShaderBinary& postVertex = load(
        postProcessPath, "FullscreenVS", RHI::ShaderStage::Vertex);
    const RHI::ShaderBinary& postPixel = load(
        postProcessPath, "TonemapPS", RHI::ShaderStage::Pixel);
    const RHI::ShaderBinary& skyPixel = load(
        postProcessPath, "SkyboxPS", RHI::ShaderStage::Pixel);
    const std::array postStages = {
        RHI::ShaderLayoutStage{&postVertex.reflection, RHI::ShaderStage::Vertex},
        RHI::ShaderLayoutStage{&postPixel.reflection, RHI::ShaderStage::Pixel},
        RHI::ShaderLayoutStage{&skyPixel.reflection, RHI::ShaderStage::Pixel}};
    m_postProcessDescriptorSetLayout = buildLayout(postStages, {});
    const RHI::ShaderBinary& brightExtractCompute = load(
        postProcessPath,
        "BrightExtractCS",
        RHI::ShaderStage::Compute);
    const RHI::ShaderBinary& blurHorizontalCompute = load(
        postProcessPath,
        "BlurHorizontalCS",
        RHI::ShaderStage::Compute);
    const RHI::ShaderBinary& blurVerticalCompute = load(
        postProcessPath,
        "BlurVerticalCS",
        RHI::ShaderStage::Compute);
    const std::array bloomComputeStages = {
        RHI::ShaderLayoutStage{
            &brightExtractCompute.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &blurHorizontalCompute.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &blurVerticalCompute.reflection,
            RHI::ShaderStage::Compute}};
    m_bloomComputeDescriptorSetLayout =
        buildLayout(bloomComputeStages, {});

    const std::filesystem::path hiZPath =
        shaderDirectory / "HiZ.hlsl";
    const RHI::ShaderBinary& hiZCopyCompute = load(
        hiZPath,
        "CopyDepthCS",
        RHI::ShaderStage::Compute);
    const RHI::ShaderBinary& hiZDownsampleCompute = load(
        hiZPath,
        "DownsampleDepthCS",
        RHI::ShaderStage::Compute);
    const std::array hiZStages = {
        RHI::ShaderLayoutStage{
            &hiZCopyCompute.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &hiZDownsampleCompute.reflection,
            RHI::ShaderStage::Compute}};
    m_hiZComputeDescriptorSetLayout =
        buildLayout(hiZStages, {});

    const std::filesystem::path debugPath = shaderDirectory / "DebugView.hlsl";
    const RHI::ShaderBinary& debugVertex = load(
        debugPath, "FullscreenVS", RHI::ShaderStage::Vertex);
    const RHI::ShaderBinary& debugPixel = load(
        debugPath, "ShadowDebugPS", RHI::ShaderStage::Pixel);
    const std::array debugStages = {
        RHI::ShaderLayoutStage{&debugVertex.reflection, RHI::ShaderStage::Vertex},
        RHI::ShaderLayoutStage{&debugPixel.reflection, RHI::ShaderStage::Pixel}};
    m_shadowDebugDescriptorSetLayout = buildLayout(debugStages, {});

    RHI::SamplerDescription samplerDescription{};
    samplerDescription.filter = RHI::Filter::Linear;
    samplerDescription.addressU = RHI::AddressMode::Repeat;
    samplerDescription.addressV = RHI::AddressMode::Repeat;
    samplerDescription.addressW = RHI::AddressMode::Repeat;
    m_sampler = device.CreateSampler(samplerDescription);

    RHI::SamplerDescription clampSamplerDescription = samplerDescription;
    clampSamplerDescription.addressU = RHI::AddressMode::ClampToEdge;
    clampSamplerDescription.addressV = RHI::AddressMode::ClampToEdge;
    clampSamplerDescription.addressW = RHI::AddressMode::ClampToEdge;
    m_linearClampSampler = device.CreateSampler(clampSamplerDescription);

    RHI::SamplerDescription shadowSamplerDescription{};
    shadowSamplerDescription.filter = RHI::Filter::ComparisonLinear;
    shadowSamplerDescription.addressU = RHI::AddressMode::ClampToBorder;
    shadowSamplerDescription.addressV = RHI::AddressMode::ClampToBorder;
    shadowSamplerDescription.addressW = RHI::AddressMode::ClampToBorder;
    shadowSamplerDescription.comparison = RHI::CompareOperation::LessEqual;
    shadowSamplerDescription.maxLod = 0.0f;
    m_shadowSampler = device.CreateSampler(shadowSamplerDescription);

    for (std::shared_ptr<RHI::IBuffer>& frameBuffer : m_frameBuffers)
    {
        RHI::BufferDescription description{};
        description.size = sizeof(FrameConstants);
        description.usage = RHI::BufferUsage::Constant;
        description.memoryAccess = RHI::MemoryAccess::CpuToGpu;
        frameBuffer = device.CreateBuffer(description);
    }

    for (std::shared_ptr<RHI::IBuffer>& shadowPassBuffer : m_shadowPassBuffers)
    {
        RHI::BufferDescription description{};
        description.size = ShadowPassConstantStride * ShadowCascadeCount;
        description.usage = RHI::BufferUsage::Constant;
        description.memoryAccess = RHI::MemoryAccess::CpuToGpu;
        shadowPassBuffer = device.CreateBuffer(description);
    }

    for (std::shared_ptr<RHI::IBuffer>& postProcessBuffer : m_postProcessBuffers)
    {
        RHI::BufferDescription description{};
        description.size = sizeof(PostProcessConstants);
        description.usage = RHI::BufferUsage::Constant;
        description.memoryAccess = RHI::MemoryAccess::CpuToGpu;
        postProcessBuffer = device.CreateBuffer(description);
    }
}

void SceneRenderer::CreatePassDescriptorSets(
    RHI::IGraphicsDevice& device)
{
    const auto createPostProcessSet =
        [&](const std::uint32_t frameIndex,
            const std::shared_ptr<RHI::ITextureView>& source,
            const std::shared_ptr<RHI::ITextureView>& auxiliary)
    {
        std::shared_ptr<RHI::IDescriptorSet> descriptorSet =
            device.CreateDescriptorSet(m_postProcessDescriptorSetLayout);
        descriptorSet->WriteBuffer(0, m_postProcessBuffers[frameIndex]);
        descriptorSet->WriteTextureView(16, source);
        descriptorSet->WriteTextureView(17, auxiliary);
        descriptorSet->WriteTexture(18, m_environmentCubemap->GetRhiTexture());
        descriptorSet->WriteTextureView(
            19, m_skyAtmosphere.GetSkyViewSampledView(
                m_settings.physicalAtmosphereEnabled && m_settings.skyboxEnabled));
        descriptorSet->WriteSampler(48, m_linearClampSampler);
        return descriptorSet;
    };
    const auto createBloomComputeSet =
        [&](const std::uint32_t frameIndex,
            const std::shared_ptr<RHI::ITextureView>& source,
            const std::shared_ptr<RHI::ITextureView>& output)
    {
        std::shared_ptr<RHI::IDescriptorSet> descriptorSet =
            device.CreateDescriptorSet(m_bloomComputeDescriptorSetLayout);
        descriptorSet->WriteBuffer(0, m_postProcessBuffers[frameIndex]);
        descriptorSet->WriteTextureView(16, source);
        descriptorSet->WriteTextureView(32, output);
        descriptorSet->WriteSampler(48, m_linearClampSampler);
        return descriptorSet;
    };

    for (std::uint32_t frameIndex = 0; frameIndex < m_framesInFlight;
        ++frameIndex)
    {
        std::shared_ptr<RHI::IDescriptorSet> deferred =
            device.CreateDescriptorSet(m_deferredDescriptorSetLayout);
        deferred->WriteBuffer(0, m_frameBuffers[frameIndex]);
        deferred->WriteBuffer(
            1,
            m_clusteredLighting
                .GetConstantsBufferShared(
                    frameIndex));
        deferred->WriteBuffer(
            2,
            m_localLightShadows
                .GetLightingConstants(
                    frameIndex));
        for (std::uint32_t gbufferIndex = 0; gbufferIndex < GBufferCount; ++gbufferIndex)
        {
            deferred->WriteTextureView(16u + gbufferIndex, m_gbufferSampledViews[gbufferIndex]);
        }
        deferred->WriteTextureView(20, m_shadowSampledView);
        deferred->WriteTexture(21, m_irradianceCubemap->GetRhiTexture());
        deferred->WriteTexture(22, m_prefilteredSpecularCubemapArray->GetRhiTexture());
        deferred->WriteTexture(23, m_brdfLutTexture->GetRhiTexture());
        deferred->WriteTextureView(
            24,
            m_screenSpaceEffects
                .GetAmbientOcclusionSampledView(
                    m_settings.deferredRenderingEnabled && m_settings.gtaoEnabled));
        deferred->WriteBuffer(
            25,
            m_clusteredLighting
                .GetLightBufferShared(
                    frameIndex));
        deferred->WriteBuffer(
            26,
            m_clusteredLighting
                .GetClusterCountBufferShared(
                    frameIndex));
        deferred->WriteBuffer(
            27,
            m_clusteredLighting
                .GetClusterIndexBufferShared(
                    frameIndex));
        deferred->WriteTextureView(
            28,
            m_localLightShadows
                .GetSpotShadowSampledView());
        deferred->WriteTextureView(
            29,
            m_localLightShadows
                .GetPointShadowSampledView());
        deferred->WriteTextureView(
            30,
            m_varianceShadowMaps.GetMomentsSampledView());
        deferred->WriteTextureView(
            31,
            m_skyAtmosphere.GetSkyViewSampledView(
                m_settings.physicalAtmosphereEnabled && m_settings.skyboxEnabled));
        deferred->WriteSampler(48, m_linearClampSampler);
        deferred->WriteSampler(49, m_shadowSampler);
        m_deferredDescriptorSets[frameIndex] = std::move(deferred);

        m_brightExtractComputeDescriptorSets[frameIndex] =
            createBloomComputeSet(
                frameIndex,
                m_temporalAntiAliasing
                    .GetResolvedSampledView(),
                m_bloomStorageViewA);
        m_blurHorizontalComputeDescriptorSets[frameIndex] =
            createBloomComputeSet(
                frameIndex,
                m_bloomSampledViewA,
                m_bloomStorageViewB);
        m_blurVerticalComputeDescriptorSets[frameIndex] =
            createBloomComputeSet(
                frameIndex,
                m_bloomSampledViewB,
                m_bloomStorageViewA);
        std::shared_ptr<RHI::ITextureView>
            captureSource =
                m_temporalAntiAliasing
                    .GetResolvedSampledView();
        switch (m_captureStage)
        {
        case RenderCaptureStage::Hdr: captureSource = m_hdrSampledView; break;
        case RenderCaptureStage::GBuffer0: captureSource = m_gbufferSampledViews[0]; break;
        case RenderCaptureStage::GBuffer1: captureSource = m_gbufferSampledViews[1]; break;
        case RenderCaptureStage::GBuffer2: captureSource = m_gbufferSampledViews[2]; break;
        case RenderCaptureStage::GBuffer3: captureSource = m_gbufferSampledViews[3]; break;
        case RenderCaptureStage::WaterDepth:
        case RenderCaptureStage::WaterMask:
        case RenderCaptureStage::WaterGBuffer0:
        case RenderCaptureStage::WaterGBuffer1:
        case RenderCaptureStage::WaterGBuffer2:
        case RenderCaptureStage::WaterRefraction:
        case RenderCaptureStage::WaterComposite:
            // Water resources are first allocated at frame build time. The
            // per-frame binding update below selects them without forcing an
            // early allocation or mutating optical history.
            break;
        case RenderCaptureStage::Bloom: captureSource = m_bloomSampledViewA; break;
        default: break;
        }
        m_tonemapDescriptorSets[frameIndex] = createPostProcessSet(
            frameIndex, captureSource, m_bloomSampledViewA);
        m_skyDescriptorSets[frameIndex] = createPostProcessSet(
            frameIndex, m_hdrSampledView, m_hdrSampledView);
        m_shadowDebugDescriptorSets[frameIndex] = device.CreateDescriptorSet(
            m_shadowDebugDescriptorSetLayout);
        m_shadowDebugDescriptorSets[frameIndex]->WriteTextureView(16, m_shadowSampledView);
        m_shadowDebugDescriptorSets[frameIndex]->WriteSampler(48, m_linearClampSampler);
    }
    Core::Check(
        m_depthSampledView != nullptr
            && m_hiZSampledViews.size()
                == m_hiZStorageViews.size(),
        "Hi-Z requires depth input and matching mip views.");
    m_hiZDescriptorSets.clear();
    m_hiZDescriptorSets.reserve(
        m_hiZStorageViews.size());
    for (std::size_t mipIndex = 0;
         mipIndex < m_hiZStorageViews.size();
         ++mipIndex)
    {
        std::shared_ptr<RHI::IDescriptorSet> descriptorSet =
            device.CreateDescriptorSet(
                m_hiZComputeDescriptorSetLayout);
        descriptorSet->WriteTextureView(
            16,
            mipIndex == 0
                ? m_depthSampledView
                : m_hiZSampledViews[mipIndex - 1]);
        descriptorSet->WriteTextureView(
            32,
            m_hiZStorageViews[mipIndex]);
        m_hiZDescriptorSets.push_back(
            std::move(descriptorSet));
    }

    // Cluster list buffers are rebuilt when the render extent changes.
    // Refresh material descriptor sets so Forward+ never retains old buffers.
    std::vector<const SceneMaterialResources*>
        updatedMaterials;
    for (SceneObjectResources& objectResources : m_objectResources)
    {
        SceneMaterialResources& resources =
            *objectResources.materialResources;
        if (std::ranges::find(
                updatedMaterials,
                &resources)
            != updatedMaterials.end())
        {
            continue;
        }
        updatedMaterials.push_back(&resources);
        for (std::uint32_t frameIndex = 0;
             frameIndex < m_framesInFlight;
             ++frameIndex)
        {
            if (resources.descriptorSets[frameIndex] == nullptr)
            {
                continue;
            }
            resources.descriptorSets[frameIndex]->WriteBuffer(
                3,
                m_clusteredLighting.GetConstantsBufferShared(frameIndex));
            resources.descriptorSets[frameIndex]->WriteBuffer(
                26,
                m_clusteredLighting.GetLightBufferShared(frameIndex));
            resources.descriptorSets[frameIndex]->WriteBuffer(
                27,
                m_clusteredLighting.GetClusterCountBufferShared(frameIndex));
            resources.descriptorSets[frameIndex]->WriteBuffer(
                28,
                m_clusteredLighting.GetClusterIndexBufferShared(frameIndex));
        }
    }
}

void SceneRenderer::UpdatePostProcessInputBindings()
{
    const bool screenSpaceCompositeEnabled =
        m_settings.deferredRenderingEnabled
        && (m_settings.screenSpaceReflectionsEnabled
            || m_settings.planarReflectionsEnabled);
    const bool temporalAntiAliasingEnabled =
        m_settings.deferredRenderingEnabled
        && m_settings.temporalAntiAliasingEnabled
        && !m_settings.fluid.enabled;

    std::shared_ptr<RHI::ITexture> currentColor =
        screenSpaceCompositeEnabled
            ? m_screenSpaceEffects.GetCompositeTexture()
            : m_hdrTexture;
    std::shared_ptr<RHI::ITextureView> currentColorView =
        screenSpaceCompositeEnabled
            ? m_screenSpaceEffects.GetCompositeSampledView()
            : m_hdrSampledView;
    if (m_settings.fluid.enabled)
    {
        m_fluidFeature.SetSceneColor(currentColor);
        currentColor =
            m_fluidFeature.GetCompositeTexture();
        currentColorView =
            m_fluidFeature.GetCompositeSampledView();
    }
    if (temporalAntiAliasingEnabled)
    {
        m_temporalAntiAliasing.SetCurrentColor(
            m_activeFrame,
            currentColor);
        currentColor =
            m_temporalAntiAliasing.GetResolvedTexture();
        currentColorView =
            m_temporalAntiAliasing.GetResolvedSampledView();
    }

    Core::Check(
        currentColor != nullptr
            && currentColorView != nullptr,
        "Post-process input selection requires a valid scene color.");
    m_brightExtractComputeDescriptorSets[m_activeFrame]
        ->WriteTextureView(16, currentColorView);

    std::shared_ptr<RHI::ITextureView> captureSource =
        currentColorView;
    // Debug captures must not sample allocated but inactive outputs either.
    // Keep the existing missing-water-output behavior: use current scene color
    // when the requested intermediate has no producer in this view/frame.
    const bool waterCaptureAvailable = m_waterOpticsFeature.HasResources()
        && m_settings.ocean.opticsModel == OceanOpticsModel::HpWater
        && m_settings.ocean.implementation == OceanImplementation::SpectralOcean
        && m_settings.fftOceanEnabled && m_settings.ocean.debug.renderWater;
    const bool captureAvailable =
        (!IsGBufferCaptureStage(m_captureStage) || m_settings.deferredRenderingEnabled)
        && (m_captureStage != RenderCaptureStage::Bloom || m_settings.bloomEnabled)
        && (!IsWaterCaptureStage(m_captureStage) || waterCaptureAvailable);
    switch (captureAvailable ? m_captureStage : RenderCaptureStage::Tonemap)
    {
    case RenderCaptureStage::Hdr:
        captureSource = m_hdrSampledView;
        break;
    case RenderCaptureStage::GBuffer0:
        captureSource = m_gbufferSampledViews[0];
        break;
    case RenderCaptureStage::GBuffer1:
        captureSource = m_gbufferSampledViews[1];
        break;
    case RenderCaptureStage::GBuffer2:
        captureSource = m_gbufferSampledViews[2];
        break;
    case RenderCaptureStage::GBuffer3:
        captureSource = m_gbufferSampledViews[3];
        break;
    case RenderCaptureStage::WaterDepth:
        if (m_waterOpticsFeature.HasResources())
            captureSource = m_waterOpticsFeature.GetCaptureView(
                WaterOpticsCaptureStage::Depth);
        break;
    case RenderCaptureStage::WaterMask:
        if (m_waterOpticsFeature.HasResources())
            captureSource = m_waterOpticsFeature.GetCaptureView(
                WaterOpticsCaptureStage::Mask);
        break;
    case RenderCaptureStage::WaterGBuffer0:
        if (m_waterOpticsFeature.HasResources())
            captureSource = m_waterOpticsFeature.GetCaptureView(
                WaterOpticsCaptureStage::GBuffer0);
        break;
    case RenderCaptureStage::WaterGBuffer1:
        if (m_waterOpticsFeature.HasResources())
            captureSource = m_waterOpticsFeature.GetCaptureView(
                WaterOpticsCaptureStage::GBuffer1);
        break;
    case RenderCaptureStage::WaterGBuffer2:
        if (m_waterOpticsFeature.HasResources())
            captureSource = m_waterOpticsFeature.GetCaptureView(
                WaterOpticsCaptureStage::GBuffer2);
        break;
    case RenderCaptureStage::WaterRefraction:
        if (m_waterOpticsFeature.HasResources())
            captureSource = m_waterOpticsFeature.GetCaptureView(
                WaterOpticsCaptureStage::Refraction);
        break;
    case RenderCaptureStage::WaterComposite:
        if (m_waterOpticsFeature.HasResources())
            captureSource = m_waterOpticsFeature.GetCaptureView(
                WaterOpticsCaptureStage::Composite);
        break;
    case RenderCaptureStage::Bloom:
        captureSource = m_bloomSampledViewA;
        break;
    default:
        break;
    }
    m_tonemapDescriptorSets[m_activeFrame]
        ->WriteTextureView(16, captureSource);
    m_tonemapDescriptorSets[m_activeFrame]->WriteTextureView(
        17, m_settings.bloomEnabled ? m_bloomSampledViewA : currentColorView);
}

void SceneRenderer::UpdateFeatureTextureBindings()
{
    const bool oceanEnabled = m_settings.fftOceanEnabled && m_settings.ocean.debug.renderWater;
    const bool spectralOceanEnabled = oceanEnabled
        && m_settings.ocean.implementation == OceanImplementation::SpectralOcean;
    const bool localWaveEnabled = spectralOceanEnabled && m_settings.ocean.local.enabled
        && m_localWaveGpuResources->IsInitialized();
    m_deferredDescriptorSets[m_activeFrame]->WriteTextureView(
        24, m_screenSpaceEffects.GetAmbientOcclusionSampledView(
            m_settings.deferredRenderingEnabled && m_settings.gtaoEnabled));
    // Match the producer predicate, including the skybox switch. Only rewrite
    // the safe frame slot; the other slots may still be used by the GPU.
    const auto skyView = m_skyAtmosphere.GetSkyViewSampledView(
        m_settings.physicalAtmosphereEnabled && m_settings.skyboxEnabled);
    m_deferredDescriptorSets[m_activeFrame]->WriteTextureView(31, skyView);
    m_tonemapDescriptorSets[m_activeFrame]->WriteTextureView(19, skyView);
    m_skyDescriptorSets[m_activeFrame]->WriteTextureView(19, skyView);
    std::vector<const SceneMaterialResources*> updatedMaterials;
    for (SceneObjectResources& objectResources : m_objectResources)
    {
        SceneMaterialResources& resources = *objectResources.materialResources;
        if (std::ranges::find(updatedMaterials, &resources) !=
            updatedMaterials.end())
        {
            continue;
        }
        updatedMaterials.push_back(&resources);
        std::shared_ptr<RHI::IDescriptorSet>& descriptorSet =
            resources.descriptorSets[m_activeFrame];
        if (descriptorSet == nullptr)
        {
            continue;
        }
        descriptorSet->WriteBuffer(
            4, m_virtualTextureCache.GetPageTableBuffer());
        descriptorSet->WriteTextureView(34, skyView);
        descriptorSet->WriteTexture(30,
            oceanEnabled && m_settings.ocean.implementation == OceanImplementation::LegacyFft
                ? m_fftOcean.GetDisplacementTexture()
                : m_virtualTextureCache.GetAtlasTexture());
        descriptorSet->WriteTexture(31,
            oceanEnabled && m_settings.ocean.implementation == OceanImplementation::LegacyFft
                ? m_fftOcean.GetNormalFoamTexture() : m_virtualTextureCache.GetAtlasTexture());
        descriptorSet->WriteTexture(
            35, m_settings.interactiveTerrainEnabled
                    && m_sharedResources
                        ->IsInteractiveTerrainInitialized()
                ? m_sharedResources->GetInteractiveTerrain().GetHeightTexture()
                : m_virtualTextureCache.GetAtlasTexture());
        descriptorSet->WriteTextureView(
            36, spectralOceanEnabled
                ? m_spectralOcean->DisplacementMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().ZeroArray());
        descriptorSet->WriteTextureView(
            37, spectralOceanEnabled
                ? m_spectralOcean->GradientMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().UpNormalArray());
        descriptorSet->WriteTextureView(
            38, spectralOceanEnabled
                ? m_spectralOcean->SlopeMomentMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().ZeroArray());
        descriptorSet->WriteTextureView(
            39, spectralOceanEnabled
                ? m_spectralOcean->FoamMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().ZeroArray());
        descriptorSet->WriteTextureView(40,
            localWaveEnabled
                ? m_localWaveGpuResources->Displacement().sampled
                : m_virtualTextureCache.GetAtlasSampledView());
        descriptorSet->WriteTextureView(41,
            localWaveEnabled
                ? m_localWaveGpuResources->Gradient().sampled
                : m_virtualTextureCache.GetAtlasSampledView());
        std::shared_ptr<RHI::IDescriptorSet>& oceanDescriptorSet =
            resources.oceanDescriptorSets[m_activeFrame];
        if (oceanDescriptorSet == nullptr)
            continue;
        oceanDescriptorSet->WriteTextureView(34, skyView);
        m_waterOpticsFeature.UpdateSurfaceDescriptorSet(
            *oceanDescriptorSet, m_activeFrame);
        oceanDescriptorSet->WriteTextureView(
            36, spectralOceanEnabled
                ? m_spectralOcean->DisplacementMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().ZeroArray());
        oceanDescriptorSet->WriteTextureView(
            37, spectralOceanEnabled
                ? m_spectralOcean->GradientMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().UpNormalArray());
        oceanDescriptorSet->WriteTextureView(
            38, spectralOceanEnabled
                ? m_spectralOcean->SlopeMomentMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().ZeroArray());
        oceanDescriptorSet->WriteTextureView(
            39, spectralOceanEnabled
                ? m_spectralOcean->FoamMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().ZeroArray());
        oceanDescriptorSet->WriteTextureView(40,
            localWaveEnabled
                ? m_localWaveGpuResources->Displacement().sampled
                : m_virtualTextureCache.GetAtlasSampledView());
        oceanDescriptorSet->WriteTextureView(41,
            localWaveEnabled
                ? m_localWaveGpuResources->Gradient().sampled
                : m_virtualTextureCache.GetAtlasSampledView());
        oceanDescriptorSet->WriteBuffer(42,
            m_oceanGeometryInstanceBuffer != nullptr
                ? m_oceanGeometryInstanceBuffer
                : m_oceanGeometryInstanceFallbackBuffer);
    }
}

void SceneRenderer::CreateSceneResources(
    RHI::IGraphicsDevice& device, const Scene::RenderSceneView& scene)
{
    const std::vector<Scene::RenderObject>& objects = scene.GetRenderObjects();
    static_assert(sizeof(ObjectConstants) <= ObjectConstantStride);

    if (m_oceanGeometryInstanceFallbackBuffer == nullptr)
    {
        OceanPatchInstanceData fallback{};
        RHI::BufferDescription description{};
        description.size = sizeof(fallback);
        description.stride = sizeof(fallback);
        description.usage = RHI::BufferUsage::ShaderResource;
        description.memoryAccess = RHI::MemoryAccess::CpuToGpu;
        m_oceanGeometryInstanceFallbackBuffer =
            device.CreateBuffer(description, &fallback);
    }

    RHI::BufferDescription objectDescription{};
    objectDescription.size =
        ObjectConstantStride * std::max<std::size_t>(objects.size(), 1u);
    objectDescription.stride = static_cast<std::uint32_t>(ObjectConstantStride);
    objectDescription.usage =
        RHI::BufferUsage::Constant | RHI::BufferUsage::ShaderResource;
    objectDescription.memoryAccess = RHI::MemoryAccess::CpuToGpu;
    for (std::shared_ptr<RHI::IBuffer>& objectBuffer : m_objectBuffers)
    {
        objectBuffer = device.CreateBuffer(objectDescription);
    }

    RHI::BufferDescription indexedObjectDescription{};
    indexedObjectDescription.size =
        sizeof(ObjectConstants) * std::max<std::size_t>(objects.size(), 1u);
    indexedObjectDescription.stride =
        static_cast<std::uint32_t>(sizeof(ObjectConstants));
    indexedObjectDescription.usage = RHI::BufferUsage::ShaderResource;
    indexedObjectDescription.memoryAccess = RHI::MemoryAccess::CpuToGpu;
    for (std::shared_ptr<RHI::IBuffer>& indexedObjectBuffer :
        m_indexedObjectBuffers)
    {
        indexedObjectBuffer = device.CreateBuffer(indexedObjectDescription);
    }

    RHI::BufferDescription instanceIndexDescription{};
    instanceIndexDescription.size =
        sizeof(std::uint32_t) * std::max<std::size_t>(objects.size() * 2u, 1u);
    instanceIndexDescription.stride = sizeof(std::uint32_t);
    instanceIndexDescription.usage = RHI::BufferUsage::ShaderResource;
    instanceIndexDescription.memoryAccess = RHI::MemoryAccess::CpuToGpu;
    for (std::shared_ptr<RHI::IBuffer>& buffer : m_instanceObjectIndexBuffers)
    {
        buffer = device.CreateBuffer(instanceIndexDescription);
    }

    m_objectResources.clear();
    m_objectResources.resize(objects.size());
    m_sceneObjectBindings.clear();
    m_sceneObjectBindings.reserve(objects.size());
    for (std::size_t objectIndex = 0; objectIndex < objects.size();
        ++objectIndex)
    {
        const Scene::RenderObject& object = objects[objectIndex];
        const Asset::Mesh* const mesh = scene.GetMesh(objectIndex);
        const Scene::RenderSceneMaterialBinding* const material =
            scene.FindMaterialBinding(objectIndex);
        Core::Check(mesh != nullptr
                && (material != nullptr || object.material != nullptr),
            "Shared RHI scene objects require runtime mesh and material "
            "resources.");
        SceneObjectResources& objectResources = m_objectResources[objectIndex];
        const std::uint64_t materialIdentity = material != nullptr
            ? material->revision.value
            : static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                  object.material.get()));
        const SceneObjectBinding binding{mesh,
            materialIdentity,
            ResolveMaterialOverrideSignature(object)};
        m_sceneObjectBindings.push_back(binding);
        for (std::size_t previousIndex = 0; previousIndex < objectIndex;
            ++previousIndex)
        {
            const SceneObjectBinding& previous =
                m_sceneObjectBindings[previousIndex];
            if (previous.materialIdentity == binding.materialIdentity &&
                previous.materialOverrideSignature ==
                    binding.materialOverrideSignature)
            {
                objectResources.materialResources =
                    m_objectResources[previousIndex].materialResources;
                break;
            }
        }
        if (objectResources.materialResources == nullptr)
        {
            objectResources.materialResources =
                CreateMaterialResources(device, scene, objectIndex);
        }
    }
}

std::shared_ptr<SceneRenderer::SceneMaterialResources>
SceneRenderer::CreateMaterialResources(
    RHI::IGraphicsDevice& device,
    const Scene::RenderSceneView& scene,
    const std::size_t objectIndex)
{
    const bool oceanEnabled = m_settings.fftOceanEnabled && m_settings.ocean.debug.renderWater;
    const bool spectralOceanEnabled = oceanEnabled
        && m_settings.ocean.implementation == OceanImplementation::SpectralOcean;
    const bool localWaveEnabled = spectralOceanEnabled && m_settings.ocean.local.enabled
        && m_localWaveGpuResources->IsInitialized();
    auto resources = std::make_shared<SceneMaterialResources>();
    resources->descriptorSets.resize(m_framesInFlight);
    resources->oceanDescriptorSets.resize(m_framesInFlight);
    resources->shadowDescriptorSets.resize(m_framesInFlight);
    const MaterialConstants materialConstants =
        ResolveMaterialConstants(scene, objectIndex);
    RHI::BufferDescription materialDescription{};
    materialDescription.size = sizeof(MaterialConstants);
    materialDescription.usage = RHI::BufferUsage::Constant;
    materialDescription.memoryAccess = RHI::MemoryAccess::CpuToGpu;
    resources->materialBuffer =
        device.CreateBuffer(materialDescription, &materialConstants);

    const Asset::Texture* const albedo = scene.GetMaterialTexture(
        objectIndex, Scene::MaterialTextureSlot::Albedo);
    const Asset::Texture* const metallicRoughness = scene.GetMaterialTexture(
        objectIndex, Scene::MaterialTextureSlot::MetallicRoughness);
    const Asset::Texture* const normal = scene.GetMaterialTexture(
        objectIndex, Scene::MaterialTextureSlot::Normal);
    const Asset::Texture* const occlusion = scene.GetMaterialTexture(
        objectIndex, Scene::MaterialTextureSlot::Occlusion);
    const Asset::Texture* const emissive = scene.GetMaterialTexture(
        objectIndex, Scene::MaterialTextureSlot::Emissive);
    Core::Check(albedo != nullptr && metallicRoughness != nullptr
                    && normal != nullptr && occlusion != nullptr
                    && emissive != nullptr,
        "Shared RHI materials require all five PBR textures.");
    for (std::uint32_t frameIndex = 0; frameIndex < m_framesInFlight;
        ++frameIndex)
    {
        std::shared_ptr<RHI::IDescriptorSet> descriptorSet =
            device.CreateDescriptorSet(m_descriptorSetLayout);
        descriptorSet->WriteBuffer(0, m_frameBuffers[frameIndex]);
        descriptorSet->WriteBuffer(
            1, m_objectBuffers[frameIndex], 0, sizeof(ObjectConstants));
        descriptorSet->WriteBuffer(2, resources->materialBuffer);
        descriptorSet->WriteBuffer(
            3, m_clusteredLighting.GetConstantsBufferShared(frameIndex));
        descriptorSet->WriteBuffer(
            4, m_virtualTextureCache.GetPageTableBuffer());
        descriptorSet->WriteTexture(16, albedo->GetRhiTexture());
        descriptorSet->WriteTexture(17, metallicRoughness->GetRhiTexture());
        descriptorSet->WriteTexture(18, normal->GetRhiTexture());
        descriptorSet->WriteTexture(19, occlusion->GetRhiTexture());
        descriptorSet->WriteTexture(20, emissive->GetRhiTexture());
        descriptorSet->WriteTextureView(21, m_shadowSampledView);
        descriptorSet->WriteTexture(22, m_environmentCubemap->GetRhiTexture());
        descriptorSet->WriteTexture(23, m_irradianceCubemap->GetRhiTexture());
        descriptorSet->WriteTexture(
            24, m_prefilteredSpecularCubemapArray->GetRhiTexture());
        descriptorSet->WriteTexture(25, m_brdfLutTexture->GetRhiTexture());
        descriptorSet->WriteBuffer(
            26, m_clusteredLighting.GetLightBufferShared(frameIndex));
        descriptorSet->WriteBuffer(
            27, m_clusteredLighting.GetClusterCountBufferShared(frameIndex));
        descriptorSet->WriteBuffer(
            28, m_clusteredLighting.GetClusterIndexBufferShared(frameIndex));
        descriptorSet->WriteTextureView(
            29, m_varianceShadowMaps.GetMomentsSampledView());
        descriptorSet->WriteTexture(30,
            oceanEnabled && m_settings.ocean.implementation == OceanImplementation::LegacyFft
                ? m_fftOcean.GetDisplacementTexture()
                : m_virtualTextureCache.GetAtlasTexture());
        descriptorSet->WriteTexture(31,
            oceanEnabled && m_settings.ocean.implementation == OceanImplementation::LegacyFft
                ? m_fftOcean.GetNormalFoamTexture() : m_virtualTextureCache.GetAtlasTexture());
        descriptorSet->WriteBuffer(32, m_indexedObjectBuffers[frameIndex]);
        descriptorSet->WriteBuffer(
            33, m_instanceObjectIndexBuffers[frameIndex]);
        descriptorSet->WriteTextureView(
            34, m_skyAtmosphere.GetSkyViewSampledView(
                m_settings.physicalAtmosphereEnabled && m_settings.skyboxEnabled));
        descriptorSet->WriteTexture(
            35, m_settings.interactiveTerrainEnabled
                    && m_sharedResources
                        ->IsInteractiveTerrainInitialized()
                ? m_sharedResources->GetInteractiveTerrain().GetHeightTexture()
                : m_virtualTextureCache.GetAtlasTexture());
        descriptorSet->WriteTextureView(
            36, spectralOceanEnabled
                ? m_spectralOcean->DisplacementMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().ZeroArray());
        descriptorSet->WriteTextureView(
            37, spectralOceanEnabled
                ? m_spectralOcean->GradientMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().UpNormalArray());
        descriptorSet->WriteTextureView(
            38, spectralOceanEnabled
                ? m_spectralOcean->SlopeMomentMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().ZeroArray());
        descriptorSet->WriteTextureView(
            39, spectralOceanEnabled
                ? m_spectralOcean->FoamMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().ZeroArray());
        descriptorSet->WriteTextureView(40,
            localWaveEnabled
                ? m_localWaveGpuResources->Displacement().sampled
                : m_virtualTextureCache.GetAtlasSampledView());
        descriptorSet->WriteTextureView(41,
            localWaveEnabled
                ? m_localWaveGpuResources->Gradient().sampled
                : m_virtualTextureCache.GetAtlasSampledView());
        descriptorSet->WriteSampler(48, m_sampler);
        descriptorSet->WriteSampler(49, m_shadowSampler);
        descriptorSet->WriteSampler(50, m_linearClampSampler);
        resources->descriptorSets[frameIndex] = std::move(descriptorSet);

        const auto hasOceanBinding = [this](const std::uint32_t binding)
        {
            const auto& bindings =
                m_oceanDescriptorSetLayout->GetDescription().bindings;
            return std::ranges::any_of(bindings,
                [binding](const RHI::DescriptorBindingDescription& candidate)
                {
                    return candidate.binding == binding;
                });
        };
        std::shared_ptr<RHI::IDescriptorSet> oceanDescriptorSet =
            device.CreateDescriptorSet(m_oceanDescriptorSetLayout);
        if (hasOceanBinding(0u))
            oceanDescriptorSet->WriteBuffer(0, m_frameBuffers[frameIndex]);
        if (hasOceanBinding(1u))
            oceanDescriptorSet->WriteBuffer(
                1, m_objectBuffers[frameIndex], 0, sizeof(ObjectConstants));
        if (hasOceanBinding(2u))
            oceanDescriptorSet->WriteBuffer(2, resources->materialBuffer);
        if (hasOceanBinding(3u))
            oceanDescriptorSet->WriteBuffer(
                3, m_clusteredLighting.GetConstantsBufferShared(frameIndex));
        if (hasOceanBinding(4u))
            oceanDescriptorSet->WriteBuffer(
                4, m_virtualTextureCache.GetPageTableBuffer());
        if (hasOceanBinding(5u))
            m_waterOpticsFeature.UpdateSurfaceDescriptorSet(
                *oceanDescriptorSet, frameIndex);
        if (hasOceanBinding(16u))
            oceanDescriptorSet->WriteTexture(16, albedo->GetRhiTexture());
        if (hasOceanBinding(17u))
            oceanDescriptorSet->WriteTexture(
                17, metallicRoughness->GetRhiTexture());
        if (hasOceanBinding(18u))
            oceanDescriptorSet->WriteTexture(18, normal->GetRhiTexture());
        if (hasOceanBinding(19u))
            oceanDescriptorSet->WriteTexture(19, occlusion->GetRhiTexture());
        if (hasOceanBinding(20u))
            oceanDescriptorSet->WriteTexture(20, emissive->GetRhiTexture());
        if (hasOceanBinding(21u))
            oceanDescriptorSet->WriteTextureView(21, m_shadowSampledView);
        if (hasOceanBinding(22u))
            oceanDescriptorSet->WriteTexture(
                22, m_environmentCubemap->GetRhiTexture());
        if (hasOceanBinding(23u))
            oceanDescriptorSet->WriteTexture(
                23, m_irradianceCubemap->GetRhiTexture());
        if (hasOceanBinding(24u))
            oceanDescriptorSet->WriteTexture(
                24, m_prefilteredSpecularCubemapArray->GetRhiTexture());
        if (hasOceanBinding(25u))
            oceanDescriptorSet->WriteTexture(
                25, m_brdfLutTexture->GetRhiTexture());
        if (hasOceanBinding(26u))
            oceanDescriptorSet->WriteBuffer(
                26, m_clusteredLighting.GetLightBufferShared(frameIndex));
        if (hasOceanBinding(27u))
            oceanDescriptorSet->WriteBuffer(
                27, m_clusteredLighting.GetClusterCountBufferShared(frameIndex));
        if (hasOceanBinding(28u))
            oceanDescriptorSet->WriteBuffer(
                28, m_clusteredLighting.GetClusterIndexBufferShared(frameIndex));
        if (hasOceanBinding(29u))
            oceanDescriptorSet->WriteTextureView(
                29, m_varianceShadowMaps.GetMomentsSampledView());
        if (hasOceanBinding(30u))
            oceanDescriptorSet->WriteTexture(30,
                oceanEnabled && m_settings.ocean.implementation == OceanImplementation::LegacyFft
                    ? m_fftOcean.GetDisplacementTexture()
                    : m_virtualTextureCache.GetAtlasTexture());
        if (hasOceanBinding(31u))
            oceanDescriptorSet->WriteTexture(
                31, oceanEnabled && m_settings.ocean.implementation == OceanImplementation::LegacyFft
                    ? m_fftOcean.GetNormalFoamTexture() : m_virtualTextureCache.GetAtlasTexture());
        if (hasOceanBinding(32u))
            oceanDescriptorSet->WriteBuffer(
                32, m_indexedObjectBuffers[frameIndex]);
        if (hasOceanBinding(33u))
            oceanDescriptorSet->WriteBuffer(
                33, m_instanceObjectIndexBuffers[frameIndex]);
        if (hasOceanBinding(34u))
            oceanDescriptorSet->WriteTextureView(
                34, m_skyAtmosphere.GetSkyViewSampledView(
                    m_settings.physicalAtmosphereEnabled && m_settings.skyboxEnabled));
        if (hasOceanBinding(35u))
            oceanDescriptorSet->WriteTexture(35,
                m_sharedResources->IsInteractiveTerrainInitialized()
                    ? m_sharedResources->GetInteractiveTerrain()
                          .GetHeightTexture()
                    : m_virtualTextureCache.GetAtlasTexture());
        if (hasOceanBinding(36u))
            oceanDescriptorSet->WriteTextureView(
                36, spectralOceanEnabled
                ? m_spectralOcean->DisplacementMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().ZeroArray());
        if (hasOceanBinding(37u))
            oceanDescriptorSet->WriteTextureView(
                37, spectralOceanEnabled
                ? m_spectralOcean->GradientMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().UpNormalArray());
        if (hasOceanBinding(38u))
            oceanDescriptorSet->WriteTextureView(
                38, spectralOceanEnabled
                ? m_spectralOcean->SlopeMomentMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().ZeroArray());
        if (hasOceanBinding(39u))
            oceanDescriptorSet->WriteTextureView(
                39, spectralOceanEnabled
                ? m_spectralOcean->FoamMap().sampledArray
                : m_sharedResources->GetOceanFallbackResources().ZeroArray());
        if (hasOceanBinding(40u))
            oceanDescriptorSet->WriteTextureView(40,
                localWaveEnabled
                    ? m_localWaveGpuResources->Displacement().sampled
                    : m_virtualTextureCache.GetAtlasSampledView());
        if (hasOceanBinding(41u))
            oceanDescriptorSet->WriteTextureView(41,
                localWaveEnabled
                    ? m_localWaveGpuResources->Gradient().sampled
                    : m_virtualTextureCache.GetAtlasSampledView());
        if (hasOceanBinding(42u))
            oceanDescriptorSet->WriteBuffer(42,
                m_oceanGeometryInstanceBuffer != nullptr
                    ? m_oceanGeometryInstanceBuffer
                    : m_oceanGeometryInstanceFallbackBuffer);
        if (hasOceanBinding(48u))
            oceanDescriptorSet->WriteSampler(48, m_sampler);
        if (hasOceanBinding(49u))
            oceanDescriptorSet->WriteSampler(49, m_shadowSampler);
        if (hasOceanBinding(50u))
            oceanDescriptorSet->WriteSampler(50, m_linearClampSampler);
        resources->oceanDescriptorSets[frameIndex] =
            std::move(oceanDescriptorSet);

        std::shared_ptr<RHI::IDescriptorSet> shadowDescriptorSet =
            device.CreateDescriptorSet(m_shadowDescriptorSetLayout);
        shadowDescriptorSet->WriteBuffer(0, m_frameBuffers[frameIndex]);
        shadowDescriptorSet->WriteBuffer(
            1, m_objectBuffers[frameIndex], 0, sizeof(ObjectConstants));
        shadowDescriptorSet->WriteBuffer(2, resources->materialBuffer);
        shadowDescriptorSet->WriteBuffer(
            3, m_shadowPassBuffers[frameIndex], 0, sizeof(ShadowPassConstants));
        shadowDescriptorSet->WriteTexture(16, albedo->GetRhiTexture());
        shadowDescriptorSet->WriteTexture(
            17, m_sharedResources->IsInteractiveTerrainInitialized()
                ? m_sharedResources->GetInteractiveTerrain()
                      .GetHeightTexture()
                : m_virtualTextureCache.GetAtlasTexture());
        shadowDescriptorSet->WriteSampler(48, m_sampler);
        shadowDescriptorSet->WriteSampler(49, m_linearClampSampler);
        resources->shadowDescriptorSets[frameIndex] =
            std::move(shadowDescriptorSet);
    }
    return resources;
}

void SceneRenderer::EnsureSceneResources(
    RHI::IGraphicsDevice& device, const Scene::RenderSceneView& scene)
{
    const std::vector<Scene::RenderObject>& objects = scene.GetRenderObjects();
    const bool layoutChanged = objects.size() != m_sceneObjectBindings.size();
    if (layoutChanged)
    {
        CreateSceneResources(device, scene);
        return;
    }
    for (std::size_t index = 0; index < objects.size(); ++index)
    {
        const Scene::RenderObject& object = objects[index];
        SceneObjectBinding& binding = m_sceneObjectBindings[index];
        const Scene::RenderSceneMaterialBinding* const material =
            scene.FindMaterialBinding(index);
        const std::uint64_t materialIdentity = material != nullptr
            ? material->revision.value
            : static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                  object.material.get()));
        const std::uint64_t materialOverrideSignature =
            ResolveMaterialOverrideSignature(object);
        const bool materialChanged =
            binding.materialIdentity != materialIdentity ||
            binding.materialOverrideSignature != materialOverrideSignature;
        if (materialChanged)
        {
            std::shared_ptr<SceneMaterialResources> sharedResources;
            for (std::size_t candidate = 0; candidate < objects.size();
                ++candidate)
            {
                if (candidate == index)
                {
                    continue;
                }
                const SceneObjectBinding& other =
                    m_sceneObjectBindings[candidate];
                if (other.materialIdentity == materialIdentity &&
                    other.materialOverrideSignature ==
                        materialOverrideSignature)
                {
                    sharedResources =
                        m_objectResources[candidate].materialResources;
                    break;
                }
            }
            m_objectResources[index].materialResources =
                sharedResources != nullptr
                    ? std::move(sharedResources)
                    : CreateMaterialResources(device, scene, index);
        }
        binding = {scene.GetMesh(index),
            materialIdentity,
            materialOverrideSignature};
    }
}

void SceneRenderer::UpdateConstants(RHI::IFrameContext& frame,
    const Scene::RenderSceneView& scene,
    const double simulationTimeSeconds)
{
    const RenderExtent extent{frame.GetFrameWidth(), frame.GetFrameHeight()};
    // Projection jitter belongs to the render frame, not to the mutable world
    // camera. Keeping a local copy makes the published RenderScene immutable.
    Scene::Camera camera = scene.GetCamera();
    camera.SetAspectRatio(
        static_cast<float>(extent.width) / static_cast<float>(extent.height));
    camera.SetProjectionJitterNdc(
        m_settings
                .temporalAntiAliasingEnabled
            && m_settings
                .deferredRenderingEnabled
            && !m_settings.fluid.enabled
        ? m_temporalAntiAliasing
              .CalculateJitterNdc(
                  m_temporalSampleIndex)
        : XMFLOAT2{});
    const Scene::DirectionalLight& light = scene.GetDirectionalLight();
    const CascadeShadowData shadowData = BuildCascadeShadowData(
        scene, m_settings, ShadowMapResolution);
    FrameConstants frameConstants{};
    const bool relativeToEye =
        m_settings.relativeToEyeEnabled;
    const Core::Double3 renderOrigin =
        camera.GetWorldPosition();
    const XMFLOAT3 cameraPosition = relativeToEye
        ? XMFLOAT3{}
        : camera.GetPosition();
    const XMMATRIX viewProjection = relativeToEye
        ? camera.GetRelativeViewProjectionMatrix()
        : camera.GetViewProjectionMatrix();
    XMFLOAT4X4 waterInverseViewProjection{};
    XMStoreFloat4x4(&waterInverseViewProjection,
        XMMatrixTranspose(XMMatrixInverse(nullptr, viewProjection)));
    m_waterOpticsFeature.SetInverseViewProjection(
        m_activeFrame, waterInverseViewProjection);
    frameConstants.cameraPosition = cameraPosition;
    const std::uint32_t legacyPointLightCount =
        std::min<std::uint32_t>(
            scene.GetActivePointLightCount(),
            SharedMaxPointLightCount);
    frameConstants.pointLightCount =
        static_cast<float>(
            legacyPointLightCount);
    frameConstants.directionalLightDirection = light.direction;
    frameConstants.directionalLightIntensity = light.intensity;
    frameConstants.directionalLightColor = light.color;
    const auto& auxiliaryDirectionalLights =
        scene.GetAuxiliaryDirectionalLights();
    for (std::uint32_t index = 0u;
         index < SharedAuxiliaryDirectionalLightCount; ++index)
    {
        frameConstants.auxiliaryDirectionalLights[index].direction =
            auxiliaryDirectionalLights[index].direction;
        frameConstants.auxiliaryDirectionalLights[index].intensity =
            std::max(auxiliaryDirectionalLights[index].intensity, 0.0f);
        frameConstants.auxiliaryDirectionalLights[index].color =
            auxiliaryDirectionalLights[index].color;
    }
    frameConstants.ambientIntensity = m_settings.ambientIntensity;
    for (std::uint32_t cascadeIndex = 0; cascadeIndex < ShadowCascadeCount; ++cascadeIndex)
    {
        XMStoreFloat4x4(
            &frameConstants.lightViewProjections[cascadeIndex],
            XMMatrixTranspose(shadowData.viewProjections[cascadeIndex]));
    }
    frameConstants.cascadeSplits = shadowData.splitDistances;
    frameConstants.skyZenithColor = m_settings.skyZenithColor;
    frameConstants.pbrEnabled = m_settings.pbrEnabled ? 1.0f : 0.0f;
    frameConstants.skyHorizonColor = m_settings.skyHorizonColor;
    frameConstants.iblEnabled = m_settings.iblEnabled ? 1.0f : 0.0f;
    frameConstants.groundColor = m_settings.groundColor;
    frameConstants.iblIntensity = m_settings.iblIntensity;
    frameConstants.iblDiffuseStrength = m_settings.iblDiffuseStrength;
    frameConstants.iblSpecularStrength = m_settings.iblSpecularStrength;
    frameConstants.iblReflectionBlend = m_settings.iblReflectionBlend;
    frameConstants.iblHorizonSharpness = m_settings.iblHorizonSharpness;
    frameConstants.iblSplitSumEnabled = m_settings.iblSplitSumEnabled ? 1.0f : 0.0f;
    frameConstants.shadowBias = m_settings.shadowBias;
    frameConstants.shadowsEnabled = m_settings.shadowsEnabled ? 1.0f : 0.0f;
    frameConstants.gtaoEnabled =
        m_settings.deferredRenderingEnabled
            && m_settings.gtaoEnabled
        ? 1.0f
        : 0.0f;
    frameConstants.directLightingEnabled = m_settings.directLightingEnabled ? 1.0f : 0.0f;
    frameConstants.pointLightsEnabled = m_settings.pointLightsEnabled ? 1.0f : 0.0f;
    frameConstants.normalMappingEnabled = m_settings.normalMappingEnabled ? 1.0f : 0.0f;
    frameConstants.occlusionEnabled = m_settings.occlusionEnabled ? 1.0f : 0.0f;
    frameConstants.emissiveEnabled = m_settings.emissiveEnabled ? 1.0f : 0.0f;
    frameConstants.alphaMaskEnabled = m_settings.alphaMaskEnabled ? 1.0f : 0.0f;
    frameConstants.clusteredLightingEnabled =
        m_settings.clusteredLightingEnabled
            && (m_settings.deferredRenderingEnabled
                || m_settings.forwardPlusEnabled)
        ? 1.0f
        : 0.0f;
    frameConstants.shadowFilterMode = static_cast<float>(
        static_cast<std::uint32_t>(m_settings.shadowFilterMode)
        + (m_settings.cascadeDebugEnabled ? 16u : 0u));
    frameConstants.cameraForward = camera.GetForwardVector();
    frameConstants.cascadeBlendFraction =
        m_settings.cascadeBlendFraction;
    const DirectX::XMFLOAT3 waterColorIntensity{
        m_settings.ocean.shading.waterColorIntensity.x,
        m_settings.ocean.shading.waterColorIntensity.y,
        m_settings.ocean.shading.waterColorIntensity.z};
    frameConstants.oceanDeepWaterColor = {
        m_settings.ocean.shading.deepWaterColor.x * waterColorIntensity.x,
        m_settings.ocean.shading.deepWaterColor.y * waterColorIntensity.y,
        m_settings.ocean.shading.deepWaterColor.z * waterColorIntensity.z};
    frameConstants.physicalAtmosphereEnabled =
        m_settings.physicalAtmosphereEnabled
            && m_settings.skyboxEnabled
        ? 1.0f
        : 0.0f;
    frameConstants.oceanScatteringColor = {
        m_settings.ocean.shading.scatteringColor.x
            * m_settings.ocean.shading.waterColorIntensity.w,
        m_settings.ocean.shading.scatteringColor.y
            * m_settings.ocean.shading.waterColorIntensity.w,
        m_settings.ocean.shading.scatteringColor.z
            * m_settings.ocean.shading.waterColorIntensity.w};
    frameConstants.atmosphereBrightness =
        m_settings.atmosphereBrightness;
    frameConstants.oceanFoamColor =
        m_settings.ocean.shading.foamColor;
    frameConstants.oceanScatteringStrength =
        m_settings.fftOceanEnabled ? 1.0f : 0.0f;
    if (m_spectralOcean != nullptr)
    {
        // Stage CPU cascade metadata before filling the same frame's surface
        // constants. Update() later in this method records the matching GPU
        // H0/evolution work and commits the complete version at EndFrame.
        (void)m_spectralOcean->ConfigureSpectrum(m_settings.ocean);
        const auto& cascades =
            m_spectralOcean->SpectrumGenerator().GetCascades();
        frameConstants.oceanCascadePatchLengths = {
            cascades[0].patchLengthMeters,
            cascades[1].patchLengthMeters,
            cascades[2].patchLengthMeters,
            cascades[3].patchLengthMeters};
        frameConstants.oceanCascadeLowerWavelengths = {
            cascades[0].wavelengthMinMeters,
            cascades[1].wavelengthMinMeters,
            cascades[2].wavelengthMinMeters,
            cascades[3].wavelengthMinMeters};
        frameConstants.oceanCascadeUpperWavelengths = {
            cascades[0].wavelengthMaxMeters,
            cascades[1].wavelengthMaxMeters,
            cascades[2].wavelengthMaxMeters,
            cascades[3].wavelengthMaxMeters};
        frameConstants.oceanCascadeUvScales = {
            1.0f / std::max(cascades[0].patchLengthMeters, 1.0f),
            1.0f / std::max(cascades[1].patchLengthMeters, 1.0f),
            1.0f / std::max(cascades[2].patchLengthMeters, 1.0f),
            1.0f / std::max(cascades[3].patchLengthMeters, 1.0f)};
        frameConstants.oceanCascadeFadeEnds = {
            cascades[0].patchLengthMeters
                * OceanCascadeVisibilityPeriods,
            cascades[1].patchLengthMeters
                * OceanCascadeVisibilityPeriods,
            cascades[2].patchLengthMeters
                * OceanCascadeVisibilityPeriods,
            0.0f};
    }
    for (std::uint32_t lightIndex = 0;
         lightIndex < legacyPointLightCount;
         ++lightIndex)
    {
        const Scene::PointLight& sourceLight = scene.GetPointLights()[lightIndex];
        frameConstants.pointLights[lightIndex].position = relativeToEye
            ? XMFLOAT3{
                static_cast<float>(
                    static_cast<double>(sourceLight.position.x)
                    - renderOrigin.x),
                static_cast<float>(
                    static_cast<double>(sourceLight.position.y)
                    - renderOrigin.y),
                static_cast<float>(
                    static_cast<double>(sourceLight.position.z)
                    - renderOrigin.z)}
            : sourceLight.position;
        frameConstants.pointLights[lightIndex].range = sourceLight.range;
        frameConstants.pointLights[lightIndex].color = sourceLight.color;
        frameConstants.pointLights[lightIndex].intensity = sourceLight.intensity;
    }
    m_frameBuffers[m_activeFrame]->Update(&frameConstants, sizeof(frameConstants));
    for (std::uint32_t cascadeIndex = 0; cascadeIndex < ShadowCascadeCount; ++cascadeIndex)
    {
        ShadowPassConstants shadowPassConstants{};
        shadowPassConstants.cascadeIndex = cascadeIndex;
        m_shadowPassBuffers[m_activeFrame]->Update(
            &shadowPassConstants,
            sizeof(shadowPassConstants),
            cascadeIndex * ShadowPassConstantStride);
    }

    PostProcessConstants postProcessConstants{};
    const bool intermediateCapture = m_captureStage != RenderCaptureStage::Tonemap;
    postProcessConstants.postProcessParams0 = {
        m_settings.exposure,
        m_settings.bloomThreshold,
        !intermediateCapture && m_settings.bloomEnabled ? m_settings.bloomIntensity : 0.0f,
        IsLinearDataCaptureStage(m_captureStage)
            ? 0.0f : (m_settings.hdrEnabled ? 1.0f : 0.0f)};
    postProcessConstants.postProcessParams1 = {
        IsLinearDataCaptureStage(m_captureStage)
            ? 0.0f
            : (m_settings.tonemappingEnabled ? 1.0f : 0.0f),
        1.0f / static_cast<float>(
            m_bloomTextureA->GetDescription().width),
        1.0f / static_cast<float>(
            m_bloomTextureA->GetDescription().height),
        m_settings.skyboxEnabled
            ? std::clamp(
                  m_settings.skyEnvironmentBlend,
                  0.0f,
                  1.0f)
            : 0.0f};
    postProcessConstants.skyZenithAtmosphere = {
        m_settings.skyZenithColor.x,
        m_settings.skyZenithColor.y,
        m_settings.skyZenithColor.z,
        m_settings.atmosphereDensity};
    postProcessConstants.skyHorizonPower = {
        m_settings.skyHorizonColor.x,
        m_settings.skyHorizonColor.y,
        m_settings.skyHorizonColor.z,
        m_settings.iblHorizonSharpness};
    postProcessConstants.groundGridEnabled = {
        m_settings.groundColor.x,
        m_settings.groundColor.y,
        m_settings.groundColor.z,
        m_settings.editorGridEnabled
                && m_editorGridAllowed
            ? 1.0f
            : 0.0f};

    const XMVECTOR worldSunDirection = XMVector3Normalize(
        XMVectorSet(-light.direction.x, -light.direction.y, -light.direction.z, 0.0f));
    XMFLOAT3 sunDirection{};
    XMStoreFloat3(&sunDirection, worldSunDirection);
    postProcessConstants.sunDirectionIntensity = {
        sunDirection.x, sunDirection.y, sunDirection.z, light.intensity};
    postProcessConstants.sunColorAngularRadius = {
        light.color.x,
        light.color.y,
        light.color.z,
        XMConvertToRadians(std::max(m_settings.sunAngularRadiusDegrees, 0.05f))};
    const bool physicalAtmosphereEnabled =
        m_settings.physicalAtmosphereEnabled
        && m_settings.skyboxEnabled;
    postProcessConstants.atmosphereLutParams = {
        physicalAtmosphereEnabled ? 1.0f : 0.0f,
        static_cast<float>(
            SkyAtmosphere::SkyViewWidth),
        static_cast<float>(
            SkyAtmosphere::SkyViewHeight),
        m_settings.atmosphereBrightness};
    const XMFLOAT3 cameraForward = camera.GetForwardVector();
    const XMFLOAT3 cameraRight = camera.GetRightVector();
    const XMFLOAT3 cameraUp = camera.GetUpVector();
    postProcessConstants.cameraPositionTanHalfFov = {
        cameraPosition.x,
        cameraPosition.y,
        cameraPosition.z,
        std::tan(camera.GetFieldOfViewYRadians() * 0.5f)};
    postProcessConstants.cameraForwardAspect = {
        cameraForward.x, cameraForward.y, cameraForward.z, camera.GetAspectRatio()};
    postProcessConstants.cameraRightGridScale = {
        cameraRight.x, cameraRight.y, cameraRight.z, std::max(m_settings.gridScale, 0.01f)};
    postProcessConstants.cameraUpGridFade = {
        cameraUp.x, cameraUp.y, cameraUp.z, std::max(m_settings.gridFadeDistance, 1.0f)};
    postProcessConstants.gridMinorColorLineWidth = {
        m_settings.gridMinorColor.x,
        m_settings.gridMinorColor.y,
        m_settings.gridMinorColor.z,
        1.0f};
    postProcessConstants.gridMajorColorAxisWidth = {
        m_settings.gridMajorColor.x,
        m_settings.gridMajorColor.y,
        m_settings.gridMajorColor.z,
        1.0f};
    m_postProcessBuffers[m_activeFrame]->Update(
        &postProcessConstants, sizeof(postProcessConstants));

    const std::vector<Scene::RenderObject>& objects = scene.GetRenderObjects();
    Core::Check(
        objects.size()
            == m_objectResources.size(),
        "Scene resources are out of sync.");
    m_currentWorldViewProjections.resize(
        objects.size());
    const OceanClipmapPlacement oceanPlacement =
        ComputeOceanClipmapPlacement(
            renderOrigin.x,
            renderOrigin.z,
            m_settings.oceanPatchLength,
            FftOcean::Resolution,
            m_settings.oceanCameraFollowEnabled);
    const double snappedOceanX = oceanPlacement.snappedX;
    const double snappedOceanZ = oceanPlacement.snappedZ;
    const auto buildObjectWorld =
        [&](const std::size_t objectIndex)
        {
            const Scene::RenderObject& object =
                objects[objectIndex];
            if (object.surfaceType
                    != Scene::RenderSurfaceType::FftOcean
                || !m_settings.oceanCameraFollowEnabled)
            {
                return relativeToEye
                    ? scene.GetObjectRelativeWorldMatrix(
                          objectIndex,
                          renderOrigin)
                    : scene.GetObjectWorldMatrix(objectIndex);
            }
            const double oceanHeight =
                object.transform.GetWorldPosition().y;
            if (m_oceanTessellationActive)
            {
                return XMMatrixTranslation(
                    static_cast<float>(relativeToEye ? -renderOrigin.x : 0.0),
                    static_cast<float>((relativeToEye
                        ? oceanHeight - renderOrigin.y : oceanHeight)
                        + m_settings.ocean.geometry.meanSeaLevel),
                    static_cast<float>(relativeToEye ? -renderOrigin.z : 0.0));
            }
            return XMMatrixTranslation(
                static_cast<float>(relativeToEye
                    ? snappedOceanX - renderOrigin.x
                    : snappedOceanX),
                static_cast<float>(relativeToEye
                    ? oceanHeight - renderOrigin.y
                    : oceanHeight),
                static_cast<float>(relativeToEye
                    ? snappedOceanZ - renderOrigin.z
                    : snappedOceanZ));
        };
    for (std::size_t objectIndex = 0;
         objectIndex < objects.size();
         ++objectIndex)
    {
        const XMMATRIX world = buildObjectWorld(
            objectIndex);
        XMStoreFloat4x4(
            &m_currentWorldViewProjections[
                objectIndex],
            XMMatrixTranspose(
                world * viewProjection));
    }
    if (m_previousWorldViewProjections.size()
        != m_currentWorldViewProjections.size())
    {
        m_previousWorldViewProjections =
            m_currentWorldViewProjections;
    }
    BuildInstanceBatches(scene);
    std::vector<std::uint32_t> drawInstanceOffsets(
        objects.size());
    if (!m_gpuDrivenActiveThisFrame)
    {
        for (std::size_t objectIndex = 0;
             objectIndex < objects.size();
             ++objectIndex)
        {
            drawInstanceOffsets[objectIndex] =
                static_cast<std::uint32_t>(
                    objectIndex);
        }
        for (const InstanceBatch& batch :
             m_instanceBatches)
        {
            drawInstanceOffsets[
                batch.representativeObjectIndex] =
                batch.firstInstance;
        }
    }
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        ObjectConstants objectConstants{};
        const Scene::RenderObject& object =
            objects[objectIndex];
        const XMMATRIX world = buildObjectWorld(objectIndex);
        XMStoreFloat4x4(
            &objectConstants.world,
            XMMatrixTranspose(world));
        const XMMATRIX normalMatrix =
            XMMatrixTranspose(XMMatrixInverse(nullptr, world));
        XMStoreFloat4x4(&objectConstants.normalMatrix,
                        XMMatrixTranspose(normalMatrix));
        objectConstants.worldViewProjection =
            m_currentWorldViewProjections[objectIndex];
        objectConstants.previousWorldViewProjection =
            m_previousWorldViewProjections[objectIndex];
        objectConstants.renderFeatureParams = {
            static_cast<float>(object.surfaceType),
            object.surfaceType == Scene::RenderSurfaceType::FftOcean
                ? (m_settings.ocean.implementation ==
                           OceanImplementation::SpectralOcean
                       ? m_settings.ocean.simulationPeriodMeters
                       : m_settings.oceanPatchLength)
                : m_settings.terrainWorldSize,
            static_cast<float>(object.surfaceType ==
                                       Scene::RenderSurfaceType::FftOcean
                                   ? (m_settings.ocean.implementation ==
                                              OceanImplementation::SpectralOcean
                                          ? m_spectralOcean->Resolution()
                                          : FftOcean::Resolution)
                                   : VirtualTextureCache::VirtualPageCount),
            static_cast<float>(VirtualTextureCache::PhysicalPageGrid)};
        if (object.surfaceType == Scene::RenderSurfaceType::VirtualTerrain)
        {
            // A zero virtual page count is the shader-visible switch for the
            // physical terrain atlas. The old path only stopped cache updates,
            // so stale resident pages were still sampled when the UI checkbox
            // was disabled.
            objectConstants.renderFeatureParams.z =
                m_settings.terrainVirtualTextureEnabled
                    ? static_cast<float>(VirtualTextureCache::VirtualPageCount)
                    : 0.0f;
            float terrainDebugMode =
                m_settings.terrainTileDebugEnabled ? 1.0f : 0.0f;
            const Scene::GpuVisibilityReason visibilityReason =
                scene.GetGpuVisibilityReason(objectIndex);
            if (m_settings.terrainTileDebugEnabled &&
                visibilityReason !=
                    Scene::GpuVisibilityReason::Unknown)
            {
                terrainDebugMode =
                    1.0f + static_cast<float>(visibilityReason);
            }
            objectConstants.renderFeatureParams2 = {
                object.quadtreePatch.halfExtent * 2.0f,
                static_cast<float>(object.quadtreePatch.level),
                terrainDebugMode,
                m_settings.terrainTileBorderWidth};
            objectConstants.renderFeatureParams3 = {
                m_settings.terrainMaterialColorsEnabled ? 1.0f : 0.0f,
                m_settings.terrainBaseHeight,
                m_settings.terrainHeightScale,
                m_settings.interactiveTerrainEnabled ? 1.0f : 0.0f};
        }
        else if (object.surfaceType == Scene::RenderSurfaceType::FftOcean)
        {
            constexpr std::array<float, 3> LegacyOceanRingMipLevels{
                0.0f, 3.0f, 5.0f};
            constexpr std::array<float, 3> SpectralOceanCellSizesMeters{
                3.125f, 12.5f, 50.0f};
            const std::size_t oceanRingIndex = std::min<std::size_t>(
                object.surfaceLodLevel,
                LegacyOceanRingMipLevels.size() - 1u);
            // Spectral shaders interpret z as physical vertex spacing and
            // derive a mip independently for each cascade. Legacy FFT keeps
            // the historical direct mip-level contract.
            const float oceanSamplingParameter =
                m_settings.ocean.implementation
                        == OceanImplementation::SpectralOcean
                    ? SpectralOceanCellSizesMeters[oceanRingIndex]
                    : LegacyOceanRingMipLevels[oceanRingIndex];
            const double oceanOriginX =
                m_settings.oceanCameraFollowEnabled
                    ? snappedOceanX
                    : object.transform.GetWorldPosition().x;
            const double oceanOriginZ =
                m_settings.oceanCameraFollowEnabled
                ? snappedOceanZ
                : object.transform.GetWorldPosition().z;
            const float oceanPeriod = m_settings.ocean.implementation
                    == OceanImplementation::SpectralOcean
                ? m_settings.ocean.simulationPeriodMeters
                : m_settings.oceanPatchLength;
            objectConstants.renderFeatureParams2 = {
                static_cast<float>(std::fmod(
                    oceanOriginX,
                    static_cast<double>(oceanPeriod))),
                static_cast<float>(std::fmod(
                    oceanOriginZ,
                    static_cast<double>(oceanPeriod))),
                oceanSamplingParameter,
                m_settings.ocean.implementation
                        == OceanImplementation::SpectralOcean
                    ? (m_oceanTessellationActive ? 2.0f : 1.0f) : 0.0f};
            if (m_settings.ocean.implementation
                == OceanImplementation::SpectralOcean)
            {
                // Shading, diagnostic views, and WaveWorks cascade/stage
                // isolation share two exact-in-float bit fields. The helper
                // applies both the stage mask and the cascade master mask;
                // no PSO variants or descriptor-layout changes are needed.
                const OceanRuntimeFlags oceanRuntimeFlags =
                    BuildOceanRuntimeFlags(m_settings.ocean);
                objectConstants.renderFeatureParams3 = {
                    m_settings.ocean.local.domainCenter.x,
                    m_settings.ocean.local.domainCenter.y,
                    m_settings.ocean.local.domainSizeMeters,
                    m_settings.ocean.local.enabled ? 1.0f : 0.0f};
                objectConstants.renderFeatureParams4 = {
                    m_settings.ocean.shading.uvWarpingAmplitude,
                    m_settings.ocean.shading.uvWarpingFrequency,
                    m_settings.ocean.geometry.geomorphingDegree,
                    static_cast<float>(m_settings.ocean.geometry.cellsPerPatch)};
                // Ocean shaders need both independent WaveWorks whitecap
                // thresholds while the shared object layout has one unused
                // ocean scalar. Pack two UNORM10 values into an exactly
                // representable numeric float without growing the
                // generic mesh constant buffer or changing its descriptors.
                const auto packThreshold = [](const float value)
                {
                    return static_cast<std::uint32_t>(std::lround(
                        std::clamp(value, 0.0f, 1.0f) * 1023.0f));
                };
                const std::uint32_t packedWhitecapThresholds =
                    packThreshold(m_settings.ocean.foam.whitecapsThreshold)
                    | (packThreshold(
                           m_settings.ocean.local.foam.whitecapsThreshold)
                        << 10u);
                objectConstants.renderFeatureParams.w =
                    static_cast<float>(packedWhitecapThresholds);
                objectConstants.drawInstanceParams.y =
                    static_cast<float>(oceanRuntimeFlags.primary);
                objectConstants.drawInstanceParams.z =
                    std::max(m_settings.ocean.shading.sunIntensity, 0.0f);
                objectConstants.drawInstanceParams.w =
                    static_cast<float>(oceanRuntimeFlags.secondary);
            }
        }
        objectConstants.drawInstanceParams.x =
            static_cast<float>(
                drawInstanceOffsets[objectIndex]);
        m_objectBuffers[m_activeFrame]->Update(
            &objectConstants, sizeof(objectConstants),
            objectIndex * ObjectConstantStride);
        m_indexedObjectBuffers[m_activeFrame]->Update(
            &objectConstants, sizeof(objectConstants),
            objectIndex * sizeof(ObjectConstants));
    }
    m_previousWorldViewProjections = m_currentWorldViewProjections;
    m_temporalAntiAliasing.Update(m_activeFrame,
                                  m_settings.temporalAntiAliasingEnabled &&
                                      m_settings.deferredRenderingEnabled &&
                                      !m_settings.fluid.enabled);
    OceanSettings oceanSimulationSettings = m_settings.ocean;
    if (oceanSimulationSettings.implementation ==
        OceanImplementation::LegacyFft)
    {
        RenderSettings legacySettings = m_settings;
        legacySettings.SyncLegacyOceanFields();
        oceanSimulationSettings = legacySettings.ocean;
    }
    if (m_sharedSimulationProducerThisFrame &&
        oceanSimulationSettings.debug.simulateWater &&
        oceanSimulationSettings.implementation ==
            OceanImplementation::SpectralOcean)
    {
        (void)m_spectralOcean->Update(m_activeFrame, simulationTimeSeconds,
                                      oceanSimulationSettings);
    }
    else if (oceanSimulationSettings.debug.simulateWater &&
             oceanSimulationSettings.implementation ==
                 OceanImplementation::LegacyFft)
    {
        m_fftOcean.Update(m_activeFrame,
                          static_cast<float>(simulationTimeSeconds),
                          oceanSimulationSettings);
    }
    m_fluidFeature.Update(
        m_activeFrame, simulationTimeSeconds, scene, camera, m_settings.fluid);
    m_planarReflections.Update(scene,
        m_activeFrame,
        m_settings.planarReflectionPlaneHeight,
        m_settings.planarReflectionsEnabled &&
            m_settings.deferredRenderingEnabled,
        m_settings.skyHorizonColor);
    m_varianceShadowMaps.Update(
        m_activeFrame, m_settings.shadowFilterMode == ShadowFilterMode::Evsm);
    m_screenSpaceEffects.Update(m_activeFrame,
        viewProjection,
        m_planarReflections.GetReflectedViewProjection(),
        cameraPosition,
        m_settings.deferredRenderingEnabled,
        m_settings.gtaoEnabled,
        m_settings.screenSpaceReflectionsEnabled,
        m_settings.planarReflectionsEnabled,
        m_settings.planarReflectionPlaneHeight,
        m_settings.planarReflectionIntensity);
    // Keep the established CPU update order while graph contribution and
    // resize ownership migrate to the lifecycle registry.
    m_clusteredLighting.Update(scene,
        m_activeFrame,
        m_settings.pointLightsEnabled && m_settings.clusteredLightingEnabled &&
            (m_settings.deferredRenderingEnabled ||
                m_settings.forwardPlusEnabled));
    m_localLightShadows.Update(
        scene,
        m_activeFrame,
        m_settings.spotLightsEnabled
            && m_settings
                   .deferredRenderingEnabled,
        m_settings.shadowsEnabled
            && m_settings
                   .localLightShadowsEnabled
            && m_settings
                   .deferredRenderingEnabled);
    ++m_temporalSampleIndex;
}

void SceneRenderer::BuildInstanceBatches(
    const Scene::RenderSceneView& scene)
{
    struct BatchKey
    {
        const Asset::Mesh* mesh = nullptr;
        std::uint64_t materialIdentity = 0;
        std::uint64_t materialOverrideSignature = 0;
        bool doubleSided = false;
        std::size_t uniqueSurfaceObject = 0;

        bool operator==(const BatchKey&) const = default;
    };
    struct BatchKeyHash
    {
        std::size_t operator()(const BatchKey& key) const
        {
            std::size_t hash = std::hash<const void*>{}(
                key.mesh);
            const auto combine = [&](const std::size_t value)
            {
                hash ^= value + 0x9e3779b9u
                    + (hash << 6u)
                    + (hash >> 2u);
            };
            combine(std::hash<std::uint64_t>{}(
                key.materialIdentity));
            combine(std::hash<std::uint64_t>{}(
                key.materialOverrideSignature));
            combine(std::hash<bool>{}(
                key.doubleSided));
            combine(std::hash<std::size_t>{}(
                key.uniqueSurfaceObject));
            return hash;
        }
    };

    const std::vector<Scene::RenderObject>& objects =
        scene.GetRenderObjects();
    std::vector<std::uint32_t> objectIndices;
    objectIndices.reserve(objects.size() * 2u);
    for (std::size_t index = 0;
         index < objects.size();
         ++index)
    {
        objectIndices.push_back(
            static_cast<std::uint32_t>(index));
    }
    m_instanceBatches.clear();
    if (m_settings.gpuInstancingEnabled
        && m_indexedObjectDrawingSupported
        && !m_gpuDrivenActiveThisFrame)
    {
        std::vector<std::vector<std::uint32_t>> groups;
        std::unordered_map<BatchKey, std::size_t, BatchKeyHash>
            batchLookup;
        for (std::size_t objectIndex = 0;
             objectIndex < objects.size();
             ++objectIndex)
        {
            const Scene::RenderObject& object =
                objects[objectIndex];
            const Asset::Mesh* const mesh = scene.GetMesh(objectIndex);
            const Scene::RenderSceneMaterialBinding* const material =
                scene.FindMaterialBinding(objectIndex);
            if (!scene.IsObjectSelected(objectIndex)
                || !object.visible
                || mesh == nullptr
                || (material == nullptr && object.material == nullptr)
                || object.surfaceType
                    == Scene::RenderSurfaceType::EditorDebugLine
                || ResolveMaterialRenderQueue(scene, objectIndex)
                    == MaterialRenderQueue::Transparent)
            {
                continue;
            }
            const BatchKey key{
                mesh,
                material != nullptr
                    ? material->revision.value
                    : static_cast<std::uint64_t>(
                          reinterpret_cast<std::uintptr_t>(
                              object.material.get())),
                ResolveMaterialOverrideSignature(object),
                IsDoubleSidedMaterial(scene, objectIndex),
                object.surfaceType
                        == Scene::RenderSurfaceType::Default
                    ? 0u
                    : objectIndex + 1u};
            const auto [iterator, inserted] =
                batchLookup.emplace(
                    key,
                    groups.size());
            if (inserted)
            {
                groups.emplace_back();
            }
            groups[iterator->second].push_back(
                static_cast<std::uint32_t>(
                    objectIndex));
        }
        for (const std::vector<std::uint32_t>& group :
             groups)
        {
            Core::Check(
                !group.empty(),
                "An instance batch cannot be empty.");
            const std::uint32_t firstInstance =
                static_cast<std::uint32_t>(
                    objectIndices.size());
            objectIndices.insert(
                objectIndices.end(),
                group.begin(),
                group.end());
            m_instanceBatches.push_back({
                group.front(),
                firstInstance,
                static_cast<std::uint32_t>(
                    group.size())});
            if (group.size() > 1u)
            {
                ++m_statistics.instancedDrawCalls;
                m_statistics.instancedObjects +=
                    static_cast<std::uint32_t>(
                        group.size());
                ++m_statistics.instanceBatchCount;
            }
        }
    }
    Core::Check(
        objectIndices.size()
            <= objects.size() * 2u,
        "Instance object-index data exceeded its allocated capacity.");
    if (!objectIndices.empty())
    {
        m_instanceObjectIndexBuffers[m_activeFrame]
            ->Update(
                objectIndices.data(),
                objectIndices.size()
                    * sizeof(std::uint32_t));
    }
}

} // namespace Prism::Renderer
