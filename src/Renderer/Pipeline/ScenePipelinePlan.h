#pragma once

#include "Renderer/RenderSettings.h"
#include "Renderer/SharedRenderGraphFrontend.h"
#include "Scene/RenderFramePacket.h"

namespace Prism::Renderer
{
enum class ScenePipelineRejectionReason
{
    None,
    CapabilityUnsupported,
    PolicyDisabled,
    StaticContentMissing,
    DynamicContentMissing,
    PendingWorkMissing,
    ProducerViewRequired
};

struct ScenePipelineActivationDecision
{
    bool active = false;
    ScenePipelineRejectionReason rejectionReason =
        ScenePipelineRejectionReason::None;
};

struct ScenePipelineFrameState
{
    Scene::RenderFrameFeatureUsage usage{
        .scene = {
            .hasOpaqueGeometry = true,
            .hasTransparentGeometry = true,
            .hasShadowCaster = true,
            .hasTerrainSurface = true,
            .hasGpuDrivenCandidate = true},
        .hasDirectionalLight = true,
        .hasPointLights = true,
        .hasSpotLights = true,
        .hasShadowedPointLights = true,
        .hasShadowedSpotLights = true};
    bool terrainPendingWork = true;
    bool sharedSimulationProducer = true;
    bool gpuDrivenSupported = true;
};

// The resolved form of the user-facing render settings.  Code that needs to
// know whether a pass actually ran should consume this plan instead of
// duplicating feature dependency checks.
struct ScenePipelineExecution
{
    bool shadows = false;
    bool deferredGeometry = false;
    bool forwardGeometry = false;
    bool clusteredLighting = false;
    bool localLightShadows = false;
    bool spotShadows = false;
    bool pointShadows = false;
    bool planarReflections = false;
    bool varianceShadows = false;
    bool gtao = false;
    bool screenSpaceComposite = false;
    bool temporalAntiAliasing = false;
    bool skyAtmosphere = false;
    bool fftOcean = false;
    bool spectralOcean = false;
    bool hpWaterOptics = false;
    bool opaqueGeometryIncludesOcean = true;
    bool waterVisibility = false;
    bool fluid = false;
    bool hiZ = false;
    bool bloom = false;
    bool gpuDriven = false;
};

struct ScenePipelinePlan
{
    SharedRenderGraphOptions graphOptions;
    ScenePipelineExecution execution;
    ScenePipelineActivationDecision shadowsDecision;
    ScenePipelineActivationDecision clusteredLightingDecision;
    ScenePipelineActivationDecision spotShadowsDecision;
    ScenePipelineActivationDecision pointShadowsDecision;
    ScenePipelineActivationDecision gtaoDecision;
    ScenePipelineActivationDecision screenSpaceReflectionsDecision;
    ScenePipelineActivationDecision gpuDrivenDecision;
    ScenePipelineActivationDecision terrainSamplingDecision;
    ScenePipelineActivationDecision terrainUpdateDecision;
};

[[nodiscard]] ScenePipelineExecution
ResolveScenePipelineExecution(
    const SharedRenderGraphOptions& options);

[[nodiscard]] ScenePipelinePlan BuildScenePipelinePlan(
    const RenderSettings& settings,
    const ScenePipelineFrameState& frameState,
    RHI::ResourceState outputReadyState);

[[nodiscard]] ScenePipelinePlan BuildScenePipelinePlan(
    const RenderSettings& settings,
    RHI::ResourceState outputReadyState);
} // namespace Prism::Renderer
