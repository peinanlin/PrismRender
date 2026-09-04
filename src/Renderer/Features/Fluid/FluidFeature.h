#pragma once

#include "Renderer/Features/Fluid/FluidCaustics.h"
#include "Renderer/Features/Fluid/FluidGraph.h"
#include "Renderer/Features/Fluid/PbfFluidSimulation.h"
#include "Renderer/Features/Fluid/ScreenSpaceFluidRenderer.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace Prism::Asset
{
class ShaderManager;
}

namespace Prism::RHI
{
class IGraphicsDevice;
class ITexture;
class ITextureView;
}

namespace Prism::Scene
{
class Camera;
class RenderSceneView;
}

namespace Prism::Renderer
{
class PipelineCache;

// Owns the full FluidLab dataflow while each subsystem retains one focused
// responsibility: PBF simulation, screen-space reconstruction, and caustics.
class FluidFeature
{
public:
    void Initialize(
        RHI::IGraphicsDevice& device,
        Asset::ShaderManager& shaderManager,
        PipelineCache& pipelineCache,
        const std::filesystem::path& shaderDirectory,
        RHI::ShaderBinaryFormat shaderFormat,
        std::uint32_t framesInFlight);
    void Resize(
        std::uint32_t width,
        std::uint32_t height,
        std::shared_ptr<RHI::ITexture> sceneColor,
        std::shared_ptr<RHI::ITexture> sceneDepth,
        std::shared_ptr<RHI::ITexture> environment);
    void Update(
        std::uint32_t frameIndex,
        double simulationTimeSeconds,
        const Scene::RenderSceneView& scene,
        const Scene::Camera& renderCamera,
        FluidSettings& settings);
    void SetSceneColor(
        std::shared_ptr<RHI::ITexture> sceneColor);
    [[nodiscard]] FluidGraphContribution
        CreateRenderGraphContribution(std::uint32_t frameIndex);
    void EndFrame(bool executed);
    void RequestReset();

    [[nodiscard]] const FluidStatistics&
        GetStatistics() const;
    [[nodiscard]] std::shared_ptr<RHI::ITexture>
        GetCompositeTexture() const;
    [[nodiscard]] std::shared_ptr<RHI::ITextureView>
        GetCompositeSampledView() const;
    [[nodiscard]] bool IsReady() const;

private:
    [[nodiscard]] TextureHandle AddPasses(
        RenderGraph& graph,
        const FluidGraphInputs& inputs,
        std::uint32_t frameIndex);
    [[nodiscard]] ScreenSpaceFluidFrameParameters
        BuildSurfaceParameters(
            const Scene::RenderSceneView& scene,
            const Scene::Camera& renderCamera,
            const FluidSettings& settings) const;
    [[nodiscard]] FluidCaustics::Parameters
        BuildCausticsParameters(
            const Scene::RenderSceneView& scene,
            const Scene::Camera& renderCamera,
            const FluidSettings& settings) const;

    RHI::IGraphicsDevice* m_device = nullptr;
    PbfFluidSimulation m_simulation;
    ScreenSpaceFluidRenderer m_surface;
    FluidCaustics m_caustics;
    std::shared_ptr<RHI::ITexture> m_sceneColor;
    std::shared_ptr<RHI::ITexture> m_sceneDepth;
    std::shared_ptr<RHI::ITexture> m_environment;
    std::shared_ptr<RHI::ITextureView>
        m_sceneDepthSampledView;
    FluidSettings m_settings{};
    double m_previousSimulationTime = 0.0;
    bool m_timeValid = false;
    bool m_lastSimulationScheduled = false;
    bool m_lastParticlePreviewScheduled = false;
    bool m_lastSurfaceScheduled = false;
    bool m_lastFoamScheduled = false;
    bool m_lastCausticsScheduled = false;
};
} // namespace Prism::Renderer
