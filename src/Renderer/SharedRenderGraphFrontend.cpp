#include "Renderer/SharedRenderGraphFrontend.h"

#include "Core/Assert.h"
#include "RHI/GraphicsResources.h"
#include "Renderer/Features/ClusteredLighting.h"
#include "Renderer/Features/GpuDrivenVisibility.h"
#include "Renderer/Features/FftOcean.h"
#include "Renderer/Features/InteractiveTerrain.h"
#include "Renderer/Features/LocalLightShadows.h"
#include "Renderer/Features/Ocean/LocalWaveGpuResources.h"
#include "Renderer/Features/Ocean/SpectralOceanSimulation.h"
#include "Renderer/Features/PlanarReflections.h"
#include "Renderer/Features/ScreenSpaceEffects.h"
#include "Renderer/Features/SkyAtmosphere.h"
#include "Renderer/Features/TemporalAntiAliasing.h"
#include "Renderer/Features/VarianceShadowMaps.h"
#include "Renderer/Pipeline/ScenePipelinePlan.h"

#include <string>
#include <utility>

namespace Prism::Renderer
{
namespace
{
void RequireCallback(
    const RenderGraph::ParameterExecuteCallback& callback,
    const char* message)
{
    Core::Check(
        static_cast<bool>(callback),
        message);
}

void ReadFrameConstants(
    RenderGraphPassParameters& parameters,
    const BufferHandle handle)
{
    parameters.ReadBuffer(
        handle,
        RHI::ResourceState::ConstantBuffer);
}

void AddHiZPasses(
    RenderGraph& graph,
    const TextureHandle sourceDepth,
    TextureHandle& hiZ,
    const RHI::ITexture& hiZTexture,
    const HiZExecuteCallback& execute)
{
    Core::Check(static_cast<bool>(execute),
        "The shared render graph requires a HiZBuild callback.");
    const std::uint32_t mipCount =
        hiZTexture.GetDescription().mipLevels;
    Core::Check(mipCount > 0,
        "Hi-Z requires at least one mip level.");
    for (std::uint32_t mipIndex = 0u;
         mipIndex < mipCount;
         ++mipIndex)
    {
        auto parameters = graph.CreatePassParameters();
        if (mipIndex == 0u)
        {
            parameters.ReadTexture(
                sourceDepth,
                RHI::ResourceState::ShaderResource);
        }
        else
        {
            parameters.ReadTexture(
                hiZ,
                RHI::ResourceState::ShaderResource,
                {mipIndex - 1u, 1u, 0u, 1u});
        }
        hiZ = parameters.WriteTexture(
            hiZ,
            RHI::ResourceState::UnorderedAccess,
            {mipIndex, 1u, 0u, 1u});
        graph.AddParameterPass(
            mipIndex == 0u
                ? "HiZBuild"
                : "HiZBuild.Mip" + std::to_string(mipIndex),
            std::move(parameters),
            [execute, mipIndex](
                RHI::ICommandContext& commandContext,
                const RenderGraphPassResources& passResources)
            {
                execute(commandContext, passResources, mipIndex);
            },
            RenderGraph::PassOptions{
                RenderGraph::QueueClass::Compute,
                true,
                false,
                true,
                RenderGraph::ParallelRecordingContract::AuditedIndependent()});
    }
    auto readyParameters = graph.CreatePassParameters();
    readyParameters.ReadTexture(
        hiZ,
        RHI::ResourceState::ShaderResource);
    graph.AddParameterPass(
        "HiZReady",
        std::move(readyParameters),
        [](RHI::ICommandContext&,
           const RenderGraphPassResources&) {},
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Compute,
            false,
            false,
            true,
            RenderGraph::ParallelRecordingContract::AuditedIndependent()});
}
} // namespace

SharedSceneGraphHandles
SharedRenderGraphFrontend::Build(
    RenderGraph& graph,
    const SharedRenderGraphResources& resources,
    const SharedRenderGraphOptions& options,
    const SharedRenderGraphCallbacks& callbacks)
{
    Core::Check(
        resources.frameConstants != nullptr
            && resources.shadowMap != nullptr
            && resources.depthBuffer != nullptr
            && resources.hiZ != nullptr
            && resources.hdrColor != nullptr
            && resources.bloomA != nullptr
            && resources.bloomB != nullptr
            && resources.outputColor != nullptr,
        "The shared render graph requires all frame resources.");
    for (const RHI::ITexture* texture :
         resources.gbuffer)
    {
        Core::Check(
            texture != nullptr,
            "The shared render graph requires every GBuffer texture.");
    }

    // A feature can be temporarily unavailable while scene resources are
    // being rebuilt (for example during a capture or live quality switch).
    // Fall back to the CPU submission path instead of dereferencing an
    // incomplete indirect-resource set; the next frame can re-enable GPU
    // driven rendering once all buffers are published.
    SharedRenderGraphOptions effectiveOptions = options;
    if (effectiveOptions.gpuDrivenEnabled
        && !graph.GetBlackboard().ContainsFeature<
            GpuDrivenVisibilityFeatureSlot>())
    {
        effectiveOptions.gpuDrivenEnabled = false;
    }
    const ScenePipelineExecution execution =
        ResolveScenePipelineExecution(effectiveOptions);
    const bool clusteredLightingEnabled =
        execution.clusteredLighting;
    const bool localLightShadowsEnabled =
        execution.localLightShadows;
    const bool planarReflectionsEnabled =
        execution.planarReflections;
    const bool varianceShadowsEnabled =
        execution.varianceShadows;
    const bool gtaoEnabled = execution.gtao;
    const bool screenSpaceCompositeEnabled =
        execution.screenSpaceComposite;
    const bool temporalAntiAliasingEnabled =
        execution.temporalAntiAliasing;
    const bool skyAtmosphereEnabled =
        execution.skyAtmosphere;
    const bool oceanFeatureEnabled =
        execution.fftOcean || execution.spectralOcean;
    const bool oceanSimulationEnabled =
        oceanFeatureEnabled && effectiveOptions.oceanSimulationEnabled;
    const bool localWaveEnabled =
        execution.spectralOcean && effectiveOptions.localWaveEnabled;
    const bool interactiveTerrainEnabled =
        options.interactiveTerrainEnabled;
    const bool interactiveTerrainUpdateEnabled =
        interactiveTerrainEnabled
        && options.interactiveTerrainUpdateEnabled;
    const bool hiZEnabled = execution.hiZ;
    const bool waterVisibilityEnabled = execution.waterVisibility;
    const WaterOpticsGraphContribution* waterOptics = nullptr;
    const SkyAtmosphereGraphContribution* atmosphere = nullptr;
    const GpuDrivenVisibilityGraphContribution* gpuVisibility = nullptr;
    const InteractiveTerrainGraphContribution* terrain = nullptr;
    const ClusteredLightingGraphContribution* clusteredLighting = nullptr;
    const LocalLightShadowsGraphContribution& localLightShadows =
        graph.GetBlackboard().Require<
            LocalLightShadowsFeatureSlot,
            LocalLightShadowsGraphContribution>({
                "SharedRenderGraphFrontend",
                options.viewId,
                graph.GetResourceGeneration(),
                1u,
                "LocalLightShadows.Contribution"});
    const VarianceShadowMapsGraphContribution& varianceShadows =
        graph.GetBlackboard().Require<
            VarianceShadowMapsFeatureSlot,
            VarianceShadowMapsGraphContribution>({
                "SharedRenderGraphFrontend",
                options.viewId,
                graph.GetResourceGeneration(),
                1u,
                "VarianceShadowMaps.Contribution"});
    const PlanarReflectionsGraphContribution* planarReflections = nullptr;
    const ScreenSpaceEffectsGraphContribution& screenSpaceEffects =
        graph.GetBlackboard().Require<
            ScreenSpaceEffectsFeatureSlot,
            ScreenSpaceEffectsGraphContribution>({
                "SharedRenderGraphFrontend",
                options.viewId,
                graph.GetResourceGeneration(),
                1u,
                "ScreenSpaceEffects.Contribution"});
    const TemporalAntiAliasingGraphContribution& temporalAntiAliasing =
        graph.GetBlackboard().Require<
            TemporalAntiAliasingFeatureSlot,
            TemporalAntiAliasingGraphContribution>({
                "SharedRenderGraphFrontend",
                options.viewId,
                graph.GetResourceGeneration(),
                1u,
                "TemporalAntiAliasing.Contribution"});
    const FftOceanGraphContribution* fftOcean = nullptr;
    const SpectralOceanGraphContribution* spectralOcean = nullptr;
    const LocalWaveGraphContribution* localWave = nullptr;

    Core::Check(localLightShadows.lightingConstants != nullptr
            && localLightShadows.renderConstants != nullptr
            && localLightShadows.spotShadowMap != nullptr
            && localLightShadows.pointShadowMap != nullptr
            && static_cast<bool>(localLightShadows.spotExecute)
            && static_cast<bool>(localLightShadows.pointExecute),
        "Local-light shadows require complete typed resources.");
    Core::Check(varianceShadows.moments != nullptr
            && varianceShadows.scratch != nullptr
            && static_cast<bool>(varianceShadows.convert)
            && static_cast<bool>(varianceShadows.horizontal)
            && static_cast<bool>(varianceShadows.vertical),
        "Variance shadows require complete typed resources.");
    Core::Check(screenSpaceEffects.ambientOcclusion != nullptr
            && screenSpaceEffects.composite != nullptr
            && static_cast<bool>(screenSpaceEffects.gtao)
            && static_cast<bool>(screenSpaceEffects.reflections),
        "Screen-space effects require complete typed resources.");
    Core::Check(temporalAntiAliasing.motionVectors != nullptr
            && temporalAntiAliasing.resolved != nullptr
            && temporalAntiAliasing.historyRead != nullptr
            && temporalAntiAliasing.historyWrite != nullptr
            && static_cast<bool>(temporalAntiAliasing.execute),
        "Temporal anti-aliasing requires complete typed resources.");

    if (clusteredLightingEnabled)
    {
        clusteredLighting = &graph.GetBlackboard().Require<
            ClusteredLightingFeatureSlot,
            ClusteredLightingGraphContribution>({
                "SharedRenderGraphFrontend",
                options.viewId,
                graph.GetResourceGeneration(),
                1u,
                "ClusteredLighting.Contribution"});
        Core::Check(clusteredLighting->clusterConstants != nullptr
                && clusteredLighting->pointLights != nullptr
                && clusteredLighting->clusterLightCounts != nullptr
                && clusteredLighting->clusterLightIndices != nullptr
                && static_cast<bool>(clusteredLighting->execute),
            "Clustered lighting requires complete typed resources.");
    }
    if (planarReflectionsEnabled)
    {
        planarReflections = &graph.GetBlackboard().Require<
            PlanarReflectionsFeatureSlot,
            PlanarReflectionsGraphContribution>({
                "SharedRenderGraphFrontend",
                options.viewId,
                graph.GetResourceGeneration(),
                1u,
                "PlanarReflections.Contribution"});
        Core::Check(planarReflections->color != nullptr
                && planarReflections->depth != nullptr
                && static_cast<bool>(planarReflections->execute),
            "Planar reflections require complete typed resources.");
    }
    if (execution.fftOcean)
    {
        fftOcean = &graph.GetBlackboard().Require<
            FftOceanFeatureSlot,
            FftOceanGraphContribution>({
                "SharedRenderGraphFrontend",
                options.viewId,
                graph.GetResourceGeneration(),
                1u,
                "FftOcean.Contribution"});
        Core::Check(fftOcean->spectrumA[0] != nullptr
                && fftOcean->spectrumA[1] != nullptr
                && fftOcean->spectrumB[0] != nullptr
                && fftOcean->spectrumB[1] != nullptr
                && fftOcean->displacement != nullptr
                && fftOcean->normalFoam != nullptr
                && static_cast<bool>(fftOcean->execute),
            "FFT ocean requires complete typed resources.");
    }
    if (execution.spectralOcean)
    {
        spectralOcean = &graph.GetBlackboard().Require<
            SpectralOceanFeatureSlot,
            SpectralOceanGraphContribution>({
                "SharedRenderGraphFrontend",
                options.viewId,
                graph.GetResourceGeneration(),
                1u,
                "SpectralOcean.Contribution"});
        Core::Check(spectralOcean->initialSpectrum != nullptr
                && spectralOcean->spectrumA[0] != nullptr
                && spectralOcean->spectrumA[1] != nullptr
                && spectralOcean->spectrumB[0] != nullptr
                && spectralOcean->spectrumB[1] != nullptr
                && spectralOcean->displacement != nullptr
                && spectralOcean->gradientFoam != nullptr
                && spectralOcean->slopeMoments != nullptr
                && spectralOcean->foamHistory[0] != nullptr
                && spectralOcean->foamHistory[1] != nullptr,
            "Spectral ocean requires complete typed resources.");
    }
    if (localWaveEnabled)
    {
        localWave = &graph.GetBlackboard().Require<
            LocalWaveFeatureSlot,
            LocalWaveGraphContribution>({
                "SharedRenderGraphFrontend",
                options.viewId,
                graph.GetResourceGeneration(),
                1u,
                "LocalWave.Contribution"});
        Core::Check(localWave->height[0] != nullptr
                && localWave->height[1] != nullptr
                && localWave->velocity[0] != nullptr
                && localWave->velocity[1] != nullptr
                && localWave->foam[0] != nullptr
                && localWave->foam[1] != nullptr
                && localWave->displacement != nullptr
                && localWave->gradient != nullptr
                && localWave->disturbanceUpload != nullptr
                && localWave->disturbance != nullptr,
            "Local wave simulation requires complete typed resources.");
    }

    if (execution.gpuDriven)
    {
        gpuVisibility = &graph.GetBlackboard().Require<
            GpuDrivenVisibilityFeatureSlot,
            GpuDrivenVisibilityGraphContribution>({
                "SharedRenderGraphFrontend",
                options.viewId,
                graph.GetResourceGeneration(),
                1u,
                "GpuDrivenVisibility.Contribution"});
        Core::Check(gpuVisibility->objectRecords != nullptr
                && gpuVisibility->indirectArguments != nullptr
                && gpuVisibility->indirectDrawCounts != nullptr
                && static_cast<bool>(gpuVisibility->execute),
            "GPU-driven rendering requires complete typed resources.");
    }
    if (interactiveTerrainEnabled)
    {
        terrain = &graph.GetBlackboard().Require<
            InteractiveTerrainFeatureSlot,
            InteractiveTerrainGraphContribution>({
                "SharedRenderGraphFrontend",
                options.viewId,
                graph.GetResourceGeneration(),
                1u,
                "InteractiveTerrain.Contribution"});
        Core::Check(terrain->rawHeight != nullptr
                && terrain->erodedHeight != nullptr
                && static_cast<bool>(terrain->execute),
            "Interactive terrain requires complete typed resources.");
    }

    if (skyAtmosphereEnabled)
    {
        atmosphere = &graph.GetBlackboard().Require<
            SkyAtmosphereFeatureSlot,
            SkyAtmosphereGraphContribution>({
                "SharedRenderGraphFrontend",
                options.viewId,
                graph.GetResourceGeneration(),
                1u,
                "SkyAtmosphere.Contribution"});
        Core::Check(atmosphere->transmittance != nullptr
                && atmosphere->skyView != nullptr
                && static_cast<bool>(atmosphere->transmittanceExecute)
                && static_cast<bool>(atmosphere->skyViewExecute),
            "Physical atmosphere requires both LUT resources and callbacks.");
    }

    if (waterVisibilityEnabled)
    {
        waterOptics = &graph.GetBlackboard().Require<
            WaterOpticsFeatureSlot,
            WaterOpticsGraphContribution>({
                "SharedRenderGraphFrontend",
                options.viewId,
                graph.GetResourceGeneration(),
                1u,
                "WaterOptics.Contribution"});
        Core::Check(waterOptics->compositeDepth != nullptr
                && waterOptics->waterMotion != nullptr
                && waterOptics->waterGBuffer[0] != nullptr
                && waterOptics->waterGBuffer[1] != nullptr
                && waterOptics->waterGBuffer[2] != nullptr
                && static_cast<bool>(waterOptics->build),
            "HPWater visibility requires its GBuffer, motion, and composite-depth resources.");
    }

    SharedSceneGraphHandles handles{};
    handles.frameConstants = graph.ImportBuffer(
        "FrameConstants",
        *resources.frameConstants,
        RHI::ResourceState::ConstantBuffer);
    if (clusteredLightingEnabled)
    {
        handles.clusterConstants = graph.ImportBuffer(
            "ClusterConstants",
            *clusteredLighting->clusterConstants,
            RHI::ResourceState::ConstantBuffer);
        handles.pointLights = graph.ImportBuffer(
            "PointLights",
            *clusteredLighting->pointLights,
            RHI::ResourceState::ShaderResource);
        handles.clusterLightCounts = graph.ImportBuffer(
            "ClusterLightCounts",
            *clusteredLighting->clusterLightCounts,
            clusteredLighting->clusterCountInitialState);
        handles.clusterLightIndices = graph.ImportBuffer(
            "ClusterLightIndices",
            *clusteredLighting->clusterLightIndices,
            clusteredLighting->clusterIndexInitialState);
    }
    handles.localLightConstants =
        graph.ImportBuffer(
            "LocalLightConstants",
            *localLightShadows.lightingConstants,
            RHI::ResourceState::ConstantBuffer);
    handles.localShadowRenderConstants =
        graph.ImportBuffer(
            "LocalShadowRenderConstants",
            *localLightShadows.renderConstants,
            RHI::ResourceState::ConstantBuffer);
    if (execution.gpuDriven)
    {
        handles.gpuObjectRecords =
            graph.ImportBuffer(
                "GpuObjectRecords",
                *gpuVisibility->objectRecords,
                gpuVisibility->objectInitialState);
        handles.indirectArguments =
            graph.ImportBuffer(
                "IndirectArguments",
                *gpuVisibility->indirectArguments,
                gpuVisibility->indirectInitialState);
        handles.indirectDrawCounts =
            graph.ImportBuffer(
                "IndirectDrawCounts",
                *gpuVisibility->indirectDrawCounts,
                gpuVisibility->countInitialState);
    }
    if (resources.environment != nullptr)
    {
        // One identity for sky/material graphics reads and Fluid compute reads.
        // A private Fluid import misses the earlier readers during queue handoff.
        handles.environment = graph.ImportTexture(
            "Environment", *resources.environment, RHI::ResourceState::ShaderResource);
    }
    handles.shadowMap = graph.ImportTexture(
        "ShadowMap",
        *resources.shadowMap,
        resources.shadowInitialState);
    handles.spotShadowMap =
        graph.ImportTexture(
            "SpotShadowMap",
            *localLightShadows.spotShadowMap,
            localLightShadows.spotShadowInitialState);
    handles.pointShadowMap =
        graph.ImportTexture(
            "PointShadowMap",
            *localLightShadows.pointShadowMap,
            localLightShadows.pointShadowInitialState);
    if (planarReflectionsEnabled)
    {
        handles.planarReflection = graph.ImportTexture(
            "PlanarReflection",
            *planarReflections->color,
            planarReflections->colorInitialState);
        handles.planarReflectionDepth = graph.ImportTexture(
            "PlanarReflectionDepth",
            *planarReflections->depth,
            planarReflections->depthInitialState);
    }
    handles.shadowMoments =
        graph.ImportTexture(
            "ShadowMoments",
            *varianceShadows.moments,
            varianceShadows.momentsInitialState);
    handles.shadowMomentsScratch =
        graph.ImportTexture(
            "ShadowMomentsScratch",
            *varianceShadows.scratch,
            varianceShadows.scratchInitialState);
    handles.depthBuffer = graph.ImportTexture(
        "DepthBuffer",
        *resources.depthBuffer,
        resources.depthInitialState);
    handles.hiZ = graph.ImportTexture(
        "HiZ",
        *resources.hiZ,
        resources.hiZInitialState);
    if (waterVisibilityEnabled)
    {
        for (std::uint32_t index = 0u;
             index < handles.waterGBuffer.size();
             ++index)
        {
            handles.waterGBuffer[index] = graph.ImportTexture(
                "WaterGBuffer" + std::to_string(index),
                *waterOptics->waterGBuffer[index],
                waterOptics->waterGBufferInitialStates[index]);
        }
        handles.waterMotion = graph.ImportTexture(
            "WaterMotion",
            *waterOptics->waterMotion,
            waterOptics->waterMotionInitialState);
        handles.waterCompositeDepth = graph.ImportTexture(
            "WaterCompositeDepth",
            *waterOptics->compositeDepth,
            waterOptics->compositeDepthInitialState);
    }
    handles.ambientOcclusion =
        graph.ImportTexture(
            "AmbientOcclusion",
            *screenSpaceEffects.ambientOcclusion,
            screenSpaceEffects.ambientOcclusionInitialState);
    handles.screenSpaceColor =
        graph.ImportTexture(
            "ScreenSpaceColor",
            *screenSpaceEffects.composite,
            screenSpaceEffects.compositeInitialState);
    handles.motionVectors = graph.ImportTexture(
        "MotionVectors",
        *temporalAntiAliasing.motionVectors,
        temporalAntiAliasing.motionInitialState);
    handles.temporalResolved =
        graph.ImportTexture(
            "TemporalResolved",
            *temporalAntiAliasing.resolved,
            temporalAntiAliasing.resolvedInitialState);
    handles.temporalHistoryRead =
        graph.ImportTexture(
            "TemporalHistoryRead",
            *temporalAntiAliasing.historyRead,
            temporalAntiAliasing.historyReadInitialState);
    handles.temporalHistoryWrite =
        graph.ImportTexture(
            "TemporalHistoryWrite",
            *temporalAntiAliasing.historyWrite,
            temporalAntiAliasing.historyWriteInitialState);
    if (skyAtmosphereEnabled)
    {
        handles.atmosphereTransmittance =
            graph.ImportTexture(
                "AtmosphereTransmittance",
                *atmosphere->transmittance,
                atmosphere->transmittanceInitialState);
        handles.atmosphereSkyView =
            graph.ImportTexture(
                "AtmosphereSkyView",
                *atmosphere->skyView,
                atmosphere->skyViewInitialState);
    }
    if (oceanFeatureEnabled)
    {
        if (execution.spectralOcean)
        {
            handles.oceanInitialSpectrum = graph.ImportTexture(
                "OceanInitialSpectrum", *spectralOcean->initialSpectrum,
                spectralOcean->initialSpectrumState);
            handles.oceanSlopeMoments = graph.ImportTexture(
                "OceanSlopeMoments", *spectralOcean->slopeMoments,
                spectralOcean->slopeMomentsState);
            for (std::uint32_t index = 0u; index < 2u; ++index)
            {
                handles.oceanFoamHistory[index] = graph.ImportTexture(
                    "OceanFoamHistory" + std::to_string(index),
                    *spectralOcean->foamHistory[index],
                    spectralOcean->foamHistoryStates[index]);
            }
            handles.oceanFoamReadIndex = spectralOcean->foamReadIndex;
            handles.oceanFoamWriteIndex = spectralOcean->foamWriteIndex;
        }
        for (std::uint32_t index = 0; index < 2; ++index)
        {
            RHI::ITexture* spectrumA = execution.spectralOcean
                ? spectralOcean->spectrumA[index]
                : fftOcean->spectrumA[index];
            RHI::ITexture* spectrumB = execution.spectralOcean
                ? spectralOcean->spectrumB[index]
                : fftOcean->spectrumB[index];
            const RHI::ResourceState spectrumAState = execution.spectralOcean
                ? spectralOcean->spectrumAStates[index]
                : fftOcean->spectrumAStates[index];
            const RHI::ResourceState spectrumBState = execution.spectralOcean
                ? spectralOcean->spectrumBStates[index]
                : fftOcean->spectrumBStates[index];
            handles.oceanSpectrumA[index] = graph.ImportTexture(
                "OceanSpectrumA" + std::to_string(index),
                *spectrumA,
                spectrumAState);
            handles.oceanSpectrumB[index] = graph.ImportTexture(
                "OceanSpectrumB" + std::to_string(index),
                *spectrumB,
                spectrumBState);
        }
        RHI::ITexture* oceanDisplacement = execution.spectralOcean
            ? spectralOcean->displacement : fftOcean->displacement;
        RHI::ITexture* oceanNormalFoam = execution.spectralOcean
            ? spectralOcean->gradientFoam : fftOcean->normalFoam;
        const RHI::ResourceState oceanDisplacementState = execution.spectralOcean
            ? spectralOcean->displacementState : fftOcean->displacementState;
        const RHI::ResourceState oceanNormalFoamState = execution.spectralOcean
            ? spectralOcean->gradientFoamState : fftOcean->normalFoamState;
        handles.oceanDisplacement = graph.ImportTexture(
            "OceanDisplacement",
            *oceanDisplacement,
            oceanDisplacementState);
        handles.oceanNormalFoam = graph.ImportTexture(
            "OceanNormalFoam",
            *oceanNormalFoam,
            oceanNormalFoamState);
        if (effectiveOptions.oceanQueriesEnabled)
        {
            Core::Check(resources.oceanQueryConstants != nullptr
                    && resources.oceanQueryPoints != nullptr
                    && resources.oceanQueryResults != nullptr
                    && resources.oceanQueryReadback != nullptr
                    && resources.oceanQueryCount > 0u
                    && static_cast<bool>(callbacks.oceanQuery),
                "Ocean queries require bounded buffers and a dispatch callback.");
            handles.oceanQueryConstants = graph.ImportBuffer(
                "Ocean.Query.Constants", *resources.oceanQueryConstants,
                RHI::ResourceState::ConstantBuffer);
            handles.oceanQueryPoints = graph.ImportBuffer(
                "Ocean.Query.Points", *resources.oceanQueryPoints,
                RHI::ResourceState::ShaderResource);
            handles.oceanQueryResults = graph.ImportBuffer(
                "Ocean.Query.Results", *resources.oceanQueryResults,
                RHI::ResourceState::UnorderedAccess);
            handles.oceanQueryReadback = graph.ImportBuffer(
                "Ocean.Query.Readback", *resources.oceanQueryReadback,
                RHI::ResourceState::CopyDestination);
        }
    }
    if (localWaveEnabled)
    {
        for (std::uint32_t index = 0u; index < 2u; ++index)
        {
            handles.localWaveHeight[index] = graph.ImportTexture(
                "Ocean.LocalWave.Height" + std::to_string(index),
                *localWave->height[index],
                localWave->heightStates[index]);
            handles.localWaveVelocity[index] = graph.ImportTexture(
                "Ocean.LocalWave.Velocity" + std::to_string(index),
                *localWave->velocity[index],
                localWave->velocityStates[index]);
            handles.localWaveFoam[index] = graph.ImportTexture(
                "Ocean.LocalWave.Foam" + std::to_string(index),
                *localWave->foam[index],
                localWave->foamStates[index]);
        }
        handles.localWaveDisplacement = graph.ImportTexture(
            "Ocean.LocalWave.Displacement",
            *localWave->displacement,
            localWave->displacementState);
        handles.localWaveGradient = graph.ImportTexture(
            "Ocean.LocalWave.Gradient",
            *localWave->gradient,
            localWave->gradientState);
        handles.localWaveDisturbanceUpload = graph.ImportBuffer(
            "Ocean.LocalWave.DisturbanceUpload",
            *localWave->disturbanceUpload,
            localWave->disturbanceUploadState);
        handles.localWaveDisturbance = graph.ImportBuffer(
            "Ocean.LocalWave.Disturbance",
            *localWave->disturbance,
            localWave->disturbanceState);
        handles.localWaveReadIndex = localWave->readIndex;
        handles.localWaveWriteIndex = localWave->writeIndex;
    }
    if (interactiveTerrainEnabled)
    {
        handles.terrainRawHeight = graph.ImportTexture(
            "TerrainRawHeight",
            *terrain->rawHeight,
            terrain->rawHeightInitialState);
        handles.terrainErodedHeight = graph.ImportTexture(
            "TerrainErodedHeight",
            *terrain->erodedHeight,
            terrain->erodedHeightInitialState);
    }
    for (std::uint32_t index = 0;
         index < SharedGBufferCount;
         ++index)
    {
        handles.gbuffer[index] =
            graph.DeclareTransientTexture(
                "GBuffer" + std::to_string(index),
                *resources.gbuffer[index],
                resources.gbufferInitialStates[index]);
    }
    handles.hdrColor =
        graph.DeclareTransientTexture(
            "HdrColor",
            *resources.hdrColor,
            resources.hdrInitialState);
    handles.bloomA =
        graph.DeclareTransientTexture(
            "BloomA",
            *resources.bloomA,
            resources.bloomAInitialState);
    handles.bloomB =
        graph.DeclareTransientTexture(
            "BloomB",
            *resources.bloomB,
            resources.bloomBInitialState);
    handles.outputColor = graph.ImportTexture(
        "OutputColor",
        *resources.outputColor,
        resources.outputInitialState);

    if (skyAtmosphereEnabled)
    {
        SkyAtmosphere::AddPasses(
            graph,
            handles.atmosphereTransmittance,
            handles.atmosphereSkyView,
            atmosphere->transmittanceExecute,
            atmosphere->skyViewExecute);
    }
    if (oceanSimulationEnabled)
    {
        if (execution.spectralOcean)
        {
            SpectralOceanGraphHandles spectralHandles{};
            spectralHandles.initialSpectrum = handles.oceanInitialSpectrum;
            spectralHandles.spectrumA = handles.oceanSpectrumA;
            spectralHandles.spectrumB = handles.oceanSpectrumB;
            spectralHandles.displacement = handles.oceanDisplacement;
            spectralHandles.gradientFoam = handles.oceanNormalFoam;
            spectralHandles.slopeMoments = handles.oceanSlopeMoments;
            spectralHandles.foamHistory = handles.oceanFoamHistory;
            spectralHandles.foamReadIndex = handles.oceanFoamReadIndex;
            spectralHandles.foamWriteIndex = handles.oceanFoamWriteIndex;
            SpectralOceanSimulation::AddPasses(graph, spectralHandles,
                spectralOcean->rebuildInitialSpectrum,
                spectralOcean->callbacks);
            handles.oceanInitialSpectrum = spectralHandles.initialSpectrum;
            handles.oceanSpectrumA = spectralHandles.spectrumA;
            handles.oceanSpectrumB = spectralHandles.spectrumB;
            handles.oceanDisplacement = spectralHandles.displacement;
            handles.oceanNormalFoam = spectralHandles.gradientFoam;
            handles.oceanSlopeMoments = spectralHandles.slopeMoments;
            handles.oceanFoamHistory = spectralHandles.foamHistory;
        }
        else
        {
            FftOcean::AddPasses(
                graph,
                handles.oceanSpectrumA,
                handles.oceanSpectrumB,
                handles.oceanDisplacement,
                handles.oceanNormalFoam,
                fftOcean->execute);
        }
        if (localWaveEnabled)
        {
            LocalWaveGraphHandles localHandles{};
            localHandles.height = handles.localWaveHeight;
            localHandles.velocity = handles.localWaveVelocity;
            localHandles.foam = handles.localWaveFoam;
            localHandles.displacement = handles.localWaveDisplacement;
            localHandles.gradient = handles.localWaveGradient;
            localHandles.disturbanceUpload = handles.localWaveDisturbanceUpload;
            localHandles.disturbance = handles.localWaveDisturbance;
            localHandles.readIndex = handles.localWaveReadIndex;
            localHandles.writeIndex = handles.localWaveWriteIndex;
            LocalWaveGpuResources::AddPasses(
                graph, localHandles, localWave->callbacks);
            handles.localWaveHeight = localHandles.height;
            handles.localWaveVelocity = localHandles.velocity;
            handles.localWaveFoam = localHandles.foam;
            handles.localWaveDisplacement = localHandles.displacement;
            handles.localWaveGradient = localHandles.gradient;
            handles.localWaveDisturbance = localHandles.disturbance;
        }
        if (effectiveOptions.oceanQueriesEnabled)
        {
            auto parameters = graph.CreatePassParameters();
            parameters.ReadBuffer(handles.oceanQueryConstants,
                RHI::ResourceState::ConstantBuffer);
            parameters.ReadBuffer(handles.oceanQueryPoints,
                RHI::ResourceState::ShaderResource);
            parameters.ReadTexture(handles.oceanDisplacement,
                RHI::ResourceState::ShaderResource);
            if (localWaveEnabled)
                parameters.ReadTexture(handles.localWaveDisplacement,
                    RHI::ResourceState::ShaderResource);
            handles.oceanQueryResults = parameters.WriteBuffer(
                handles.oceanQueryResults,
                RHI::ResourceState::UnorderedAccess);
            handles.oceanQueryReadback = parameters.WriteBuffer(
                handles.oceanQueryReadback,
                RHI::ResourceState::CopyDestination);
            graph.AddParameterPass("Ocean.Query", std::move(parameters),
                callbacks.oceanQuery,
                RenderGraph::PassOptions{RenderGraph::QueueClass::Compute,
                    true, false, false});
        }
    }
    if (interactiveTerrainUpdateEnabled)
    {
        InteractiveTerrain::AddPasses(
            graph,
            handles.terrainRawHeight,
            handles.terrainErodedHeight,
            terrain->execute);
    }

    if (execution.shadows)
    {
        RequireCallback(
            callbacks.shadow,
            "The shared render graph requires a Shadow callback.");
        auto parameters =
            graph.CreatePassParameters();
        ReadFrameConstants(
            parameters,
            handles.frameConstants);
        if (interactiveTerrainEnabled)
        {
            parameters.ReadTexture(
                handles.terrainErodedHeight,
                RHI::ResourceState::ShaderResource);
        }
        handles.shadowMap =
            parameters.WriteTexture(
                handles.shadowMap,
                RHI::ResourceState::DepthWrite);
        graph.AddParameterPass(
            "Shadow",
            std::move(parameters),
            callbacks.shadow);
    }

    if (execution.gpuDriven)
    {
        GpuDrivenVisibility::AddPasses(
            graph,
            handles.gpuObjectRecords,
            handles.hiZ,
            handles.indirectArguments,
            handles.indirectDrawCounts,
            gpuVisibility->execute);
    }

    if (varianceShadowsEnabled)
    {
        VarianceShadowMaps::AddPasses(
            graph,
            handles.shadowMap,
            handles.shadowMoments,
            handles.shadowMomentsScratch,
            varianceShadows.convert,
            varianceShadows.horizontal,
            varianceShadows.vertical);
    }

    if (planarReflectionsEnabled)
    {
        PlanarReflections::AddPasses(
            graph,
            handles.planarReflection,
            handles.planarReflectionDepth,
            planarReflections->execute,
            handles.environment);
    }

    if (clusteredLightingEnabled)
    {
        ClusteredLighting::AddPasses(
            graph,
            handles.clusterConstants,
            handles.pointLights,
            handles.clusterLightCounts,
            handles.clusterLightIndices,
            clusteredLighting->execute);
    }

    if (localLightShadowsEnabled)
    {
        LocalLightShadows::AddPasses(
            graph,
            handles.localShadowRenderConstants,
            handles.spotShadowMap,
            handles.pointShadowMap,
            execution.spotShadows,
            execution.pointShadows,
            localLightShadows.spotExecute,
            localLightShadows.pointExecute);
    }

    if (execution.deferredGeometry)
    {
        RequireCallback(
            callbacks.gbuffer,
            "The shared render graph requires a GBuffer callback.");
        auto gbufferParameters =
            graph.CreatePassParameters();
        ReadFrameConstants(
            gbufferParameters,
            handles.frameConstants);
        if (execution.gpuDriven)
        {
            gbufferParameters.ReadBuffer(
                handles.indirectArguments,
                RHI::ResourceState::
                    IndirectArgument);
            gbufferParameters.ReadBuffer(
                handles.indirectDrawCounts,
                RHI::ResourceState::IndirectArgument);
        }
        // Generic mesh shaders retain the shared ocean descriptors even when
        // HPWater draws are excluded; their descriptor layouts must be valid.
        if (oceanFeatureEnabled)
        {
            gbufferParameters.ReadTexture(
                handles.oceanDisplacement,
                RHI::ResourceState::ShaderResource);
            gbufferParameters.ReadTexture(
                handles.oceanNormalFoam,
                RHI::ResourceState::ShaderResource);
            if (execution.spectralOcean)
            {
                gbufferParameters.ReadTexture(
                    handles.oceanSlopeMoments,
                    RHI::ResourceState::ShaderResource);
                gbufferParameters.ReadTexture(
                    handles.oceanFoamHistory[handles.oceanFoamWriteIndex],
                    RHI::ResourceState::ShaderResource);
                if (localWaveEnabled)
                {
                    gbufferParameters.ReadTexture(
                        handles.localWaveDisplacement,
                        RHI::ResourceState::ShaderResource);
                    gbufferParameters.ReadTexture(
                        handles.localWaveGradient,
                        RHI::ResourceState::ShaderResource);
                }
            }
        }
        if (interactiveTerrainEnabled)
        {
            gbufferParameters.ReadTexture(
                handles.terrainErodedHeight,
                RHI::ResourceState::ShaderResource);
        }
        for (TextureHandle& gbuffer :
             handles.gbuffer)
        {
            gbuffer =
                gbufferParameters.WriteTexture(
                    gbuffer,
                    RHI::ResourceState::RenderTarget);
        }
        handles.depthBuffer =
            gbufferParameters.WriteTexture(
                handles.depthBuffer,
                RHI::ResourceState::DepthWrite);
        handles.motionVectors =
            gbufferParameters.WriteTexture(
                handles.motionVectors,
                RHI::ResourceState::RenderTarget);
        graph.AddParameterPass(
            "GBuffer",
            std::move(gbufferParameters),
            callbacks.gbuffer);

        if (gtaoEnabled)
        {
            ScreenSpaceEffects::AddGtaoPasses(
                graph,
                handles.gbuffer[0],
                handles.gbuffer[1],
                handles.ambientOcclusion,
                screenSpaceEffects.gtao);
        }

        RequireCallback(
            callbacks.deferredLighting,
            "The shared render graph requires a DeferredLighting callback.");
        auto lightingParameters =
            graph.CreatePassParameters();
        if (handles.environment.IsValid())
            lightingParameters.ReadTexture(handles.environment, RHI::ResourceState::ShaderResource);
        ReadFrameConstants(
            lightingParameters,
            handles.frameConstants);
        if (clusteredLightingEnabled)
        {
            lightingParameters.ReadBuffer(
                handles.clusterConstants,
                RHI::ResourceState::ConstantBuffer);
            lightingParameters.ReadBuffer(
                handles.pointLights,
                RHI::ResourceState::ShaderResource);
            lightingParameters.ReadBuffer(
                handles.clusterLightCounts,
                RHI::ResourceState::ShaderResource);
            lightingParameters.ReadBuffer(
                handles.clusterLightIndices,
                RHI::ResourceState::ShaderResource);
        }
        lightingParameters.ReadBuffer(
            handles.localLightConstants,
            RHI::ResourceState::ConstantBuffer);
        // Runtime-disabled branches retain their sampled descriptors. Layouts
        // must be valid even when their contents will not be accessed.
        for (const auto texture : {handles.shadowMap, handles.spotShadowMap,
                 handles.pointShadowMap, handles.shadowMoments})
            lightingParameters.ReadTexture(texture, RHI::ResourceState::ShaderResource);
        if (gtaoEnabled)
        {
            lightingParameters.ReadTexture(
                handles.ambientOcclusion,
                RHI::ResourceState::ShaderResource);
        }
        if (skyAtmosphereEnabled)
        {
            lightingParameters.ReadTexture(
                handles.atmosphereSkyView,
                RHI::ResourceState::ShaderResource);
        }
        for (const TextureHandle gbuffer :
             handles.gbuffer)
        {
            lightingParameters.ReadTexture(
                gbuffer,
                RHI::ResourceState::ShaderResource);
        }
        handles.hdrColor =
            lightingParameters.WriteTexture(
                handles.hdrColor,
                RHI::ResourceState::RenderTarget);
        graph.AddParameterPass(
            "DeferredLighting",
            std::move(lightingParameters),
            callbacks.deferredLighting,
            RenderGraph::PassOptions{
                RenderGraph::QueueClass::Graphics,
                false,
                true,
                true,
                RenderGraph::ParallelRecordingContract::AuditedIndependent()});
    }
    else
    {
        RequireCallback(
            callbacks.forwardGeometry,
            "The shared render graph requires a ForwardGeometry callback.");
        auto parameters =
            graph.CreatePassParameters();
        if (handles.environment.IsValid())
            parameters.ReadTexture(handles.environment, RHI::ResourceState::ShaderResource);
        ReadFrameConstants(
            parameters,
            handles.frameConstants);
        if (clusteredLightingEnabled)
        {
            parameters.ReadBuffer(
                handles.clusterConstants,
                RHI::ResourceState::ConstantBuffer);
            parameters.ReadBuffer(
                handles.pointLights,
                RHI::ResourceState::ShaderResource);
            parameters.ReadBuffer(
                handles.clusterLightCounts,
                RHI::ResourceState::ShaderResource);
            parameters.ReadBuffer(
                handles.clusterLightIndices,
                RHI::ResourceState::ShaderResource);
        }
        if (execution.gpuDriven)
        {
            parameters.ReadBuffer(
                handles.indirectArguments,
                RHI::ResourceState::
                    IndirectArgument);
            parameters.ReadBuffer(
                handles.indirectDrawCounts,
                RHI::ResourceState::IndirectArgument);
        }
        if (oceanFeatureEnabled)
        {
            parameters.ReadTexture(
                handles.oceanDisplacement,
                RHI::ResourceState::ShaderResource);
            parameters.ReadTexture(
                handles.oceanNormalFoam,
                RHI::ResourceState::ShaderResource);
            if (execution.spectralOcean)
            {
                parameters.ReadTexture(
                    handles.oceanSlopeMoments,
                    RHI::ResourceState::ShaderResource);
                parameters.ReadTexture(
                    handles.oceanFoamHistory[handles.oceanFoamWriteIndex],
                    RHI::ResourceState::ShaderResource);
                if (localWaveEnabled)
                {
                    parameters.ReadTexture(
                        handles.localWaveDisplacement,
                        RHI::ResourceState::ShaderResource);
                    parameters.ReadTexture(
                        handles.localWaveGradient,
                        RHI::ResourceState::ShaderResource);
                }
            }
        }
        if (interactiveTerrainEnabled)
        {
            parameters.ReadTexture(
                handles.terrainErodedHeight,
                RHI::ResourceState::ShaderResource);
        }
        for (const auto texture : {handles.shadowMap, handles.spotShadowMap,
                 handles.pointShadowMap, handles.shadowMoments})
            parameters.ReadTexture(texture, RHI::ResourceState::ShaderResource);
        if (skyAtmosphereEnabled)
        {
            parameters.ReadTexture(
                handles.atmosphereSkyView,
                RHI::ResourceState::ShaderResource);
        }
        handles.hdrColor =
            parameters.WriteTexture(
                handles.hdrColor,
                RHI::ResourceState::RenderTarget);
        handles.depthBuffer =
            parameters.WriteTexture(
                handles.depthBuffer,
                RHI::ResourceState::DepthWrite);
        graph.AddParameterPass(
            "ForwardGeometry",
            std::move(parameters),
            callbacks.forwardGeometry);
    }

    if (waterVisibilityEnabled)
    {
        AddHiZPasses(graph,
            handles.depthBuffer,
            handles.hiZ,
            *resources.hiZ,
            callbacks.hiZ);
        WaterOpticsGraphInputs inputs{};
        inputs.frameConstants = handles.frameConstants;
        inputs.clusterConstants = handles.clusterConstants;
        inputs.pointLights = handles.pointLights;
        inputs.clusterLightCounts = handles.clusterLightCounts;
        inputs.clusterLightIndices = handles.clusterLightIndices;
        inputs.sceneColor = handles.hdrColor;
        inputs.sceneMotion = handles.motionVectors;
        inputs.opaqueDepth = handles.depthBuffer;
        inputs.opaqueHiZ = handles.hiZ;
        inputs.shadowMap = handles.shadowMap;
        inputs.shadowMoments = handles.shadowMoments;
        inputs.atmosphereSkyView = handles.atmosphereSkyView;
        inputs.environment = handles.environment;
        inputs.waterGBuffer = handles.waterGBuffer;
        inputs.waterMotion = handles.waterMotion;
        inputs.compositeDepth = handles.waterCompositeDepth;
        inputs.oceanDisplacement = handles.oceanDisplacement;
        inputs.oceanNormalFoam = handles.oceanNormalFoam;
        inputs.oceanSlopeMoments = handles.oceanSlopeMoments;
        inputs.oceanFoamHistory =
            handles.oceanFoamHistory[handles.oceanFoamWriteIndex];
        inputs.localWaveDisplacement = handles.localWaveDisplacement;
        inputs.localWaveGradient = handles.localWaveGradient;
        inputs.clusteredLightingEnabled = clusteredLightingEnabled;
        inputs.shadowsEnabled = execution.shadows;
        inputs.varianceShadowsEnabled = varianceShadowsEnabled;
        inputs.atmosphereEnabled = skyAtmosphereEnabled;
        inputs.localWaveEnabled = localWaveEnabled;
        const WaterOpticsGraphResult result =
            waterOptics->build(graph, inputs);
        Core::Check(result.sceneColor.IsValid()
                && result.compositeDepth.IsValid()
                && result.waterMotion.IsValid(),
            "Water optics returned incomplete graph outputs.");
        handles.hdrColor = result.sceneColor;
        handles.motionVectors = result.sceneMotion;
        handles.waterCompositeDepth = result.compositeDepth;
        handles.waterGBuffer = result.waterGBuffer;
        handles.waterMotion = result.waterMotion;
    }

    RequireCallback(
        callbacks.transparentGeometry,
        "The shared render graph requires a TransparentGeometry callback.");
    auto transparentParameters =
        graph.CreatePassParameters();
    if (handles.environment.IsValid())
        transparentParameters.ReadTexture(handles.environment, RHI::ResourceState::ShaderResource);
    ReadFrameConstants(
        transparentParameters,
        handles.frameConstants);
    if (clusteredLightingEnabled)
    {
        transparentParameters.ReadBuffer(
            handles.clusterConstants,
            RHI::ResourceState::ConstantBuffer);
        transparentParameters.ReadBuffer(
            handles.pointLights,
            RHI::ResourceState::ShaderResource);
        transparentParameters.ReadBuffer(
            handles.clusterLightCounts,
            RHI::ResourceState::ShaderResource);
        transparentParameters.ReadBuffer(
            handles.clusterLightIndices,
            RHI::ResourceState::ShaderResource);
    }
    if (execution.gpuDriven)
    {
        transparentParameters.ReadBuffer(
            handles.indirectArguments,
            RHI::ResourceState::IndirectArgument);
        transparentParameters.ReadBuffer(
            handles.indirectDrawCounts,
            RHI::ResourceState::IndirectArgument);
    }
    if (execution.shadows)
    {
        transparentParameters.ReadTexture(
            handles.shadowMap,
            RHI::ResourceState::ShaderResource);
    }
    if (localLightShadowsEnabled)
    {
        transparentParameters.ReadTexture(
            handles.spotShadowMap,
            RHI::ResourceState::ShaderResource);
        transparentParameters.ReadTexture(
            handles.pointShadowMap,
            RHI::ResourceState::ShaderResource);
    }
    if (varianceShadowsEnabled)
    {
        transparentParameters.ReadTexture(
            handles.shadowMoments,
            RHI::ResourceState::ShaderResource);
    }
    // Transparent rendering loads the opaque HDR and depth attachments before
    // producing new versions. These reads keep the opaque producer passes live
    // and make the load dependency explicit to the graph compiler.
    transparentParameters.ReadTexture(
        handles.hdrColor,
        RHI::ResourceState::RenderTarget);
    TextureHandle& transparentDepth = waterVisibilityEnabled
        ? handles.waterCompositeDepth
        : handles.depthBuffer;
    transparentParameters.ReadTexture(
        transparentDepth,
        RHI::ResourceState::DepthWrite);
    handles.hdrColor =
        transparentParameters.WriteTexture(
            handles.hdrColor,
            RHI::ResourceState::RenderTarget);
    transparentDepth =
        transparentParameters.WriteTexture(
            transparentDepth,
            RHI::ResourceState::DepthWrite);
    graph.AddParameterPass(
        "TransparentGeometry",
        std::move(transparentParameters),
        callbacks.transparentGeometry);

    TextureHandle postProcessColor = handles.hdrColor;
    if (hiZEnabled && !waterVisibilityEnabled)
    {
        AddHiZPasses(graph,
            handles.depthBuffer,
            handles.hiZ,
            *resources.hiZ,
            callbacks.hiZ);
    }

    if (screenSpaceCompositeEnabled)
    {
        ScreenSpaceEffects::AddCompositePasses(
            graph,
            handles.hdrColor,
            handles.hiZ,
            handles.gbuffer[0],
            handles.gbuffer[1],
            handles.ambientOcclusion,
            handles.planarReflection,
            gtaoEnabled,
            planarReflectionsEnabled,
            handles.screenSpaceColor,
            screenSpaceEffects.reflections);
        postProcessColor = handles.screenSpaceColor;
    }

    if (options.fluidEnabled)
    {
        const FluidGraphContribution& fluid =
            graph.GetBlackboard().Require<
                FluidFeatureSlot,
                FluidGraphContribution>({
                    "SharedRenderGraphFrontend",
                    options.viewId,
                    graph.GetResourceGeneration(),
                    1u,
                    "Fluid.Contribution"});
        Core::Check(static_cast<bool>(fluid.build),
            "The shared render graph requires a Fluid contribution when fluid rendering is enabled.");
        FluidGraphInputs inputs{};
        inputs.sceneColor = postProcessColor;
        inputs.sceneDepth = waterVisibilityEnabled
            ? handles.waterCompositeDepth
            : handles.depthBuffer;
        inputs.environment = handles.environment;
        const FluidGraphResult result = fluid.build(graph, inputs);
        Core::Check(
            result.sceneColor.IsValid(),
            "Fluid returned an invalid scene-color output.");
        postProcessColor = result.sceneColor;
    }

    if (temporalAntiAliasingEnabled)
    {
        postProcessColor =
            TemporalAntiAliasing::AddPasses(
                graph,
                postProcessColor,
                handles.motionVectors,
                handles.temporalHistoryRead,
                handles.temporalResolved,
                handles.temporalHistoryWrite,
                temporalAntiAliasing.execute);
    }

    if (execution.bloom)
    {
        RequireCallback(
            callbacks.bloomExtract,
            "The shared render graph requires a BloomExtract callback.");
        auto extractParameters =
            graph.CreatePassParameters();
        extractParameters.ReadTexture(
            postProcessColor,
            RHI::ResourceState::ShaderResource);
        handles.bloomA =
            extractParameters.WriteTexture(
                handles.bloomA,
                RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass(
            "BloomExtract",
            std::move(extractParameters),
            callbacks.bloomExtract,
            RenderGraph::PassOptions{
                RenderGraph::QueueClass::Compute,
                false,
                true,
                true,
                RenderGraph::ParallelRecordingContract::AuditedIndependent()});

        RequireCallback(
            callbacks.bloomHorizontal,
            "The shared render graph requires a BloomHorizontal callback.");
        auto horizontalParameters =
            graph.CreatePassParameters();
        horizontalParameters.ReadTexture(
            handles.bloomA,
            RHI::ResourceState::ShaderResource);
        handles.bloomB =
            horizontalParameters.WriteTexture(
                handles.bloomB,
                RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass(
            "BloomHorizontal",
            std::move(horizontalParameters),
            callbacks.bloomHorizontal,
            RenderGraph::PassOptions{
                RenderGraph::QueueClass::Compute,
                false,
                true,
                true,
                RenderGraph::ParallelRecordingContract::AuditedIndependent()});

        RequireCallback(
            callbacks.bloomVertical,
            "The shared render graph requires a BloomVertical callback.");
        auto verticalParameters =
            graph.CreatePassParameters();
        verticalParameters.ReadTexture(
            handles.bloomB,
            RHI::ResourceState::ShaderResource);
        handles.bloomA =
            verticalParameters.WriteTexture(
                handles.bloomA,
                RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass(
            "BloomVertical",
            std::move(verticalParameters),
            callbacks.bloomVertical,
            RenderGraph::PassOptions{
                RenderGraph::QueueClass::Compute,
                false,
                true,
                true,
                RenderGraph::ParallelRecordingContract::AuditedIndependent()});
    }

    RequireCallback(
        callbacks.tonemap,
        "The shared render graph requires a Tonemap callback.");
    auto tonemapParameters =
        graph.CreatePassParameters();
    tonemapParameters.ReadTexture(
        postProcessColor,
        RHI::ResourceState::ShaderResource);
    if (execution.bloom)
    {
        tonemapParameters.ReadTexture(
            handles.bloomA,
            RHI::ResourceState::ShaderResource);
    }
    handles.outputColor =
        tonemapParameters.WriteTexture(
            handles.outputColor,
            RHI::ResourceState::RenderTarget);
    graph.AddParameterPass(
        "Tonemap",
        std::move(tonemapParameters),
        callbacks.tonemap);
    graph.MarkOutput(handles.outputColor);

    auto readyParameters =
        graph.CreatePassParameters();
    readyParameters.ReadTexture(
        handles.outputColor,
        options.outputReadyState);
    graph.AddParameterPass(
        "OutputReady",
        std::move(readyParameters),
        callbacks.outputReady
            ? callbacks.outputReady
            : [](RHI::ICommandContext&,
                 const RenderGraphPassResources&) {},
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Graphics,
            true,
            false});

    graph.GetBlackboard().Set(handles);
    return handles;
}
} // namespace Prism::Renderer
