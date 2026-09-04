#include "Asset/AssetRegistry.h"
#include "Platform/Window.h"
#include "RHI/IFrameContext.h"
#include "RHI/IRenderBackend.h"
#include "RHI/RenderBackendFactory.h"
#include "Renderer/DemoSceneSettings.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/SceneRendererSharedResources.h"
#include "Scene/DemoSceneCatalog.h"
#include "Scene/RenderScene.h"
#include "RenderFramePacketFixture.h"

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace
{
void Expect(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
}

int main(int argc, char** argv)
{
    try
    {
        using namespace Prism;
        Expect(
            argc == 2
                && (std::string_view(argv[1]) == "d3d12"
                    || std::string_view(argv[1]) == "vulkan"),
            "Usage: PrismGpuDrivenVisibilityGpuTests <d3d12|vulkan>");
        const RHI::GraphicsApi api =
            std::string_view(argv[1]) == "d3d12"
            ? RHI::GraphicsApi::Direct3D12
            : RHI::GraphicsApi::Vulkan;

        Platform::Window window(
            "GPU-driven visibility lifecycle regression",
            480u,
            300u);
        auto backend = RHI::CreateRenderBackend(api);
        backend->Initialize(window);
        RHI::IFrameContext& frame = backend->GetFrameContext();
        Asset::AssetRegistry assetRegistry;
        Scene::RenderScene scene;
        Scene::DemoSceneCatalog::Populate(
            Scene::DemoSceneId::GpuDrivenLab,
            assetRegistry,
            backend->GetGraphicsDevice(),
            scene,
            480.0f / 300.0f);

        auto shared =
            std::make_shared<Renderer::SceneRendererSharedResources>();
        Renderer::SceneRenderer renderer;
        Renderer::ApplyDemoSceneSettings(
            Scene::DemoSceneId::GpuDrivenLab,
            renderer.GetSettings());
        renderer.GetSettings().gpuVisibilityReadbackEnabled = true;
        renderer.Initialize(
            *backend,
            std::filesystem::path(PRISM_RENDER_SHADER_DIR) / "Mesh.hlsl",
            scene,
            nullptr,
            false,
            shared,
            true,
            1u);
        Tests::RenderFramePacketFixture packetFixture(assetRegistry);

        const bool expectedSupported = api == RHI::GraphicsApi::Vulkan;
        Expect(
            renderer.SupportsGpuDrivenRendering() == expectedSupported,
            "GPU-driven backend capability selection changed.");

        const auto renderFrame =
            [&](const Scene::RenderScene& activeScene)
        {
            Expect(
                frame.BeginFrame() == RHI::FrameResult::Ready,
                "GPU-driven fixture BeginFrame failed.");
            const auto packet = packetFixture.Build(
                activeScene,
                0.0,
                frame.GetFrameWidth(),
                frame.GetFrameHeight());
            const Renderer::LogicalFrameId logicalFrameId =
                packet->GetLogicalFrameId().value;
            renderer.SetSharedSimulationProducerForFrame(
                logicalFrameId,
                true);
            renderer.Render(
                *backend,
                packet,
                Scene::GameRenderViewId);
            const bool gpuVisibilityPass = std::ranges::any_of(
                renderer.GetRenderGraph().GetPassInfos(),
                [](const auto& pass)
                {
                    return pass.name == "GpuVisibility";
                });

            RHI::RenderingInfo rendering{};
            rendering.width = frame.GetFrameWidth();
            rendering.height = frame.GetFrameHeight();
            RHI::RenderingAttachment color{};
            color.view = &frame.GetCurrentBackBufferView();
            color.loadOperation = RHI::LoadOperation::Clear;
            color.storeOperation = RHI::StoreOperation::Store;
            color.stateBefore =
                api == RHI::GraphicsApi::Vulkan
                ? RHI::ResourceState::Present
                : RHI::ResourceState::RenderTarget;
            color.stateAfter = RHI::ResourceState::RenderTarget;
            rendering.colorAttachments.push_back(color);
            backend->GetCommandContext().BeginRendering(rendering);
            backend->GetCommandContext().EndRendering();
            Expect(
                frame.EndFrame() == RHI::FrameResult::Ready,
                "GPU-driven fixture EndFrame failed.");
            return gpuVisibilityPass;
        };

        const std::uint32_t warmupFrames =
            frame.GetFramesInFlight() * 2u + 2u;
        for (std::uint32_t index = 0; index < warmupFrames; ++index)
        {
            Expect(
                renderFrame(scene) == expectedSupported,
                "GPU visibility pass did not match backend support.");
        }
        const Renderer::RendererStatistics& gpuStatistics =
            renderer.GetStatistics();
        if (expectedSupported)
        {
            Expect(
                gpuStatistics.gpuDrivenCandidateObjects
                        == scene.GetRenderObjects().size()
                    && gpuStatistics.gpuVisibilityStatisticsValid
                    && renderer.GetGpuVisibilityReasons().size()
                        == scene.GetRenderObjects().size()
                    && renderer.GetGpuVisibilityCamera() != nullptr,
                "GPU object records or delayed visibility readback were not coherent.");
        }

        renderer.GetSettings().gpuDrivenEnabled = false;
        Expect(
            !renderFrame(scene),
            "Disabled GPU-driven rendering still submitted visibility work.");
        Expect(
            renderer.GetStatistics().gpuDrivenCandidateObjects == 0u
                && renderer.GetStatistics().renderedObjects > 0u,
            "Disabled GPU-driven rendering did not use the CPU draw fallback.");

        frame.WaitForGpu();
        renderer.ReleaseSwapChainResources();
        glfwSetWindowSize(window.GetNativeWindow(), 640, 360);
        window.PollEvents();
        frame.Resize(640u, 360u);
        renderer.RecreateSwapChainResources(*backend);
        renderer.GetSettings().gpuDrivenEnabled = true;
        Expect(
            renderer.GetFinalOutputWidth() == 640u
                && renderer.GetFinalOutputHeight() == 360u
                && renderFrame(scene) == expectedSupported,
            "GPU-driven HiZ inputs did not follow the view resize.");

        frame.WaitForGpu();
        Scene::RenderScene replacementScene;
        Scene::DemoSceneCatalog::Populate(
            Scene::DemoSceneId::EditorPreview,
            assetRegistry,
            backend->GetGraphicsDevice(),
            replacementScene,
            640.0f / 360.0f);
        renderer.NotifySceneChanged(replacementScene);
        Expect(
            renderer.GetGpuVisibilityReasons().empty()
                && renderer.GetGpuVisibilityCamera() == nullptr,
            "Scene switch retained stale GPU visibility feedback.");
        for (std::uint32_t index = 0; index < warmupFrames; ++index)
        {
            Expect(
                renderFrame(replacementScene) == expectedSupported,
                "Scene switch changed GPU-driven capability behavior.");
        }
        if (expectedSupported)
        {
            Expect(
                renderer.GetStatistics().gpuDrivenCandidateObjects
                        == replacementScene.GetRenderObjects().size()
                    && renderer.GetGpuVisibilityReasons().size()
                        == replacementScene.GetRenderObjects().size(),
                "Scene switch did not rebuild GPU object records and readback mapping.");
        }

        frame.WaitForGpu();
        std::cout
            << "GPU-driven visibility lifecycle, fallback, resize, scene switch, and readback passed.\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "GPU-driven visibility regression failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
