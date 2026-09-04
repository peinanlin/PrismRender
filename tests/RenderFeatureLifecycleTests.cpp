#include "Renderer/Features/RenderFeature.h"
#include "Renderer/Features/BuiltInRenderFeatures.h"
#include "Renderer/Features/ClusteredLighting.h"
#include "Renderer/Features/FftOcean.h"
#include "Renderer/Features/Fluid/FluidFeature.h"
#include "Renderer/Features/GpuDrivenVisibility.h"
#include "Renderer/Features/InteractiveTerrain.h"
#include "Renderer/Features/LocalLightShadows.h"
#include "Renderer/Features/Ocean/LocalWaveGpuResources.h"
#include "Renderer/Features/Ocean/SpectralOceanSimulation.h"
#include "Renderer/Features/PlanarReflections.h"
#include "Renderer/Features/ScreenSpaceEffects.h"
#include "Renderer/Features/SkyAtmosphere.h"
#include "Renderer/Features/TemporalAntiAliasing.h"
#include "Renderer/Features/VarianceShadowMaps.h"
#include "Renderer/Features/VirtualTextureCache.h"
#include "Renderer/RenderFeatureRegistry.h"
#include "Renderer/SharedRenderGraphFrontend.h"
#include "Scene/RenderScene.h"
#include "IndependentRenderFeatureFixture.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
using namespace Prism::Renderer;

void Expect(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <typename Callback>
void ExpectFailureContaining(
    Callback&& callback,
    const std::string& expectedText)
{
    try
    {
        std::forward<Callback>(callback)();
    }
    catch (const std::exception& exception)
    {
        Expect(
            std::string(exception.what()).find(expectedText)
                != std::string::npos,
            "Failure did not identify the expected registration problem.");
        return;
    }
    throw std::runtime_error("Expected lifecycle registration to fail.");
}

void TestRegistrationDescription()
{
    static_assert(!std::is_polymorphic_v<RenderFeatureRegistration>);

    RenderFeatureRegistration registration{};
    registration.id = "prism.spectral-ocean";
    registration.scope = RenderFeatureScope::DeviceShared;
    registration.stages = {
        RenderFeatureStage::FramePreparation,
        RenderFeatureStage::WaterOptics};
    registration.dependencies = {
        {"prism.shader-manager", true},
        {"prism.debug-overlay", false}};
    registration.requiredOperations = {
        RenderFeatureOperation::Initialize,
        RenderFeatureOperation::BuildGraph,
        RenderFeatureOperation::Shutdown};
    registration.initializationPrerequisites = {
        RenderFeatureInitializationPrerequisite::GraphicsDevice,
        RenderFeatureInitializationPrerequisite::ShaderManager,
        RenderFeatureInitializationPrerequisite::SharedResources};
    registration.operations.initialize =
        [](const RenderFeatureInitializationContext&) {};
    registration.operations.buildGraph =
        [](RenderFeatureGraphContext&) {};
    registration.operations.shutdown = []() {};

    RenderFeatureRegistry registry;
    registry.Register(std::move(registration));

    const auto& registrations = registry.GetLifecycleRegistrations();
    Expect(registrations.size() == 1, "Expected one lifecycle registration.");
    const RenderFeatureRegistration& stored = registrations.front();
    Expect(stored.id == "prism.spectral-ocean", "Stable feature ID changed.");
    Expect(
        stored.scope == RenderFeatureScope::DeviceShared,
        "Device-shared scope was not preserved.");
    Expect(stored.stages.size() == 2, "Feature stages were not preserved.");
    Expect(stored.dependencies.size() == 2, "Dependencies were not preserved.");
    Expect(stored.dependencies[0].required, "Required dependency became optional.");
    Expect(!stored.dependencies[1].required, "Optional dependency became required.");
    Expect(
        stored.initializationPrerequisites.size() == 3,
        "Initialization prerequisites were not preserved.");
    Expect(
        stored.operations.Has(RenderFeatureOperation::Initialize),
        "Required initialize operation is missing.");
    Expect(
        stored.operations.Has(RenderFeatureOperation::BuildGraph),
        "Required graph operation is missing.");
    Expect(
        stored.operations.Has(RenderFeatureOperation::Shutdown),
        "Required shutdown operation is missing.");
    Expect(
        !stored.operations.Has(RenderFeatureOperation::PrepareFrame),
        "Omitted optional prepare operation became mandatory.");
    Expect(
        !stored.operations.Has(RenderFeatureOperation::Resize),
        "Omitted optional resize operation became mandatory.");
    Expect(
        !stored.operations.Has(RenderFeatureOperation::SceneChanged),
        "Omitted optional scene operation became mandatory.");
}

void TestViewLocalScope()
{
    RenderFeatureRegistration registration{};
    registration.id = "prism.taa";
    registration.scope = RenderFeatureScope::ViewLocal;

    RenderFeatureRegistry registry;
    registry.Register(std::move(registration));

    const auto& registrations = registry.GetLifecycleRegistrations();
    Expect(registrations.size() == 1, "Expected one view-local registration.");
    Expect(
        registrations.front().scope == RenderFeatureScope::ViewLocal,
        "View-local scope was not preserved.");
}

void TestScopedLifecycleEventRouting()
{
    RenderFeatureRegistry registry;
    std::size_t sharedPrepare = 0;
    std::size_t viewPrepare = 0;
    std::size_t sharedResize = 0;
    std::size_t viewResize = 0;
    std::size_t sharedScene = 0;
    std::size_t viewScene = 0;
    const auto registerFeature =
        [&](const char* id,
            const RenderFeatureScope scope,
            std::size_t& prepare,
            std::size_t& resize,
            std::size_t& scene)
    {
        RenderFeatureRegistration registration{};
        registration.id = id;
        registration.scope = scope;
        registration.operations.prepareFrame =
            [&prepare](const RenderFeatureFrameContext&)
            {
                ++prepare;
            };
        registration.operations.resize =
            [&resize](const RenderFeatureResizeContext&)
            {
                ++resize;
            };
        registration.operations.sceneChanged =
            [&scene](const RenderFeatureSceneContext&)
            {
                ++scene;
            };
        registry.Register(std::move(registration));
    };
    registerFeature(
        "shared-events",
        RenderFeatureScope::DeviceShared,
        sharedPrepare,
        sharedResize,
        sharedScene);
    registerFeature(
        "view-events",
        RenderFeatureScope::ViewLocal,
        viewPrepare,
        viewResize,
        viewScene);
    registry.InitializeLifecycleFeatures(
        RenderFeatureInitializationContext{});
    Prism::Scene::RenderScene renderScene;
    const auto renderSceneView =
        Prism::Scene::RenderSceneView{
            Prism::Scene::RenderSceneControlView, renderScene};
    registry.PrepareLifecycleFeatures(
        {91, 2, 0, 1280, 800, renderSceneView},
        RenderFeatureScope::ViewLocal);
    registry.ResizeLifecycleFeatures(
        {91, 2, 1280, 800, true},
        RenderFeatureScope::DeviceShared);
    registry.NotifyLifecycleSceneChanged(
        {renderSceneView, 7},
        RenderFeatureScope::ViewLocal);
    Expect(
        sharedPrepare == 0
            && viewPrepare == 1
            && sharedResize == 1
            && viewResize == 0
            && sharedScene == 0
            && viewScene == 1,
        "Scoped lifecycle routing crossed shared/view ownership.");
    registry.ShutdownLifecycleFeatures();
}

RenderFeatureRegistration MakeFeature(
    std::string id,
    const RenderFeatureStage stage = RenderFeatureStage::Opaque)
{
    RenderFeatureRegistration registration{};
    registration.id = std::move(id);
    registration.stages = {stage};
    return registration;
}

void TestValidationAndStableOrdering()
{
    {
        RenderFeatureRegistry registry;
        registry.Register(MakeFeature("duplicate"));
        registry.Register(MakeFeature("duplicate"));
        ExpectFailureContaining(
            [&registry]()
            { static_cast<void>(registry.ResolveLifecycleOrder()); },
            "Duplicate render feature ID: duplicate");
    }
    {
        RenderFeatureRegistry registry;
        auto consumer = MakeFeature("consumer");
        consumer.dependencies.push_back({"missing", true});
        registry.Register(std::move(consumer));
        ExpectFailureContaining(
            [&registry]()
            { static_cast<void>(registry.ResolveLifecycleOrder()); },
            "missing required dependency 'missing'");
    }
    {
        RenderFeatureRegistry registry;
        auto first = MakeFeature("first");
        auto second = MakeFeature("second");
        first.dependencies.push_back({"second", true});
        second.dependencies.push_back({"first", true});
        registry.Register(std::move(first));
        registry.Register(std::move(second));
        ExpectFailureContaining(
            [&registry]()
            { static_cast<void>(registry.ResolveLifecycleOrder()); },
            "dependency cycle");
    }
    {
        RenderFeatureRegistry registry;
        auto missingOperation = MakeFeature("missing-operation");
        missingOperation.requiredOperations = {
            RenderFeatureOperation::Initialize};
        registry.Register(std::move(missingOperation));
        ExpectFailureContaining(
            [&registry]()
            { static_cast<void>(registry.ResolveLifecycleOrder()); },
            "missing a required lifecycle operation");
    }
    {
        RenderFeatureRegistry registry;
        registry.Register(
            MakeFeature("post", RenderFeatureStage::PostProcess));
        registry.Register(
            MakeFeature("opaque-b", RenderFeatureStage::Opaque));
        registry.Register(
            MakeFeature("opaque-a", RenderFeatureStage::Opaque));
        auto shadowAfterPost =
            MakeFeature("shadow-after-post", RenderFeatureStage::Shadows);
        shadowAfterPost.dependencies.push_back({"post", true});
        registry.Register(std::move(shadowAfterPost));

        const auto& order = registry.ResolveLifecycleOrder();
        Expect(order.size() == 4, "Resolved lifecycle order has wrong size.");
        Expect(order[0] == "opaque-b", "Stage order is not stable.");
        Expect(order[1] == "opaque-a", "Registration tie order changed.");
        Expect(order[2] == "post", "Dependency-ready stage order changed.");
        Expect(
            order[3] == "shadow-after-post",
            "Feature ran before its declared dependency.");
    }
}

void TestInitializationFailureCleanupAndIdempotentShutdown()
{
    std::vector<std::string> events;
    RenderFeatureRegistry registry;

    auto dependency = MakeFeature("dependency");
    dependency.requiredOperations = {
        RenderFeatureOperation::Initialize,
        RenderFeatureOperation::Shutdown};
    dependency.operations.initialize =
        [&events](const RenderFeatureInitializationContext&)
        { events.push_back("initialize-dependency"); };
    dependency.operations.shutdown =
        [&events]() { events.push_back("shutdown-dependency"); };
    registry.Register(std::move(dependency));

    auto failing = MakeFeature("failing");
    failing.dependencies.push_back({"dependency", true});
    failing.requiredOperations = {
        RenderFeatureOperation::Initialize,
        RenderFeatureOperation::Shutdown};
    failing.operations.initialize =
        [&events](const RenderFeatureInitializationContext&)
        {
            events.push_back("initialize-failing");
            throw std::runtime_error("synthetic initialization failure");
        };
    failing.operations.shutdown =
        [&events]() { events.push_back("shutdown-failing"); };
    registry.Register(std::move(failing));

    ExpectFailureContaining(
        [&registry]()
        {
            registry.InitializeLifecycleFeatures(
                RenderFeatureInitializationContext{});
        },
        "synthetic initialization failure");
    const std::vector<std::string> expectedFailureEvents{
        "initialize-dependency",
        "initialize-failing",
        "shutdown-failing",
        "shutdown-dependency"};
    Expect(
        events == expectedFailureEvents,
        "Partial initialization cleanup order is incorrect.");
    registry.ShutdownLifecycleFeatures();
    Expect(
        events == expectedFailureEvents,
        "Shutdown after failed initialization was not idempotent.");

    events.clear();
    RenderFeatureRegistry successfulRegistry;
    auto first = MakeFeature("first");
    first.operations.initialize =
        [&events](const RenderFeatureInitializationContext&)
        { events.push_back("initialize-first"); };
    first.operations.shutdown =
        [&events]() { events.push_back("shutdown-first"); };
    successfulRegistry.Register(std::move(first));
    auto second = MakeFeature("second");
    second.dependencies.push_back({"first", true});
    second.operations.initialize =
        [&events](const RenderFeatureInitializationContext&)
        { events.push_back("initialize-second"); };
    second.operations.shutdown =
        [&events]() { events.push_back("shutdown-second"); };
    successfulRegistry.Register(std::move(second));

    successfulRegistry.InitializeLifecycleFeatures(
        RenderFeatureInitializationContext{});
    successfulRegistry.ShutdownLifecycleFeatures();
    successfulRegistry.ShutdownLifecycleFeatures();
    const std::vector<std::string> expectedSuccessEvents{
        "initialize-first",
        "initialize-second",
        "shutdown-second",
        "shutdown-first"};
    Expect(
        events == expectedSuccessEvents,
        "Successful shutdown was not reverse-order and idempotent.");
}

void TestIndependentStageFeatureExtension()
{
    using namespace Prism::Renderer::Tests;

    struct ExistingOutputSlot
    {
    };
    struct ExistingOutput
    {
        std::uint32_t token = 0;
    };

    RenderFeatureRegistry registry;
    RenderFeatureRegistration existing{};
    existing.id = "test.existing-temporal-output";
    existing.scope = RenderFeatureScope::ViewLocal;
    existing.stages = {RenderFeatureStage::Temporal};
    existing.operations.buildGraph =
        [](RenderFeatureGraphContext& context)
    {
        context.graph.GetBlackboard().Publish<ExistingOutputSlot>(
            ExistingOutput{41u},
            {"test.existing-temporal-output",
             RenderGraphBlackboardValueScope::ViewLocal,
             context.viewId,
             static_cast<std::uint32_t>(context.logicalFrameId),
             1u});
    };
    registry.Register(std::move(existing));

    IndependentRenderFeatureFixture feature;
    registry.Register(feature.Registration());
    registry.InitializeLifecycleFeatures(
        RenderFeatureInitializationContext{});

    RenderGraph graph;
    Prism::Scene::RenderScene scene;
    const auto sceneView =
        Prism::Scene::RenderSceneView{
            Prism::Scene::RenderSceneControlView, scene};
    RenderFeatureGraphContext context{
        graph, 71, 9, 0, 1280, 800, sceneView, false};
    registry.BuildLifecycleStage(RenderFeatureStage::Temporal, context);
    graph.GetBlackboard().Publish<IndependentPostProcessInputSlot>(
        IndependentPostProcessInput{9u},
        {"test.input-producer",
         RenderGraphBlackboardValueScope::ViewLocal,
         context.viewId,
         static_cast<std::uint32_t>(context.logicalFrameId),
         1u});
    registry.BuildLifecycleStage(RenderFeatureStage::PostProcess, context);

    const auto request = RenderGraphBlackboardRequest{
        "test.extension-verifier",
        context.viewId,
        static_cast<std::uint32_t>(context.logicalFrameId),
        1u,
        "IndependentPostProcess.OutputColor"};
    Expect(
        graph.GetBlackboard().Require<ExistingOutputSlot, ExistingOutput>(
            {"test.extension-verifier",
             context.viewId,
             static_cast<std::uint32_t>(context.logicalFrameId),
             1u,
             "ExistingTemporal.Output"}).token == 41u,
        "An independent Feature changed an existing typed output.");
    Expect(
        graph.GetBlackboard().Require<IndependentPostProcessOutputSlot,
            IndependentPostProcessOutput>(request).resourceToken == 10u
            && feature.BuildCount() == 1u,
        "The independent Feature did not use only its typed graph contract.");
    const auto passes = graph.GetPassDescriptions();
    Expect(
        std::ranges::count(
            passes,
            "TestIndependentPostProcess",
            &RenderGraph::PassDescription::name) == 1,
        "The independent Feature did not join the existing PostProcess stage exactly once.");

    RenderFeatureRegistry disabledRegistry;
    IndependentRenderFeatureFixture disabledFeature;
    disabledFeature.SetEnabled(false);
    disabledRegistry.Register(disabledFeature.Registration());
    disabledRegistry.InitializeLifecycleFeatures(
        RenderFeatureInitializationContext{});
    RenderGraph disabledGraph;
    RenderFeatureGraphContext disabledContext{
        disabledGraph, 72, 9, 0, 1280, 800, sceneView, false};
    disabledRegistry.BuildLifecycleStage(
        RenderFeatureStage::PostProcess, disabledContext);
    Expect(
        disabledFeature.BuildCount() == 0u
            && disabledGraph.GetPassDescriptions().empty(),
        "A disabled independent Feature changed the existing graph.");
    disabledRegistry.ShutdownLifecycleFeatures();

    RenderFeatureRegistry invalidRegistry;
    IndependentRenderFeatureFixture invalidFeature;
    invalidRegistry.Register(invalidFeature.Registration());
    invalidRegistry.InitializeLifecycleFeatures(
        RenderFeatureInitializationContext{});
    RenderGraph invalidGraph;
    RenderFeatureGraphContext invalidContext{
        invalidGraph, 73, 9, 0, 1280, 800, sceneView, false};
    ExpectFailureContaining(
        [&invalidRegistry, &invalidContext]()
        {
            invalidRegistry.BuildLifecycleStage(
                RenderFeatureStage::PostProcess, invalidContext);
        },
        "test.independent-post-process");
    ExpectFailureContaining(
        [&invalidRegistry, &invalidContext]()
        {
            invalidRegistry.BuildLifecycleStage(
                RenderFeatureStage::PostProcess, invalidContext);
        },
        "IndependentPostProcess.InputColor");
    invalidRegistry.ShutdownLifecycleFeatures();
    registry.ShutdownLifecycleFeatures();
}

void TestSkyAtmosphereLifecycleRegistration()
{
    RenderFeatureRegistry registry;
    SkyAtmosphere skyAtmosphere;
    std::size_t prepareCount = 0;
    RenderViewId preparedView = 0;
    std::uint32_t preparedFrame = 0;
    BuiltInRenderFeatures::RegisterSkyAtmosphere(
        registry,
        skyAtmosphere,
        [&](const RenderFeatureFrameContext& context)
        {
            ++prepareCount;
            preparedView = context.viewId;
            preparedFrame = context.frameIndex;
        });

    const auto& registrations = registry.GetLifecycleRegistrations();
    Expect(
        registrations.size() == 1
            && registrations.front().id
                == BuiltInRenderFeatures::SkyAtmosphereId
            && registrations.front().scope == RenderFeatureScope::ViewLocal
            && registrations.front().stages
                == std::vector{RenderFeatureStage::FramePreparation}
            && registrations.front().operations.prepareFrame
            && registrations.front().operations.buildGraph
            && !registrations.front().operations.resize
            && !registrations.front().operations.sceneChanged,
        "SkyAtmosphere lost its fixed-LUT frame lifecycle contract.");

    registry.InitializeLifecycleFeatures(
        RenderFeatureInitializationContext{});
    Prism::Scene::RenderScene scene;
    const auto sceneView =
        Prism::Scene::RenderSceneView{
            Prism::Scene::RenderSceneControlView, scene};
    registry.PrepareLifecycleFeatures(
        {9, 2, 1, 1280, 800, sceneView});
    Expect(
        prepareCount == 1 && preparedView == 2 && preparedFrame == 1,
        "SkyAtmosphere parameters were not updated for the target view/frame.");
    registry.ShutdownLifecycleFeatures();
}

void TestViewLocalLifecycleEvents()
{
    struct EventState
    {
        std::size_t prepareCount = 0;
        std::size_t resizeCount = 0;
        std::size_t sceneCount = 0;
        RenderViewId lastViewId = 0;
        bool lastResizeWasGpuIdle = false;
    };

    const auto makeRegistry = [](EventState& state)
    {
        RenderFeatureRegistry registry;
        RenderFeatureRegistration registration{};
        registration.id = "test.view-local-events";
        registration.scope = RenderFeatureScope::ViewLocal;
        registration.operations.prepareFrame =
            [&state](const RenderFeatureFrameContext& context)
        {
            ++state.prepareCount;
            state.lastViewId = context.viewId;
        };
        registration.operations.resize =
            [&state](const RenderFeatureResizeContext& context)
        {
            ++state.resizeCount;
            state.lastViewId = context.viewId;
            state.lastResizeWasGpuIdle = context.gpuIdle;
        };
        registration.operations.sceneChanged =
            [&state](const RenderFeatureSceneContext&)
        {
            ++state.sceneCount;
        };
        registry.Register(std::move(registration));
        registry.InitializeLifecycleFeatures(
            RenderFeatureInitializationContext{});
        return registry;
    };

    EventState gameState{};
    EventState sceneState{};
    RenderFeatureRegistry gameRegistry = makeRegistry(gameState);
    RenderFeatureRegistry sceneRegistry = makeRegistry(sceneState);
    Prism::Scene::RenderScene scene;
    const auto sceneView =
        Prism::Scene::RenderSceneView{
            Prism::Scene::RenderSceneControlView, scene};

    gameRegistry.PrepareLifecycleFeatures(
        {21, 1, 0, 1280, 800, sceneView});
    gameRegistry.ResizeLifecycleFeatures(
        {21, 1, 1920, 1080, true});
    gameRegistry.NotifyLifecycleSceneChanged({sceneView, 4});

    Expect(gameState.prepareCount == 1
            && gameState.resizeCount == 1
            && gameState.sceneCount == 1,
        "View-local lifecycle events were not dispatched once.");
    Expect(gameState.lastViewId == 1
            && gameState.lastResizeWasGpuIdle,
        "View identity or resize completion state was lost.");
    Expect(sceneState.prepareCount == 0
            && sceneState.resizeCount == 0
            && sceneState.sceneCount == 0,
        "One view's lifecycle event leaked into another registry.");

    gameRegistry.ShutdownLifecycleFeatures();
    sceneRegistry.ShutdownLifecycleFeatures();
}

void TestFluidGraphRegistrationScope()
{
    RenderFeatureRegistry registry;
    FluidFeature fluid;
    BuiltInRenderFeatures::RegisterFluid(registry, fluid);
    const auto& registrations = registry.GetLifecycleRegistrations();
    Expect(registrations.size() == 1
            && registrations.front().id == "Fluid"
            && registrations.front().scope
                == RenderFeatureScope::ViewLocal
            && registrations.front().stages
                == std::vector{RenderFeatureStage::PostProcess},
        "Fluid lost its per-SceneRenderer view-local scope or stage.");

    registry.InitializeLifecycleFeatures(
        RenderFeatureInitializationContext{});
    RenderGraph graph;
    Prism::Scene::RenderScene scene;
    const auto sceneView =
        Prism::Scene::RenderSceneView{
            Prism::Scene::RenderSceneControlView, scene};
    RenderFeatureGraphContext context{
        graph, 31, 2, 0, 640, 400, sceneView, false};
    registry.BuildLifecycleStage(
        RenderFeatureStage::WaterOptics,
        context);
    const auto contributionRequest = RenderGraphBlackboardRequest{
        "test.fluid-registration",
        context.viewId,
        graph.GetResourceGeneration(),
        1u,
        "Fluid.Contribution"};
    Expect(!graph.GetBlackboard()
                .GetOptionalOr<FluidFeatureSlot, FluidGraphContribution>(
                    contributionRequest, {})
                .build,
        "Fluid registered outside its declared PostProcess stage.");
    registry.BuildLifecycleStage(
        RenderFeatureStage::PostProcess,
        context);
    Expect(static_cast<bool>(
        graph.GetBlackboard()
            .Require<FluidFeatureSlot, FluidGraphContribution>(
                contributionRequest)
            .build),
        "Fluid did not publish its Feature-owned graph callback.");
    registry.ShutdownLifecycleFeatures();
}

void TestLightingLifecycleRegistrations()
{
    RenderFeatureRegistry registry;
    ClusteredLighting clusteredLighting;
    LocalLightShadows localLightShadows;

    BuiltInRenderFeatures::RegisterClusteredLighting(
        registry,
        clusteredLighting);
    BuiltInRenderFeatures::RegisterLocalLightShadows(
        registry,
        localLightShadows);

    const auto& registrations = registry.GetLifecycleRegistrations();
    Expect(
        registrations.size() == 2,
        "Lighting Feature registrations were not assembled exactly once.");
    const auto findRegistration = [&registrations](const std::string_view id)
        -> const RenderFeatureRegistration&
    {
        const auto match = std::find_if(
            registrations.begin(),
            registrations.end(),
            [id](const RenderFeatureRegistration& registration)
            { return registration.id == id; });
        Expect(match != registrations.end(),
            "A lighting Feature lost its stable registration ID.");
        return *match;
    };
    const RenderFeatureRegistration& clustered =
        findRegistration(BuiltInRenderFeatures::ClusteredLightingId);
    const RenderFeatureRegistration& shadows =
        findRegistration(BuiltInRenderFeatures::LocalLightShadowsId);
    Expect(
        clustered.scope == RenderFeatureScope::ViewLocal
            && clustered.stages
                == std::vector{RenderFeatureStage::Lighting}
            && clustered.operations.buildGraph
            && clustered.operations.resize,
        "ClusteredLighting lost its view-local build/resize contract.");
    Expect(
        shadows.scope == RenderFeatureScope::ViewLocal
            && shadows.stages
                == std::vector{RenderFeatureStage::Shadows}
            && shadows.operations.buildGraph
            && !shadows.operations.resize,
        "LocalLightShadows lost its fixed-size shadow lifecycle contract.");

    const auto& order = registry.ResolveLifecycleOrder();
    Expect(
        order == std::vector<RenderFeatureId>{
            std::string(BuiltInRenderFeatures::LocalLightShadowsId),
            std::string(BuiltInRenderFeatures::ClusteredLightingId)},
        "Lighting Features did not resolve in Shadows-before-Lighting order.");
    Expect(
        !registry.AreLifecycleFeaturesInitialized(),
        "Resolved lighting registrations were reported initialized too early.");
    registry.InitializeLifecycleFeatures(
        RenderFeatureInitializationContext{});
    Expect(
        registry.AreLifecycleFeaturesInitialized(),
        "Lighting lifecycle did not expose its initialized state.");
    registry.ShutdownLifecycleFeatures();
}

void TestShadowAndReflectionLifecycleRegistrations()
{
    RenderFeatureRegistry registry;
    VarianceShadowMaps varianceShadowMaps;
    PlanarReflections planarReflections;

    BuiltInRenderFeatures::RegisterVarianceShadowMaps(
        registry,
        varianceShadowMaps);
    BuiltInRenderFeatures::RegisterPlanarReflections(
        registry,
        planarReflections);

    const auto& registrations = registry.GetLifecycleRegistrations();
    Expect(
        registrations.size() == 2,
        "Shadow/reflection Features were not assembled exactly once.");
    const RenderFeatureRegistration& variance = registrations[0];
    const RenderFeatureRegistration& planar = registrations[1];
    Expect(
        variance.id == BuiltInRenderFeatures::VarianceShadowMapsId
            && variance.scope == RenderFeatureScope::ViewLocal
            && variance.stages
                == std::vector{RenderFeatureStage::Shadows}
            && variance.operations.buildGraph
            && !variance.operations.resize,
        "VarianceShadowMaps lost its fixed-resolution graph contract.");
    Expect(
        planar.id == BuiltInRenderFeatures::PlanarReflectionsId
            && planar.scope == RenderFeatureScope::ViewLocal
            && planar.stages
                == std::vector{RenderFeatureStage::Shadows}
            && planar.operations.buildGraph
            && planar.operations.resize,
        "PlanarReflections lost its view-local build/resize contract.");
    Expect(
        registry.ResolveLifecycleOrder()
            == std::vector<RenderFeatureId>{
                std::string(BuiltInRenderFeatures::VarianceShadowMapsId),
                std::string(BuiltInRenderFeatures::PlanarReflectionsId)},
        "Shadow/reflection registration order changed.");
}

void TestPostProcessLifecycleRegistrations()
{
    RenderFeatureRegistry registry;
    ScreenSpaceEffects screenSpaceEffects;
    TemporalAntiAliasing temporalAntiAliasing;
    std::vector<std::string> resizeOrder;

    RenderFeatureRegistration planar{};
    planar.id = std::string(BuiltInRenderFeatures::PlanarReflectionsId);
    planar.scope = RenderFeatureScope::ViewLocal;
    planar.stages = {RenderFeatureStage::Shadows};
    planar.requiredOperations = {RenderFeatureOperation::Resize};
    planar.operations.resize =
        [&resizeOrder](const RenderFeatureResizeContext&)
        { resizeOrder.emplace_back("planar"); };
    registry.Register(std::move(planar));
    BuiltInRenderFeatures::RegisterScreenSpaceEffects(
        registry,
        screenSpaceEffects,
        [&resizeOrder](const RenderFeatureResizeContext&)
        { resizeOrder.emplace_back("screen-space"); });
    BuiltInRenderFeatures::RegisterTemporalAntiAliasing(
        registry,
        temporalAntiAliasing,
        [&resizeOrder](const RenderFeatureResizeContext&)
        { resizeOrder.emplace_back("taa"); });

    const auto& registrations = registry.GetLifecycleRegistrations();
    Expect(
        registrations.size() == 3,
        "Post-process lifecycle registrations were not assembled exactly once.");
    const RenderFeatureRegistration& screenSpace = registrations[1];
    const RenderFeatureRegistration& temporal = registrations[2];
    Expect(
        screenSpace.id == BuiltInRenderFeatures::ScreenSpaceEffectsId
            && screenSpace.scope == RenderFeatureScope::ViewLocal
            && screenSpace.stages
                == std::vector{RenderFeatureStage::PostProcess}
            && screenSpace.operations.buildGraph
            && screenSpace.operations.resize,
        "ScreenSpaceEffects lost its view-local graph/resize contract.");
    Expect(
        temporal.id == BuiltInRenderFeatures::TemporalAntiAliasingId
            && temporal.scope == RenderFeatureScope::ViewLocal
            && temporal.stages
                == std::vector{RenderFeatureStage::Temporal}
            && temporal.operations.buildGraph
            && temporal.operations.resize
            && temporal.operations.sceneChanged,
        "TemporalAntiAliasing lost its graph/resize/history contract.");
    const auto& order = registry.ResolveLifecycleOrder();
    Expect(
        order == std::vector<RenderFeatureId>{
            std::string(BuiltInRenderFeatures::PlanarReflectionsId),
            std::string(BuiltInRenderFeatures::ScreenSpaceEffectsId),
            std::string(BuiltInRenderFeatures::TemporalAntiAliasingId)},
        "Post-process dependencies did not preserve resize input ordering.");

    registry.InitializeLifecycleFeatures(
        RenderFeatureInitializationContext{});
    registry.ResizeLifecycleFeatures(
        RenderFeatureResizeContext{1, 7, 1280, 800, true});
    Expect(
        resizeOrder
            == std::vector<std::string>{"planar", "screen-space", "taa"},
        "Screen-space/TAA resize callbacks ran in the wrong order.");
    Prism::Scene::RenderScene scene;
    const auto sceneView =
        Prism::Scene::RenderSceneView{
            Prism::Scene::RenderSceneControlView, scene};
    registry.NotifyLifecycleSceneChanged({sceneView, 2});
    Expect(
        temporalAntiAliasing.GetHistoryResetCallCount() == 1,
        "TAA scene change did not reset only its local history.");

    RenderFeatureRegistry editorRegistry;
    ScreenSpaceEffects editorScreenSpaceEffects;
    TemporalAntiAliasing editorTemporalAntiAliasing;
    RenderFeatureRegistration editorPlanar{};
    editorPlanar.id = std::string(BuiltInRenderFeatures::PlanarReflectionsId);
    editorPlanar.scope = RenderFeatureScope::ViewLocal;
    editorPlanar.stages = {RenderFeatureStage::Shadows};
    editorRegistry.Register(std::move(editorPlanar));
    BuiltInRenderFeatures::RegisterScreenSpaceEffects(
        editorRegistry,
        editorScreenSpaceEffects,
        [](const RenderFeatureResizeContext&) {});
    BuiltInRenderFeatures::RegisterTemporalAntiAliasing(
        editorRegistry,
        editorTemporalAntiAliasing,
        [](const RenderFeatureResizeContext&) {});
    editorRegistry.InitializeLifecycleFeatures(
        RenderFeatureInitializationContext{});
    Expect(
        editorTemporalAntiAliasing.GetHistoryResetCallCount() == 0,
        "The Game view scene event leaked into the Editor TAA history.");
    editorRegistry.NotifyLifecycleSceneChanged({sceneView, 3});
    Expect(
        temporalAntiAliasing.GetHistoryResetCallCount() == 1
            && editorTemporalAntiAliasing.GetHistoryResetCallCount() == 1,
        "A view-local scene event leaked into another TAA history.");
    editorRegistry.ShutdownLifecycleFeatures();
    registry.ShutdownLifecycleFeatures();
}

void TestOceanLifecycleRegistrations()
{
    RenderFeatureRegistry registry;
    FftOcean fftOcean;
    SpectralOceanSimulation spectralOcean;
    LocalWaveGpuResources localWave;
    std::size_t fftSelectionChecks = 0;
    std::size_t spectralSelectionChecks = 0;
    std::size_t localSelectionChecks = 0;

    BuiltInRenderFeatures::RegisterFftOcean(
        registry,
        fftOcean,
        [&fftSelectionChecks]()
        {
            ++fftSelectionChecks;
            return false;
        });
    BuiltInRenderFeatures::RegisterSpectralOcean(
        registry,
        spectralOcean,
        [&spectralSelectionChecks]()
        {
            ++spectralSelectionChecks;
            return false;
        });
    BuiltInRenderFeatures::RegisterLocalWave(
        registry,
        localWave,
        [&localSelectionChecks]()
        {
            ++localSelectionChecks;
            return false;
        });

    const auto& registrations = registry.GetLifecycleRegistrations();
    Expect(
        registrations.size() == 3,
        "Ocean lifecycle registrations were not assembled exactly once.");
    const RenderFeatureRegistration& legacy = registrations[0];
    const RenderFeatureRegistration& spectral = registrations[1];
    const RenderFeatureRegistration& local = registrations[2];
    Expect(
        legacy.id == BuiltInRenderFeatures::FftOceanId
            && legacy.scope == RenderFeatureScope::ViewLocal
            && legacy.stages
                == std::vector{RenderFeatureStage::FramePreparation}
            && legacy.operations.buildGraph
            && !legacy.operations.initialize
            && !legacy.operations.shutdown,
        "Legacy FFT ocean lost its view-local borrowed graph contract.");
    Expect(
        spectral.id == BuiltInRenderFeatures::SpectralOceanId
            && spectral.scope == RenderFeatureScope::DeviceShared
            && spectral.stages
                == std::vector{RenderFeatureStage::FramePreparation}
            && spectral.operations.buildGraph
            && !spectral.operations.initialize
            && !spectral.operations.shutdown,
        "Spectral ocean no longer reflects shared-owner graph output.");
    Expect(
        local.id == BuiltInRenderFeatures::LocalWaveId
            && local.scope == RenderFeatureScope::DeviceShared
            && local.stages
                == std::vector{RenderFeatureStage::FramePreparation}
            && local.dependencies.size() == 1
            && local.dependencies.front().featureId
                == BuiltInRenderFeatures::SpectralOceanId
            && local.dependencies.front().required
            && local.operations.buildGraph
            && !local.operations.initialize
            && !local.operations.shutdown,
        "Local waves lost their shared-owner spectral dependency.");
    Expect(
        registry.ResolveLifecycleOrder()
            == std::vector<RenderFeatureId>{
                std::string(BuiltInRenderFeatures::FftOceanId),
                std::string(BuiltInRenderFeatures::SpectralOceanId),
                std::string(BuiltInRenderFeatures::LocalWaveId)},
        "Ocean graph publication order changed.");

    registry.InitializeLifecycleFeatures(
        RenderFeatureInitializationContext{});
    RenderGraph graph;
    Prism::Scene::RenderScene scene;
    const auto sceneView =
        Prism::Scene::RenderSceneView{
            Prism::Scene::RenderSceneControlView, scene};
    RenderFeatureGraphContext context{
        graph, 51, 2, 0, 1280, 800, sceneView, false};
    registry.BuildLifecycleStage(
        RenderFeatureStage::PostProcess,
        context);
    Expect(
        fftSelectionChecks == 0
            && spectralSelectionChecks == 0
            && localSelectionChecks == 0,
        "Ocean graph output ran outside FramePreparation.");
    registry.BuildLifecycleStage(
        RenderFeatureStage::FramePreparation,
        context);
    Expect(
        fftSelectionChecks == 1
            && spectralSelectionChecks == 1
            && localSelectionChecks == 1,
        "Ocean implementation predicates were not evaluated exactly once.");
    registry.ShutdownLifecycleFeatures();
}

void TestTerrainLifecycleRegistrations()
{
    RenderFeatureRegistry registry;
    InteractiveTerrain interactiveTerrain;
    VirtualTextureCache virtualTextureCache;
    std::size_t terrainPrepareCount = 0;
    std::size_t virtualTexturePrepareCount = 0;

    BuiltInRenderFeatures::RegisterInteractiveTerrain(
        registry,
        interactiveTerrain,
        [&terrainPrepareCount](const RenderFeatureFrameContext&)
        {
            ++terrainPrepareCount;
        });
    BuiltInRenderFeatures::RegisterVirtualTextureCache(
        registry,
        virtualTextureCache,
        [&virtualTexturePrepareCount](const RenderFeatureFrameContext&)
        {
            ++virtualTexturePrepareCount;
        });

    const auto& registrations = registry.GetLifecycleRegistrations();
    Expect(
        registrations.size() == 2,
        "Terrain lifecycle registrations were not assembled exactly once.");
    const RenderFeatureRegistration& interactive = registrations[0];
    const RenderFeatureRegistration& virtualTexture = registrations[1];
    Expect(
        interactive.id == BuiltInRenderFeatures::InteractiveTerrainId
            && interactive.scope == RenderFeatureScope::DeviceShared
            && interactive.stages
                == std::vector{RenderFeatureStage::FramePreparation}
            && interactive.operations.prepareFrame
            && interactive.operations.buildGraph
            && interactive.operations.sceneChanged
            && !interactive.operations.initialize
            && !interactive.operations.shutdown,
        "InteractiveTerrain lost its shared borrowed lifecycle contract.");
    Expect(
        virtualTexture.id == BuiltInRenderFeatures::VirtualTextureCacheId
            && virtualTexture.scope == RenderFeatureScope::ViewLocal
            && virtualTexture.stages
                == std::vector{RenderFeatureStage::FramePreparation}
            && virtualTexture.operations.prepareFrame
            && !virtualTexture.operations.buildGraph
            && virtualTexture.operations.sceneChanged
            && !virtualTexture.operations.initialize
            && !virtualTexture.operations.shutdown,
        "VirtualTextureCache lost its view-local residency contract.");

    InteractiveTerrainCommand command{};
    command.revision = 17u;
    interactiveTerrain.UpdateCommand(command);
    registry.InitializeLifecycleFeatures(
        RenderFeatureInitializationContext{});
    Prism::Scene::RenderScene scene;
    const auto sceneView =
        Prism::Scene::RenderSceneView{
            Prism::Scene::RenderSceneControlView, scene};
    registry.PrepareLifecycleFeatures(
        {41, 2, 1, 1280, 800, sceneView});
    Expect(
        terrainPrepareCount == 1
            && virtualTexturePrepareCount == 1,
        "Terrain frame preparation did not run exactly once.");
    Expect(
        interactiveTerrain.PendingCommandRevision() == 17u
            && !virtualTextureCache.IsResetPending(),
        "Terrain test preconditions were not established.");
    registry.NotifyLifecycleSceneChanged({sceneView, 3});
    Expect(
        interactiveTerrain.PendingCommandRevision() == 0u
            && virtualTextureCache.IsResetPending(),
        "Scene change did not reset shared terrain command identity and request view-local VT residency reset.");
    registry.ShutdownLifecycleFeatures();
}

void TestGpuDrivenVisibilityLifecycleRegistration()
{
    RenderFeatureRegistry registry;
    GpuDrivenVisibility visibility;
    std::size_t prepareCount = 0;
    std::size_t resizeCount = 0;
    BuiltInRenderFeatures::RegisterGpuDrivenVisibility(
        registry,
        visibility,
        [&prepareCount](const RenderFeatureFrameContext&)
        {
            ++prepareCount;
        },
        [&resizeCount](const RenderFeatureResizeContext& context)
        {
            Expect(
                context.gpuIdle,
                "GPU-driven resize escaped the GPU-idle safe point.");
            ++resizeCount;
        });

    const auto& registrations = registry.GetLifecycleRegistrations();
    Expect(
        registrations.size() == 1,
        "GPU-driven visibility was not assembled exactly once.");
    const RenderFeatureRegistration& registration = registrations.front();
    Expect(
        registration.id
                == BuiltInRenderFeatures::GpuDrivenVisibilityId
            && registration.scope == RenderFeatureScope::ViewLocal
            && registration.stages
                == std::vector{RenderFeatureStage::FramePreparation}
            && registration.operations.prepareFrame
            && registration.operations.buildGraph
            && registration.operations.resize
            && registration.operations.sceneChanged
            && !registration.operations.initialize
            && !registration.operations.shutdown,
        "GPU-driven visibility lost its view-local lifecycle contract.");

    Prism::Scene::RenderScene scene;
    const auto sceneView =
        Prism::Scene::RenderSceneView{
            Prism::Scene::RenderSceneControlView, scene};
    registry.InitializeLifecycleFeatures(
        RenderFeatureInitializationContext{});
    registry.PrepareLifecycleFeatures(
        {71, 2, 1, 1280, 800, sceneView});
    registry.ResizeLifecycleFeatures(
        {72, 2, 1600, 900, true});
    registry.NotifyLifecycleSceneChanged({sceneView, 5});
    Expect(
        prepareCount == 1 && resizeCount == 1,
        "GPU-driven lifecycle events did not run exactly once.");

    RenderGraph graph;
    RenderFeatureGraphContext graphContext{
        graph, 73, 2, 1, 1600, 900, sceneView, false};
    registry.BuildLifecycleStage(
        RenderFeatureStage::FramePreparation,
        graphContext);
    Expect(
        !graph.GetBlackboard().ContainsFeature<
            GpuDrivenVisibilityFeatureSlot>(),
        "Disabled GPU-driven visibility did not preserve the CPU fallback graph.");
    registry.ShutdownLifecycleFeatures();
}
} // namespace

int main()
{
    try
    {
        TestRegistrationDescription();
        TestViewLocalScope();
        TestScopedLifecycleEventRouting();
        TestValidationAndStableOrdering();
        TestInitializationFailureCleanupAndIdempotentShutdown();
        TestIndependentStageFeatureExtension();
        TestSkyAtmosphereLifecycleRegistration();
        TestViewLocalLifecycleEvents();
        TestFluidGraphRegistrationScope();
        TestLightingLifecycleRegistrations();
        TestShadowAndReflectionLifecycleRegistrations();
        TestPostProcessLifecycleRegistrations();
        TestOceanLifecycleRegistrations();
        TestTerrainLifecycleRegistrations();
        TestGpuDrivenVisibilityLifecycleRegistration();
        std::cout << "Render feature lifecycle tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Render feature lifecycle tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
