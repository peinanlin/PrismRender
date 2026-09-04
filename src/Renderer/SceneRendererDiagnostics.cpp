#include "Renderer/SceneRenderer.h"

#include "Renderer/FrameDiagnostics.h"
#include "Renderer/RenderGraphDiagnostics.h"
#include "Renderer/SceneRendererSharedResources.h"

#include <stdexcept>
#include <string>

namespace Prism::Renderer
{
namespace
{
const char* ToString(const ScenePipelineRejectionReason reason)
{
    switch (reason)
    {
    case ScenePipelineRejectionReason::None:
        return "none";
    case ScenePipelineRejectionReason::CapabilityUnsupported:
        return "capability_unsupported";
    case ScenePipelineRejectionReason::PolicyDisabled:
        return "policy_disabled";
    case ScenePipelineRejectionReason::StaticContentMissing:
        return "static_content_missing";
    case ScenePipelineRejectionReason::DynamicContentMissing:
        return "dynamic_content_missing";
    case ScenePipelineRejectionReason::PendingWorkMissing:
        return "pending_work_missing";
    case ScenePipelineRejectionReason::ProducerViewRequired:
        return "producer_view_required";
    }
    return "unknown";
}

nlohmann::json DecisionJson(
    const ScenePipelineActivationDecision& decision)
{
    return {
        {"active", decision.active},
        {"rejectionReason", ToString(decision.rejectionReason)}};
}
} // namespace

RenderViewDiagnostics SceneRenderer::BuildFrameDiagnostics(const RHI::GraphicsApi api) const
{
    if (!m_frameDiagnosticsEnabled)
        throw std::logic_error("Frame diagnostics were not enabled for this renderer.");
    auto counts = nlohmann::json::object();
    auto simulationCounts = nlohmann::json::object();
    for (const std::string& name : m_diagnosticRecordedPasses)
    {
        counts[name] = counts.value(name, 0u) + 1u;
        if (name.starts_with("SpectralOcean.") || name.starts_with("FftOcean.")
            || name.starts_with("Ocean.LocalWave.") || name == "PbfFluid.Simulate"
            || name == "InteractiveTerrain.BrushAndErosion")
            simulationCounts[name] = simulationCounts.value(name, 0u) + 1u;
    }
    const auto& optical = m_waterOpticsFeature.GetHistoryState();
    const auto& key = optical.GetKey();
    const auto& volumetric = m_waterOpticsFeature.GetVolumetricHistoryState();
    nlohmann::json featureActivation = nullptr;
    if (m_lastPipelinePlan.has_value())
    {
        const ScenePipelinePlan& plan = *m_lastPipelinePlan;
        const Scene::RenderFrameFeatureUsage& usage =
            m_lastPipelineFrameState.usage;
        featureActivation = {
            {"capability", {
                {"gpuDriven", m_lastPipelineFrameState.gpuDrivenSupported},
                {"interactiveTerrain", true}}},
            {"policy", {
                {"directionalShadows", m_settings.shadowsEnabled},
                {"clusteredLighting", m_settings.clusteredLightingEnabled},
                {"localLightShadows", m_settings.localLightShadowsEnabled},
                {"gtao", m_settings.gtaoEnabled},
                {"screenSpaceReflections", m_settings.screenSpaceReflectionsEnabled},
                {"gpuDriven", m_settings.gpuDrivenEnabled},
                {"interactiveTerrain", m_settings.interactiveTerrainEnabled}}},
            {"staticUsage", {
                {"opaqueGeometry", usage.scene.hasOpaqueGeometry},
                {"transparentGeometry", usage.scene.hasTransparentGeometry},
                {"shadowCaster", usage.scene.hasShadowCaster},
                {"terrainSurface", usage.scene.hasTerrainSurface},
                {"gpuDrivenCandidate", usage.scene.hasGpuDrivenCandidate}}},
            {"dynamicUsage", {
                {"directionalLight", usage.hasDirectionalLight},
                {"pointLights", usage.hasPointLights},
                {"spotLights", usage.hasSpotLights},
                {"shadowedPointLights", usage.hasShadowedPointLights},
                {"shadowedSpotLights", usage.hasShadowedSpotLights}}},
            {"terrainState", {
                {"pendingWork", m_lastPipelineFrameState.terrainPendingWork},
                {"producerView", m_lastPipelineFrameState.sharedSimulationProducer},
                {"gpuInitialized", m_sharedResources
                    ->IsInteractiveTerrainInitialized()}}},
            {"decisions", {
                {"directionalShadows", DecisionJson(plan.shadowsDecision)},
                {"clusteredLighting", DecisionJson(plan.clusteredLightingDecision)},
                {"spotShadows", DecisionJson(plan.spotShadowsDecision)},
                {"pointShadows", DecisionJson(plan.pointShadowsDecision)},
                {"gtao", DecisionJson(plan.gtaoDecision)},
                {"screenSpaceReflections", DecisionJson(plan.screenSpaceReflectionsDecision)},
                {"gpuDriven", DecisionJson(plan.gpuDrivenDecision)},
                {"terrainSampling", DecisionJson(plan.terrainSamplingDecision)},
                {"terrainUpdate", DecisionJson(plan.terrainUpdateDecision)}}}};
    }
    return {{{"backend", std::string(RHI::ToString(api))},
        {"outputTarget", m_renderToSwapChain ? "swapchain" : "view-texture"},
        {"width", GetFinalOutputWidth()}, {"height", GetFinalOutputHeight()},
        {"logicalFrameId", m_logicalFrameId},
        {"simulationProducer", m_sharedSimulationProducerThisFrame},
        {"passCountMeaning", "completed-command-recording-or-replay-not-gpu-completion"},
        {"recordedPasses", m_diagnosticRecordedPasses}, {"recordedPassCounts", counts},
        {"sharedSimulationPassCounts", simulationCounts},
        {"featureActivation", std::move(featureActivation)},
        {"settings", {{"taaEnabled", m_settings.temporalAntiAliasingEnabled},
            {"shadowsEnabled", m_settings.shadowsEnabled}}},
        {"history", {
            {"observationPoint", "end-render-before-capture"},
            {"taa", {{"valid", m_temporalAntiAliasing.IsHistoryValid()},
                {"readIndex", m_temporalAntiAliasing.GetHistoryReadIndex()},
                {"resetCallCount", m_temporalAntiAliasing.GetHistoryResetCallCount()}}},
            {"waterOptics", {{"valid", optical.IsValid()}, {"version", optical.GetVersion()},
                {"lastInvalidationBits", static_cast<std::uint32_t>(optical.GetLastInvalidation())},
                {"key", {{"width", key.width}, {"height", key.height},
                    {"quality", static_cast<std::uint32_t>(key.quality)},
                    {"cameraCutVersion", key.cameraCutVersion}, {"sceneVersion", key.sceneVersion},
                    {"surfaceHistoryVersion", key.surfaceHistoryVersion},
                    {"explicitResetSerial", key.explicitResetSerial}}}}},
            {"waterVolumetric", {{"valid", volumetric.IsValid()}, {"version", volumetric.GetVersion()}}},
            {"oceanResetRequests", {{"full", m_settings.ocean.debug.fullResetSerial},
                {"history", m_settings.ocean.debug.historyResetSerial},
                {"local", m_settings.ocean.debug.localResetSerial}}}}},
        {"graphStateMeaning", "logical-rdg-final-state-and-declared-accesses-not-driver-validation"},
        {"graph", BuildRenderGraphReport(m_renderGraph, api)}}};
}
} // namespace Prism::Renderer
