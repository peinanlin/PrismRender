#include "Renderer/Features/BuiltInRenderFeatures.h"

#include "Core/Assert.h"
#include "Renderer/Features/ClusteredLighting.h"
#include "Renderer/Features/FftOcean.h"
#include "Renderer/Features/Fluid/FluidFeature.h"
#include "Renderer/Features/GpuDrivenVisibility.h"
#include "Renderer/Features/InteractiveTerrain.h"
#include "Renderer/Features/LocalLightShadows.h"
#include "Renderer/Features/Ocean/LocalWaveGpuResources.h"
#include "Renderer/Features/Ocean/SpectralOceanSimulation.h"
#include "Renderer/Features/Ocean/WaterOpticsFeature.h"
#include "Renderer/Features/PlanarReflections.h"
#include "Renderer/Features/ScreenSpaceEffects.h"
#include "Renderer/Features/SkyAtmosphere.h"
#include "Renderer/Features/TemporalAntiAliasing.h"
#include "Renderer/Features/VarianceShadowMaps.h"
#include "Renderer/Features/VirtualTextureCache.h"

#include <string>
#include <utility>

namespace Prism::Renderer
{
void BuiltInRenderFeatures::RegisterSkyAtmosphere(
    RenderFeatureRegistry& registry,
    SkyAtmosphere& feature,
    std::function<void(const RenderFeatureFrameContext&)> prepareFrame)
{
    Core::Check(
        static_cast<bool>(prepareFrame),
        "SkyAtmosphere requires a frame preparation callback.");
    RenderFeatureRegistration registration{};
    registration.id = std::string(SkyAtmosphereId);
    registration.scope = RenderFeatureScope::ViewLocal;
    registration.stages = {
        RenderFeatureStage::FramePreparation};
    registration.requiredOperations = {
        RenderFeatureOperation::PrepareFrame,
        RenderFeatureOperation::BuildGraph};
    registration.operations.prepareFrame = std::move(prepareFrame);

    registration.operations.buildGraph =
        [&feature](RenderFeatureGraphContext& context)
        {
            context.graph.GetBlackboard().Publish<
                SkyAtmosphereFeatureSlot>(
                    feature.CreateRenderGraphContribution(
                        context.frameIndex),
                    {std::string(SkyAtmosphereId),
                     RenderGraphBlackboardValueScope::ViewLocal,
                     context.viewId,
                     context.graph.GetResourceGeneration(),
                     1u});
        };
    registry.Register(std::move(registration));
}

void BuiltInRenderFeatures::RegisterWaterOptics(
    RenderFeatureRegistry& registry,
    WaterOpticsFeature& feature,
    std::function<void(const RenderFeatureFrameContext&)> prepareFrame,
    std::function<WaterOpticsPassCallbacks(
        const RenderFeatureGraphContext&)> callbackFactory)
{
    Core::Check(
        static_cast<bool>(prepareFrame),
        "WaterOptics requires a frame preparation callback.");
    Core::Check(
        static_cast<bool>(callbackFactory),
        "WaterOptics requires a graph callback factory.");

    RenderFeatureRegistration registration{};
    registration.id = std::string(WaterOpticsId);
    registration.scope = RenderFeatureScope::ViewLocal;
    registration.stages = {RenderFeatureStage::WaterOptics};
    registration.requiredOperations = {
        RenderFeatureOperation::PrepareFrame,
        RenderFeatureOperation::BuildGraph,
        RenderFeatureOperation::Resize,
        RenderFeatureOperation::SceneChanged,
        RenderFeatureOperation::Shutdown};
    registration.operations.prepareFrame = std::move(prepareFrame);
    registration.operations.buildGraph =
        [&feature, callbackFactory = std::move(callbackFactory)](
            RenderFeatureGraphContext& context)
        {
            if (!feature.HasResources())
            {
                return;
            }
            context.graph.GetBlackboard().Publish<WaterOpticsFeatureSlot>(
                feature.CreateRenderGraphContribution(
                    callbackFactory(context)),
                {std::string(WaterOpticsId),
                 RenderGraphBlackboardValueScope::ViewLocal,
                 context.viewId,
                 context.graph.GetResourceGeneration(),
                 1u});
        };
    registration.operations.resize =
        [&feature](const RenderFeatureResizeContext& context)
        {
            feature.ReleaseSizeDependentResources(
                context.logicalFrameId,
                context.gpuIdle);
            feature.ResetHistory();
        };
    registration.operations.sceneChanged =
        [&feature](const RenderFeatureSceneContext&)
        {
            feature.NotifySceneChanged();
        };
    registration.operations.shutdown =
        [&feature]() { feature.Shutdown(); };
    registry.Register(std::move(registration));
}

void BuiltInRenderFeatures::RegisterFluid(
    RenderFeatureRegistry& registry,
    FluidFeature& feature)
{
    RenderFeatureRegistration registration{};
    registration.id = std::string(FluidId);
    registration.scope = RenderFeatureScope::ViewLocal;
    registration.stages = {RenderFeatureStage::PostProcess};
    registration.requiredOperations = {
        RenderFeatureOperation::BuildGraph};
    registration.operations.buildGraph =
        [&feature](RenderFeatureGraphContext& context)
        {
            context.graph.GetBlackboard().Publish<FluidFeatureSlot>(
                feature.CreateRenderGraphContribution(context.frameIndex),
                {std::string(FluidId),
                 RenderGraphBlackboardValueScope::ViewLocal,
                 context.viewId,
                 context.graph.GetResourceGeneration(),
                 1u});
        };
    registry.Register(std::move(registration));
}

void BuiltInRenderFeatures::RegisterClusteredLighting(
    RenderFeatureRegistry& registry,
    ClusteredLighting& feature)
{
    RenderFeatureRegistration registration{};
    registration.id = std::string(ClusteredLightingId);
    registration.scope = RenderFeatureScope::ViewLocal;
    registration.stages = {RenderFeatureStage::Lighting};
    registration.requiredOperations = {
        RenderFeatureOperation::BuildGraph,
        RenderFeatureOperation::Resize};
    registration.operations.resize =
        [featurePtr = &feature](const RenderFeatureResizeContext& context)
        {
            Core::Check(
                context.gpuIdle,
                "ClusteredLighting resize requires the renderer GPU-idle safe point.");
            featurePtr->Resize(context.width, context.height);
        };
    registration.operations.buildGraph =
        [featurePtr = &feature](RenderFeatureGraphContext& context)
        {
            context.graph.GetBlackboard().Publish<
                ClusteredLightingFeatureSlot>(
                    featurePtr->CreateRenderGraphContribution(
                        context.frameIndex),
                    {std::string(ClusteredLightingId),
                     RenderGraphBlackboardValueScope::ViewLocal,
                     context.viewId,
                     context.graph.GetResourceGeneration(),
                     1u});
        };
    registry.Register(std::move(registration));
}

void BuiltInRenderFeatures::RegisterLocalLightShadows(
    RenderFeatureRegistry& registry,
    LocalLightShadows& feature)
{
    RenderFeatureRegistration registration{};
    registration.id = std::string(LocalLightShadowsId);
    registration.scope = RenderFeatureScope::ViewLocal;
    registration.stages = {RenderFeatureStage::Shadows};
    registration.requiredOperations = {
        RenderFeatureOperation::BuildGraph};
    registration.operations.buildGraph =
        [featurePtr = &feature](RenderFeatureGraphContext& context)
        {
            std::shared_ptr<const Scene::RenderSceneView> scene =
                context.sceneLifetime != nullptr
                ? context.sceneLifetime
                : std::make_shared<const Scene::RenderSceneView>(
                      context.scene);
            context.graph.GetBlackboard().Publish<
                LocalLightShadowsFeatureSlot>(
                    featurePtr->CreateRenderGraphContribution(
                        context.frameIndex,
                        std::move(scene)),
                    {std::string(LocalLightShadowsId),
                     RenderGraphBlackboardValueScope::ViewLocal,
                     context.viewId,
                     context.graph.GetResourceGeneration(),
                     1u});
        };
    registry.Register(std::move(registration));
}

void BuiltInRenderFeatures::RegisterVarianceShadowMaps(
    RenderFeatureRegistry& registry,
    VarianceShadowMaps& feature)
{
    RenderFeatureRegistration registration{};
    registration.id = std::string(VarianceShadowMapsId);
    registration.scope = RenderFeatureScope::ViewLocal;
    registration.stages = {RenderFeatureStage::Shadows};
    registration.requiredOperations = {
        RenderFeatureOperation::BuildGraph};
    registration.operations.buildGraph =
        [featurePtr = &feature](RenderFeatureGraphContext& context)
        {
            context.graph.GetBlackboard().Publish<
                VarianceShadowMapsFeatureSlot>(
                    featurePtr->CreateRenderGraphContribution(
                        context.frameIndex),
                    {std::string(VarianceShadowMapsId),
                     RenderGraphBlackboardValueScope::ViewLocal,
                     context.viewId,
                     context.graph.GetResourceGeneration(),
                     1u});
        };
    registry.Register(std::move(registration));
}

void BuiltInRenderFeatures::RegisterPlanarReflections(
    RenderFeatureRegistry& registry,
    PlanarReflections& feature)
{
    RenderFeatureRegistration registration{};
    registration.id = std::string(PlanarReflectionsId);
    registration.scope = RenderFeatureScope::ViewLocal;
    registration.stages = {RenderFeatureStage::Shadows};
    registration.requiredOperations = {
        RenderFeatureOperation::BuildGraph,
        RenderFeatureOperation::Resize};
    registration.operations.resize =
        [featurePtr = &feature](const RenderFeatureResizeContext& context)
        {
            Core::Check(
                context.gpuIdle,
                "PlanarReflections resize requires the renderer GPU-idle safe point.");
            featurePtr->Resize(context.width, context.height);
        };
    registration.operations.buildGraph =
        [featurePtr = &feature](RenderFeatureGraphContext& context)
        {
            std::shared_ptr<const Scene::RenderSceneView> scene =
                context.sceneLifetime != nullptr
                ? context.sceneLifetime
                : std::make_shared<const Scene::RenderSceneView>(
                      context.scene);
            context.graph.GetBlackboard().Publish<
                PlanarReflectionsFeatureSlot>(
                    featurePtr->CreateRenderGraphContribution(
                        context.frameIndex,
                        std::move(scene)),
                    {std::string(PlanarReflectionsId),
                     RenderGraphBlackboardValueScope::ViewLocal,
                     context.viewId,
                     context.graph.GetResourceGeneration(),
                     1u});
        };
    registry.Register(std::move(registration));
}

void BuiltInRenderFeatures::RegisterScreenSpaceEffects(
    RenderFeatureRegistry& registry,
    ScreenSpaceEffects& feature,
    std::function<void(const RenderFeatureResizeContext&)> resize)
{
    Core::Check(
        static_cast<bool>(resize),
        "ScreenSpaceEffects requires a resize input callback.");
    RenderFeatureRegistration registration{};
    registration.id = std::string(ScreenSpaceEffectsId);
    registration.scope = RenderFeatureScope::ViewLocal;
    registration.stages = {RenderFeatureStage::PostProcess};
    registration.dependencies = {
        {std::string(PlanarReflectionsId), true}};
    registration.requiredOperations = {
        RenderFeatureOperation::BuildGraph,
        RenderFeatureOperation::Resize};
    registration.operations.resize = std::move(resize);
    registration.operations.buildGraph =
        [featurePtr = &feature](RenderFeatureGraphContext& context)
        {
            context.graph.GetBlackboard().Publish<
                ScreenSpaceEffectsFeatureSlot>(
                    featurePtr->CreateRenderGraphContribution(
                        context.frameIndex),
                    {std::string(ScreenSpaceEffectsId),
                     RenderGraphBlackboardValueScope::ViewLocal,
                     context.viewId,
                     context.graph.GetResourceGeneration(),
                     1u});
        };
    registry.Register(std::move(registration));
}

void BuiltInRenderFeatures::RegisterTemporalAntiAliasing(
    RenderFeatureRegistry& registry,
    TemporalAntiAliasing& feature,
    std::function<void(const RenderFeatureResizeContext&)> resize)
{
    Core::Check(
        static_cast<bool>(resize),
        "TemporalAntiAliasing requires a resize input callback.");
    RenderFeatureRegistration registration{};
    registration.id = std::string(TemporalAntiAliasingId);
    registration.scope = RenderFeatureScope::ViewLocal;
    registration.stages = {RenderFeatureStage::Temporal};
    registration.dependencies = {
        {std::string(ScreenSpaceEffectsId), true}};
    registration.requiredOperations = {
        RenderFeatureOperation::BuildGraph,
        RenderFeatureOperation::Resize,
        RenderFeatureOperation::SceneChanged};
    registration.operations.resize = std::move(resize);
    registration.operations.sceneChanged =
        [featurePtr = &feature](const RenderFeatureSceneContext&)
        {
            featurePtr->ResetHistory();
        };
    registration.operations.buildGraph =
        [featurePtr = &feature](RenderFeatureGraphContext& context)
        {
            context.graph.GetBlackboard().Publish<
                TemporalAntiAliasingFeatureSlot>(
                    featurePtr->CreateRenderGraphContribution(
                        context.frameIndex),
                    {std::string(TemporalAntiAliasingId),
                     RenderGraphBlackboardValueScope::ViewLocal,
                     context.viewId,
                     context.graph.GetResourceGeneration(),
                     1u});
        };
    registry.Register(std::move(registration));
}

void BuiltInRenderFeatures::RegisterFftOcean(
    RenderFeatureRegistry& registry,
    FftOcean& feature,
    std::function<bool()> selected)
{
    Core::Check(
        static_cast<bool>(selected),
        "FftOcean requires an implementation-selection predicate.");
    RenderFeatureRegistration registration{};
    registration.id = std::string(FftOceanId);
    registration.scope = RenderFeatureScope::ViewLocal;
    registration.stages = {RenderFeatureStage::FramePreparation};
    registration.requiredOperations = {
        RenderFeatureOperation::BuildGraph};

    registration.operations.buildGraph =
        [featurePtr = &feature, selected = std::move(selected)](
            RenderFeatureGraphContext& context)
        {
            if (selected())
            {
                context.graph.GetBlackboard().Publish<FftOceanFeatureSlot>(
                    featurePtr->CreateRenderGraphContribution(
                        context.frameIndex),
                    {std::string(FftOceanId),
                     RenderGraphBlackboardValueScope::ViewLocal,
                     context.viewId,
                     context.graph.GetResourceGeneration(),
                     1u});
            }
        };
    registry.Register(std::move(registration));
}

void BuiltInRenderFeatures::RegisterGpuDrivenVisibility(
    RenderFeatureRegistry& registry,
    GpuDrivenVisibility& feature,
    std::function<void(const RenderFeatureFrameContext&)> prepareFrame,
    std::function<void(const RenderFeatureResizeContext&)> resize)
{
    Core::Check(
        static_cast<bool>(prepareFrame),
        "GpuDrivenVisibility requires a frame preparation callback.");
    Core::Check(
        static_cast<bool>(resize),
        "GpuDrivenVisibility requires a resize input callback.");

    RenderFeatureRegistration registration{};
    registration.id = std::string(GpuDrivenVisibilityId);
    registration.scope = RenderFeatureScope::ViewLocal;
    registration.stages = {RenderFeatureStage::FramePreparation};
    registration.requiredOperations = {
        RenderFeatureOperation::PrepareFrame,
        RenderFeatureOperation::BuildGraph,
        RenderFeatureOperation::Resize,
        RenderFeatureOperation::SceneChanged};
    registration.operations.prepareFrame = std::move(prepareFrame);
    registration.operations.resize = std::move(resize);
    registration.operations.sceneChanged =
        [featurePtr = &feature](const RenderFeatureSceneContext&)
        {
            featurePtr->NotifySceneChanged();
        };

    registration.operations.buildGraph =
        [featurePtr = &feature](RenderFeatureGraphContext& context)
        {
            if (!context.gpuDrivenEnabled)
            {
                return;
            }
            context.graph.GetBlackboard().Publish<
                GpuDrivenVisibilityFeatureSlot>(
                    featurePtr->CreateRenderGraphContribution(
                        context.frameIndex),
                    {std::string(GpuDrivenVisibilityId),
                     RenderGraphBlackboardValueScope::ViewLocal,
                     context.viewId,
                     context.graph.GetResourceGeneration(),
                     1u});
        };
    registry.Register(std::move(registration));
}

void BuiltInRenderFeatures::RegisterSpectralOcean(
    RenderFeatureRegistry& registry,
    SpectralOceanSimulation& feature,
    std::function<bool()> selected)
{
    Core::Check(
        static_cast<bool>(selected),
        "SpectralOcean requires an implementation-selection predicate.");
    RenderFeatureRegistration registration{};
    registration.id = std::string(SpectralOceanId);
    registration.scope = RenderFeatureScope::DeviceShared;
    registration.stages = {RenderFeatureStage::FramePreparation};
    registration.requiredOperations = {
        RenderFeatureOperation::BuildGraph};

    // The registry borrows the instance. SceneRendererSharedResources remains
    // its sole owner and outlives both view registries.
    registration.operations.buildGraph =
        [featurePtr = &feature, selected = std::move(selected)](
            RenderFeatureGraphContext& context)
        {
            if (selected())
            {
                context.graph.GetBlackboard().Publish<
                    SpectralOceanFeatureSlot>(
                        featurePtr->CreateRenderGraphContribution(
                            context.frameIndex),
                        {std::string(SpectralOceanId),
                         RenderGraphBlackboardValueScope::DeviceShared,
                         0u,
                         context.graph.GetResourceGeneration(),
                         1u});
            }
        };
    registry.Register(std::move(registration));
}

void BuiltInRenderFeatures::RegisterLocalWave(
    RenderFeatureRegistry& registry,
    LocalWaveGpuResources& feature,
    std::function<bool()> selected)
{
    Core::Check(
        static_cast<bool>(selected),
        "LocalWave requires an implementation-selection predicate.");
    RenderFeatureRegistration registration{};
    registration.id = std::string(LocalWaveId);
    registration.scope = RenderFeatureScope::DeviceShared;
    registration.stages = {RenderFeatureStage::FramePreparation};
    registration.dependencies = {
        {std::string(SpectralOceanId), true}};
    registration.requiredOperations = {
        RenderFeatureOperation::BuildGraph};

    // Like the spectral simulation, this is a borrowed shared owner reference.
    registration.operations.buildGraph =
        [featurePtr = &feature, selected = std::move(selected)](
            RenderFeatureGraphContext& context)
        {
            if (selected())
            {
                context.graph.GetBlackboard().Publish<LocalWaveFeatureSlot>(
                    featurePtr->CreateRenderGraphContribution(
                        context.frameIndex),
                    {std::string(LocalWaveId),
                     RenderGraphBlackboardValueScope::DeviceShared,
                     0u,
                     context.graph.GetResourceGeneration(),
                     1u});
            }
        };
    registry.Register(std::move(registration));
}

void BuiltInRenderFeatures::RegisterInteractiveTerrain(
    RenderFeatureRegistry& registry,
    InteractiveTerrain& feature,
    std::function<void(const RenderFeatureFrameContext&)> prepareFrame,
    std::function<bool(const RenderFeatureGraphContext&)> active)
{
    Core::Check(
        static_cast<bool>(prepareFrame),
        "InteractiveTerrain requires a frame preparation callback.");
    RenderFeatureRegistration registration{};
    registration.id = std::string(InteractiveTerrainId);
    registration.scope = RenderFeatureScope::DeviceShared;
    registration.stages = {RenderFeatureStage::FramePreparation};
    registration.requiredOperations = {
        RenderFeatureOperation::PrepareFrame,
        RenderFeatureOperation::BuildGraph,
        RenderFeatureOperation::SceneChanged};
    registration.operations.prepareFrame = std::move(prepareFrame);
    registration.operations.sceneChanged =
        [featurePtr = &feature](const RenderFeatureSceneContext&)
        {
            featurePtr->RequestReset();
        };

    registration.operations.buildGraph =
        [featurePtr = &feature,
         active = std::move(active)](RenderFeatureGraphContext& context)
        {
            if (active && !active(context))
            {
                return;
            }
            context.graph.GetBlackboard().Publish<
                InteractiveTerrainFeatureSlot>(
                    featurePtr->CreateRenderGraphContribution(
                        context.frameIndex),
                    {std::string(InteractiveTerrainId),
                     RenderGraphBlackboardValueScope::DeviceShared,
                     0u,
                     context.graph.GetResourceGeneration(),
                     1u});
        };
    registry.Register(std::move(registration));
}

void BuiltInRenderFeatures::RegisterVirtualTextureCache(
    RenderFeatureRegistry& registry,
    VirtualTextureCache& feature,
    std::function<void(const RenderFeatureFrameContext&)> prepareFrame)
{
    Core::Check(
        static_cast<bool>(prepareFrame),
        "VirtualTextureCache requires a frame preparation callback.");
    RenderFeatureRegistration registration{};
    registration.id = std::string(VirtualTextureCacheId);
    registration.scope = RenderFeatureScope::ViewLocal;
    registration.stages = {RenderFeatureStage::FramePreparation};
    registration.requiredOperations = {
        RenderFeatureOperation::PrepareFrame,
        RenderFeatureOperation::SceneChanged};
    registration.operations.prepareFrame = std::move(prepareFrame);
    registration.operations.sceneChanged =
        [featurePtr = &feature](const RenderFeatureSceneContext&)
        {
            featurePtr->RequestReset();
        };
    registry.Register(std::move(registration));
}

} // namespace Prism::Renderer
