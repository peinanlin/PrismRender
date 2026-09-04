#include "Renderer/Pipeline/ScenePipelinePlan.h"

namespace Prism::Renderer
{
namespace
{
ScenePipelineActivationDecision ResolveActivation(
    const bool policyEnabled,
    const bool staticApplicable,
    const bool dynamicApplicable = true,
    const bool capabilitySupported = true)
{
    if (!capabilitySupported)
        return {false, ScenePipelineRejectionReason::CapabilityUnsupported};
    if (!policyEnabled)
        return {false, ScenePipelineRejectionReason::PolicyDisabled};
    if (!staticApplicable)
        return {false, ScenePipelineRejectionReason::StaticContentMissing};
    if (!dynamicApplicable)
        return {false, ScenePipelineRejectionReason::DynamicContentMissing};
    return {true, ScenePipelineRejectionReason::None};
}
} // namespace

ScenePipelineExecution ResolveScenePipelineExecution(
    const SharedRenderGraphOptions& options)
{
    ScenePipelineExecution execution{};
    execution.shadows = options.shadowsEnabled;
    execution.deferredGeometry =
        options.deferredRenderingEnabled;
    execution.forwardGeometry =
        !options.deferredRenderingEnabled;
    execution.clusteredLighting =
        options.clusteredLightingEnabled;
    execution.localLightShadows =
        execution.deferredGeometry
        && options.localLightShadowsEnabled;
    execution.spotShadows = execution.localLightShadows
        && options.spotShadowsEnabled;
    execution.pointShadows = execution.localLightShadows
        && options.pointShadowsEnabled;
    execution.planarReflections =
        execution.deferredGeometry
        && options.planarReflectionsEnabled;
    execution.varianceShadows =
        execution.shadows
        && options.varianceShadowsEnabled;
    execution.gtao =
        execution.deferredGeometry
        && options.gtaoEnabled;
    execution.screenSpaceComposite =
        execution.deferredGeometry
        && (options.screenSpaceReflectionsEnabled
            || execution.planarReflections);
    execution.temporalAntiAliasing =
        execution.deferredGeometry
        && options.temporalAntiAliasingEnabled
        // Fluid has no motion-vector/reactive-mask output yet. Running TAA
        // would jitter scene depth independently and smear moving particles.
        && !options.fluidEnabled;
    execution.skyAtmosphere =
        options.physicalAtmosphereEnabled;
    execution.fftOcean = options.fftOceanEnabled
        && options.oceanImplementation == OceanImplementation::LegacyFft;
    execution.spectralOcean = options.spectralOceanEnabled
        && options.oceanImplementation == OceanImplementation::SpectralOcean;
    execution.hpWaterOptics = execution.spectralOcean
        && options.oceanOpticsModel == OceanOpticsModel::HpWater;
    execution.waterVisibility = execution.hpWaterOptics;
    execution.opaqueGeometryIncludesOcean = !execution.hpWaterOptics;
    execution.fluid = options.fluidEnabled;
    execution.gpuDriven = options.gpuDrivenEnabled;
    execution.hiZ =
        execution.gpuDriven
        || execution.screenSpaceComposite
        || execution.hpWaterOptics;
    execution.bloom = options.bloomEnabled;
    return execution;
}

ScenePipelinePlan BuildScenePipelinePlan(
    const RenderSettings& settings,
    const ScenePipelineFrameState& frameState,
    const RHI::ResourceState outputReadyState)
{
    ScenePipelinePlan plan{};
    const Scene::RenderFrameFeatureUsage& usage = frameState.usage;
    plan.shadowsDecision = ResolveActivation(
        settings.shadowsEnabled,
        usage.scene.hasShadowCaster,
        usage.hasDirectionalLight);
    plan.clusteredLightingDecision = ResolveActivation(
        settings.clusteredLightingEnabled
            && (settings.deferredRenderingEnabled
                || settings.forwardPlusEnabled),
        true,
        usage.hasPointLights || usage.hasSpotLights);
    plan.spotShadowsDecision = ResolveActivation(
        settings.shadowsEnabled
            && settings.localLightShadowsEnabled
            && settings.spotLightsEnabled
            && settings.deferredRenderingEnabled,
        usage.scene.hasShadowCaster,
        usage.hasShadowedSpotLights);
    plan.pointShadowsDecision = ResolveActivation(
        settings.shadowsEnabled
            && settings.localLightShadowsEnabled
            && settings.pointLightsEnabled
            && settings.deferredRenderingEnabled,
        usage.scene.hasShadowCaster,
        usage.hasShadowedPointLights);
    plan.gtaoDecision = ResolveActivation(
        settings.gtaoEnabled && settings.deferredRenderingEnabled,
        usage.scene.hasOpaqueGeometry);
    plan.screenSpaceReflectionsDecision = ResolveActivation(
        settings.screenSpaceReflectionsEnabled
            && settings.deferredRenderingEnabled,
        usage.scene.hasOpaqueGeometry);
    plan.gpuDrivenDecision = ResolveActivation(
        settings.gpuDrivenEnabled,
        usage.scene.hasGpuDrivenCandidate,
        true,
        frameState.gpuDrivenSupported);
    plan.terrainSamplingDecision = ResolveActivation(
        settings.interactiveTerrainEnabled,
        usage.scene.hasTerrainSurface);
    plan.terrainUpdateDecision = plan.terrainSamplingDecision;
    if (plan.terrainUpdateDecision.active
        && !frameState.sharedSimulationProducer)
    {
        plan.terrainUpdateDecision = {
            false,
            ScenePipelineRejectionReason::ProducerViewRequired};
    }
    else if (plan.terrainUpdateDecision.active
        && !frameState.terrainPendingWork)
    {
        plan.terrainUpdateDecision = {
            false,
            ScenePipelineRejectionReason::PendingWorkMissing};
    }
    SharedRenderGraphOptions& options =
        plan.graphOptions;
    options.shadowsEnabled = plan.shadowsDecision.active;
    options.deferredRenderingEnabled =
        settings.deferredRenderingEnabled;
    options.clusteredLightingEnabled =
        plan.clusteredLightingDecision.active;
    options.spotShadowsEnabled =
        plan.spotShadowsDecision.active;
    options.pointShadowsEnabled =
        plan.pointShadowsDecision.active;
    options.localLightShadowsEnabled =
        options.spotShadowsEnabled
        || options.pointShadowsEnabled;
    options.planarReflectionsEnabled =
        settings.planarReflectionsEnabled;
    options.varianceShadowsEnabled =
        settings.shadowsEnabled
        && (settings.shadowFilterMode
                == ShadowFilterMode::Vsm
            || settings.shadowFilterMode
                == ShadowFilterMode::Evsm);
    options.gtaoEnabled = plan.gtaoDecision.active;
    options.screenSpaceReflectionsEnabled =
        plan.screenSpaceReflectionsDecision.active;
    options.temporalAntiAliasingEnabled =
        settings.temporalAntiAliasingEnabled;
    options.physicalAtmosphereEnabled =
        settings.physicalAtmosphereEnabled
        && settings.skyboxEnabled;
    const bool oceanEnabled = settings.ocean.debug.renderWater;
    options.oceanImplementation = settings.ocean.implementation;
    options.oceanOpticsModel = settings.ocean.opticsModel;
    options.fftOceanEnabled = oceanEnabled
        && settings.fftOceanEnabled;
    options.spectralOceanEnabled = oceanEnabled
        && settings.fftOceanEnabled;
    options.oceanSimulationEnabled = settings.ocean.debug.simulateWater;
    options.localWaveEnabled = options.spectralOceanEnabled
        && settings.ocean.local.enabled;
    options.interactiveTerrainEnabled =
        plan.terrainSamplingDecision.active;
    options.interactiveTerrainUpdateEnabled =
        plan.terrainUpdateDecision.active;
    options.fluidEnabled = settings.fluid.enabled;
    options.bloomEnabled = settings.bloomEnabled;
    options.gpuDrivenEnabled =
        plan.gpuDrivenDecision.active;
    options.outputReadyState = outputReadyState;
    plan.execution =
        ResolveScenePipelineExecution(options);
    return plan;
}

ScenePipelinePlan BuildScenePipelinePlan(
    const RenderSettings& settings,
    const RHI::ResourceState outputReadyState)
{
    return BuildScenePipelinePlan(
        settings,
        ScenePipelineFrameState{},
        outputReadyState);
}
} // namespace Prism::Renderer
