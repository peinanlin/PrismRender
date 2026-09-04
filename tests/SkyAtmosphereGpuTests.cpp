#include "Asset/AssetRegistry.h"
#include "Platform/Window.h"
#include "RHI/IFrameContext.h"
#include "RHI/IRenderBackend.h"
#include "RHI/RenderBackendFactory.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/SceneRendererSharedResources.h"
#include "Renderer/DemoSceneSettings.h"
#include "Scene/DefaultSceneFactory.h"
#include "Scene/DemoSceneCatalog.h"
#include "Scene/RenderScene.h"
#include "RenderFramePacketFixture.h"

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
void Expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main(int argc, char** argv)
{
    try
    {
        using namespace Prism;
        const bool lightingPreset = argc == 3 && std::string_view(argv[2]) == "--lighting-preset";
        const bool optionalBindings = argc == 3 && std::string_view(argv[2]) == "--optional-bindings";
        const bool waterBindings = argc == 3 && std::string_view(argv[2]) == "--water-bindings";
        const bool oceanBindings = waterBindings || (argc == 3 && std::string_view(argv[2]) == "--ocean-bindings");
        Expect((argc == 2 || lightingPreset || optionalBindings || oceanBindings) && (std::string_view(argv[1]) == "d3d12" || std::string_view(argv[1]) == "vulkan"),
            "Usage: PrismSkyAtmosphereGpuTests <d3d12|vulkan> [--lighting-preset|--optional-bindings|--ocean-bindings|--water-bindings]");
        const auto api = std::string_view(argv[1]) == "d3d12" ? RHI::GraphicsApi::Direct3D12 : RHI::GraphicsApi::Vulkan;
        Platform::Window window("Sky binding lifecycle regression", 320u, 200u);
        auto backend = RHI::CreateRenderBackend(api);
        backend->Initialize(window);
        auto& frame = backend->GetFrameContext();
        Scene::RenderScene scene;
        Asset::AssetRegistry assetRegistry;
        Scene::DefaultSceneFactory::PopulateEditorPreviewScene(backend->GetGraphicsDevice(), scene);
        Scene::DefaultSceneFactory::ConfigureEditorPreviewWorld(scene, 1.6f);
        auto shared = std::make_shared<Renderer::SceneRendererSharedResources>();
        std::array<Renderer::SceneRenderer, 2> views;
        Tests::RenderFramePacketFixture packetFixture(assetRegistry);
        struct WaitBeforeViewsDie
        {
            RHI::IFrameContext& frame;
            ~WaitBeforeViewsDie() { try { frame.WaitForGpu(); } catch (...) {} }
        } waitBeforeViewsDie{frame};
        const auto shader = std::filesystem::path(PRISM_RENDER_SHADER_DIR) / "Mesh.hlsl";
        for (std::size_t view = 0; view < views.size(); ++view)
        {
            // Optional isolation uses the shipped Lights preset, never a
            // modified Demo default. The default deferred regression remains.
            if (lightingPreset)
                Renderer::ApplyDemoSceneSettings(Scene::DemoSceneId::LightingLab, views[view].GetSettings());
            if (oceanBindings)
            {
                auto& settings = views[view].GetSettings();
                settings.fftOceanEnabled = true;
                settings.ocean.implementation = Renderer::OceanImplementation::SpectralOcean;
                settings.ocean.debug.renderWater = false;
                settings.ocean.local.enabled = true;
                if (waterBindings)
                    settings.ocean.opticsModel = Renderer::OceanOpticsModel::HpWater;
            }
            views[view].Initialize(*backend, shader, scene, nullptr, false, shared, view == 0u);
        }
        // Each phase spans every frame slot. The two views deliberately disagree
        // and exercise both the physical-atmosphere and skybox enable predicates.
        constexpr std::uint32_t PhaseCount = 8u;
        constexpr std::array<std::array<bool, 2>, PhaseCount> planarEnabled{{
            {false, false}, {true, false}, {false, true}, {false, false},
            {false, true}, {true, false}, {true, true}, {false, false}}};
        constexpr std::array<std::array<bool, 2>, PhaseCount> aoEnabled{{
            {false, false}, {false, true}, {true, false}, {false, false},
            {true, false}, {false, true}, {true, true}, {false, false}}};
        for (std::uint32_t phase = 0; phase < PhaseCount; ++phase)
        {
            // Rebuild once after disabled frames, once after an enabled frame.
            // Match the application's idle-before-resize resource contract.
            if (phase == 4u || phase == 6u)
            {
                frame.WaitForGpu();
                for (auto& view : views) view.ReleaseSwapChainResources();
                const int width = phase == 4u ? 400 : 320;
                const int height = phase == 4u ? 250 : 200;
                glfwSetWindowSize(window.GetNativeWindow(), width, height);
                window.PollEvents();
                frame.Resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
                for (auto& view : views)
                {
                    view.RecreateSwapChainResources(*backend);
                    Expect(view.GetFinalOutputWidth() == static_cast<std::uint32_t>(width)
                        && view.GetFinalOutputHeight() == static_cast<std::uint32_t>(height),
                        "View-sized resources did not follow the resize.");
                }
            }
            std::cout << "Optional bindings phase " << phase << std::endl;
            for (std::uint32_t step = 0; step <= frame.GetFramesInFlight(); ++step)
            {
                Expect(frame.BeginFrame() == RHI::FrameResult::Ready, "Sky fixture BeginFrame failed.");
                const auto packet = packetFixture.Build(
                    scene,
                    0.0,
                    frame.GetFrameWidth(),
                    frame.GetFrameHeight());
                const Renderer::LogicalFrameId logicalFrameId =
                    packet->GetLogicalFrameId().value;
                for (std::size_t view = 0; view < views.size(); ++view)
                {
                    auto& settings = views[view].GetSettings();
                    const auto skyPhase = phase % 4u;
                    settings.physicalAtmosphereEnabled = view == 0u ? skyPhase == 1u || skyPhase == 2u : skyPhase == 2u;
                    settings.skyboxEnabled = view == 0u ? skyPhase != 2u : true;
                    settings.planarReflectionsEnabled = planarEnabled[phase][view];
                    settings.gtaoEnabled = aoEnabled[phase][view];
                    if (oceanBindings)
                    {
                        // Both views share the existing first-view producer.
                        // Exercise hidden water with allocated local resources.
                        settings.ocean.debug.renderWater = skyPhase == 1u || skyPhase == 2u;
                        settings.ocean.local.enabled = skyPhase != 1u;
                    }
                    if (optionalBindings)
                    {
                        const bool enabled = aoEnabled[phase][view];
                        settings.shadowsEnabled = enabled;
                        settings.localLightShadowsEnabled = enabled;
                        settings.clusteredLightingEnabled = enabled;
                        settings.temporalAntiAliasingEnabled = enabled;
                        settings.bloomEnabled = enabled;
                        settings.screenSpaceReflectionsEnabled = enabled;
                        settings.shadowFilterMode = phase == 2u || phase == 6u
                            ? Renderer::ShadowFilterMode::Evsm : Renderer::ShadowFilterMode::Pcf3x3;
                    }
                    views[view].SetSharedSimulationProducerForFrame(
                        logicalFrameId,
                        view == 0u);
                    views[view].Render(
                        *backend,
                        packet,
                        Scene::GameRenderViewId);
                    const auto& passes = views[view].GetRenderGraph().GetPassInfos();
                    const bool produced = std::ranges::any_of(passes, [](const auto& pass)
                    { return pass.name == "AtmosphereSkyView"; });
                    Expect(produced == (settings.physicalAtmosphereEnabled && settings.skyboxEnabled),
                        "Sky LUT producer did not match this view's enable predicate.");
                    const bool planarProduced = std::ranges::any_of(passes, [](const auto& pass)
                    { return pass.name == "PlanarReflection"; });
                    Expect(planarProduced == (settings.deferredRenderingEnabled && settings.planarReflectionsEnabled),
                        "Planar producer did not match this view's enable predicate.");
                    const bool aoProduced = std::ranges::any_of(passes, [](const auto& pass)
                    { return pass.name == "GTAO"; });
                    Expect(aoProduced == (settings.deferredRenderingEnabled && settings.gtaoEnabled),
                        "GTAO producer did not match this view's enable predicate.");
                }
                RHI::RenderingInfo rendering{};
                rendering.width = frame.GetFrameWidth();
                rendering.height = frame.GetFrameHeight();
                RHI::RenderingAttachment color{};
                color.view = &frame.GetCurrentBackBufferView();
                color.loadOperation = RHI::LoadOperation::Clear;
                color.storeOperation = RHI::StoreOperation::Store;
                color.stateBefore = api == RHI::GraphicsApi::Vulkan ? RHI::ResourceState::Present : RHI::ResourceState::RenderTarget;
                color.stateAfter = RHI::ResourceState::RenderTarget;
                rendering.colorAttachments.push_back(color);
                backend->GetCommandContext().BeginRendering(rendering);
                backend->GetCommandContext().EndRendering();
                Expect(frame.EndFrame() == RHI::FrameResult::Ready, "Sky fixture EndFrame failed.");
            }
        }
        frame.WaitForGpu();
        std::cout << "Optional bindings: two full scene views, independent toggles across all frame slots and two resizes passed.\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Sky binding regression failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
