#include "Renderer/Features/Fluid/FluidFeature.h"

#include "Core/Assert.h"
#include "RHI/IGraphicsDevice.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderSceneView.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>

namespace Prism::Renderer
{
using namespace DirectX;

void FluidFeature::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderDirectory,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight)
{
    m_device = &device;
    m_simulation.Initialize(
        device,
        shaderManager,
        pipelineCache,
        shaderDirectory,
        shaderFormat,
        framesInFlight,
        m_settings);
    m_surface.Initialize(
        device,
        shaderManager,
        pipelineCache,
        shaderDirectory,
        shaderFormat,
        framesInFlight);
    m_caustics.Initialize(
        device,
        shaderManager,
        pipelineCache,
        shaderDirectory / "FluidCaustics.hlsl",
        shaderFormat,
        framesInFlight);
}

void FluidFeature::Resize(
    const std::uint32_t width,
    const std::uint32_t height,
    std::shared_ptr<RHI::ITexture> sceneColor,
    std::shared_ptr<RHI::ITexture> sceneDepth,
    std::shared_ptr<RHI::ITexture> environment)
{
    Core::Check(
        m_device != nullptr && sceneColor != nullptr
            && sceneDepth != nullptr && environment != nullptr,
        "Fluid resize requires initialized scene inputs.");
    m_sceneColor = std::move(sceneColor);
    m_sceneDepth = std::move(sceneDepth);
    m_environment = std::move(environment);

    RHI::TextureViewDescription sampledView{};
    sampledView.type = RHI::TextureViewType::Sampled;
    m_sceneDepthSampledView = m_device->CreateTextureView(
        m_sceneDepth,
        sampledView);

    m_surface.Resize(width, height);
    m_caustics.Resize(
        width,
        height,
        m_surface.GetRawDepthSampledView(),
        m_surface.GetNormalSampledView(),
        m_sceneDepthSampledView);
    m_surface.SetInputs(
        m_simulation.GetParticlePositionBufferShared(),
        m_simulation.GetParticleDensityBufferShared(),
        m_simulation.GetParticleCount(),
        m_sceneColor,
        m_sceneDepth,
        m_environment,
        m_caustics.GetCompositeTexture());
}

void FluidFeature::Update(
    const std::uint32_t frameIndex,
    const double simulationTimeSeconds,
    const Scene::RenderSceneView& scene,
    const Scene::Camera& renderCamera,
    FluidSettings& settings)
{
    float deltaTime = 1.0f / 60.0f;
    if (m_timeValid)
    {
        deltaTime = static_cast<float>(std::clamp(
            simulationTimeSeconds - m_previousSimulationTime,
            0.0,
            0.25));
    }
    m_previousSimulationTime = simulationTimeSeconds;
    m_timeValid = true;

    if (settings.resetRequested)
    {
        m_simulation.RequestReset();
        settings.resetRequested = false;
    }
    m_settings = settings;
    m_simulation.Update(frameIndex, deltaTime, settings);

    // A particle-count/domain change may rebuild the simulation allocation.
    // SetInputs is pointer-stable in the common case and only rebuilds surface
    // descriptor sets when that allocation actually changes.
    if (m_sceneColor != nullptr && m_sceneDepth != nullptr
        && m_environment != nullptr && m_caustics.IsReady())
    {
        m_surface.SetInputs(
            m_simulation.GetParticlePositionBufferShared(),
            m_simulation.GetParticleDensityBufferShared(),
            m_simulation.GetParticleCount(),
            m_sceneColor,
            m_sceneDepth,
            m_environment,
            m_caustics.GetCompositeTexture());
    }
    m_surface.Update(
        frameIndex,
        BuildSurfaceParameters(
            scene,
            renderCamera,
            settings));
    m_caustics.Update(
        frameIndex,
        BuildCausticsParameters(
            scene,
            renderCamera,
            settings));
}

FluidGraphContribution FluidFeature::CreateRenderGraphContribution(
    const std::uint32_t frameIndex)
{
    FluidGraphContribution contribution{};
    contribution.build =
        [this, frameIndex](
            RenderGraph& graph,
            const FluidGraphInputs& inputs)
        {
            return FluidGraphResult{AddPasses(
                graph, inputs, frameIndex)};
        };
    return contribution;
}

void FluidFeature::SetSceneColor(
    std::shared_ptr<RHI::ITexture> sceneColor)
{
    Core::Check(
        sceneColor != nullptr,
        "Fluid scene-color input cannot be null.");
    m_sceneColor = std::move(sceneColor);
    if (m_sceneDepth == nullptr || m_environment == nullptr
        || !m_caustics.IsReady())
    {
        return;
    }
    m_surface.SetInputs(
        m_simulation.GetParticlePositionBufferShared(),
        m_simulation.GetParticleDensityBufferShared(),
        m_simulation.GetParticleCount(),
        m_sceneColor,
        m_sceneDepth,
        m_environment,
        m_caustics.GetCompositeTexture());
}

TextureHandle FluidFeature::AddPasses(
    RenderGraph& graph,
    const FluidGraphInputs& inputs,
    const std::uint32_t frameIndex)
{
    Core::Check(
        IsReady(),
        "Fluid graph registration requires initialized resources.");

    PbfFluidGraphResources simulationResources =
        m_simulation.RegisterRenderGraph(
            graph,
            frameIndex,
            !m_settings.paused);
    m_lastSimulationScheduled =
        simulationResources.simulationScheduled;
    if (m_lastSimulationScheduled)
    {
        PbfFluidSimulation::AddPasses(
            graph,
            simulationResources,
            [this, frameIndex](
                RHI::ICommandContext& commandContext,
                const RenderGraphPassResources&)
            {
                m_simulation.Execute(
                    commandContext,
                    frameIndex);
            });
    }

    Core::Check(inputs.environment.IsValid(),
        "Fluid requires the shared environment graph input.");
    const bool particlePreview =
        m_settings.renderMode == FluidRenderMode::Particles;
    const bool causticsEnabled = !particlePreview
        && m_settings.causticsEnabled
        && m_settings.causticsIntensity > 0.0f;
    TextureHandle causticsRaw{};
    TextureHandle causticsBlur{};
    // FluidCompositeCS has a statically reflected caustics SRV in every
    // variant. Import it even when generation is disabled so the render graph
    // owns the descriptor's ShaderResource transition instead of relying on a
    // uniform branch to hide an Undefined image layout from Vulkan.
    TextureHandle causticsComposite = graph.ImportTexture(
        "Fluid.Caustics.Composite",
        *m_caustics.GetCompositeTexture(),
        m_caustics.GetCompositeInitialState());
    if (causticsEnabled)
    {
        causticsRaw = graph.ImportTexture(
            "Fluid.Caustics.Raw",
            *m_caustics.GetRawTexture(),
            m_caustics.GetRawInitialState());
        causticsBlur = graph.ImportTexture(
            "Fluid.Caustics.BlurPing",
            *m_caustics.GetBlurPingTexture(),
            m_caustics.GetBlurPingInitialState());
    }

    ScreenSpaceFluidGraphResources surfaceResources =
        m_surface.RegisterRenderGraph(
            graph,
            simulationResources.positions,
            simulationResources.densities,
            inputs.sceneColor,
            inputs.sceneDepth,
            inputs.environment,
            causticsComposite);
    const ScreenSpaceFluidPassCallbacks surfaceCallbacks =
        m_surface.CreatePassCallbacks(frameIndex);
    if (particlePreview)
    {
        ScreenSpaceFluidRenderer::AddParticlePreviewPasses(
            graph,
            surfaceResources,
            surfaceCallbacks);
        m_lastParticlePreviewScheduled = true;
        m_lastSurfaceScheduled = false;
        m_lastFoamScheduled = false;
        m_lastCausticsScheduled = false;
        return surfaceResources.composite;
    }

    m_lastParticlePreviewScheduled = false;
    ScreenSpaceFluidRenderer::AddSurfacePasses(
        graph,
        surfaceResources,
        surfaceCallbacks);
    ScreenSpaceFluidRenderer::AddFilterPasses(
        graph,
        surfaceResources,
        m_settings.bilateralIterations,
        m_settings.foamEnabled,
        surfaceCallbacks);
    m_lastSurfaceScheduled = true;
    m_lastFoamScheduled = m_settings.foamEnabled;

    if (causticsEnabled)
    {
        FluidCaustics::AddPasses(
            graph,
            surfaceResources.rawDepth,
            surfaceResources.normal,
            inputs.sceneDepth,
            causticsRaw,
            causticsBlur,
            causticsComposite,
            [this, frameIndex](
                RHI::ICommandContext& commandContext,
                const RenderGraphPassResources&)
            {
                m_caustics.ExecuteGenerate(
                    commandContext,
                    frameIndex);
            },
            [this, frameIndex](
                RHI::ICommandContext& commandContext,
                const RenderGraphPassResources&)
            {
                m_caustics.ExecuteBlurHorizontal(
                    commandContext,
                    frameIndex);
            },
            [this, frameIndex](
                RHI::ICommandContext& commandContext,
                const RenderGraphPassResources&)
            {
                m_caustics.ExecuteBlurVertical(
                    commandContext,
                    frameIndex);
            });
    }
    m_lastCausticsScheduled = causticsEnabled;

    surfaceResources.caustics = causticsComposite;
    ScreenSpaceFluidRenderer::AddCompositePass(
        graph,
        surfaceResources,
        surfaceCallbacks);
    return causticsEnabled && m_settings.causticsDebugView
        ? causticsComposite
        : surfaceResources.composite;
}

void FluidFeature::EndFrame(const bool executed)
{
    m_simulation.EndFrame(
        executed && m_lastSimulationScheduled,
        RHI::ResourceState::ShaderResource,
        RHI::ResourceState::ShaderResource);
    if (m_lastParticlePreviewScheduled)
    {
        m_surface.EndParticlePreviewFrame(executed);
    }
    else
    {
        m_surface.EndFrame(
            executed && m_lastSurfaceScheduled,
            executed && m_lastSurfaceScheduled,
            executed && m_lastFoamScheduled,
            executed && m_lastSurfaceScheduled);
    }
    m_caustics.EndFrame(
        executed && m_lastCausticsScheduled,
        executed
            && (m_lastParticlePreviewScheduled
                || m_lastSurfaceScheduled));
    m_lastSimulationScheduled = false;
    m_lastParticlePreviewScheduled = false;
    m_lastSurfaceScheduled = false;
    m_lastFoamScheduled = false;
    m_lastCausticsScheduled = false;
}

void FluidFeature::RequestReset()
{
    m_simulation.RequestReset();
    m_timeValid = false;
}

const FluidStatistics& FluidFeature::GetStatistics() const
{
    return m_simulation.GetStatistics();
}

std::shared_ptr<RHI::ITexture>
FluidFeature::GetCompositeTexture() const
{
    return m_settings.causticsEnabled
            && m_settings.causticsDebugView
        ? m_caustics.GetCompositeTexture()
        : m_surface.GetCompositeTexture();
}

std::shared_ptr<RHI::ITextureView>
FluidFeature::GetCompositeSampledView() const
{
    return m_settings.causticsEnabled
            && m_settings.causticsDebugView
        ? m_caustics.GetCompositeSampledView()
        : m_surface.GetCompositeSampledView();
}

bool FluidFeature::IsReady() const
{
    return m_device != nullptr
        && m_simulation.IsInitialized()
        && m_surface.IsReady()
        && m_caustics.IsReady()
        && m_environment != nullptr;
}

ScreenSpaceFluidFrameParameters
FluidFeature::BuildSurfaceParameters(
    const Scene::RenderSceneView& scene,
    const Scene::Camera& renderCamera,
    const FluidSettings& settings) const
{
    ScreenSpaceFluidFrameParameters result{};
    result.view = renderCamera.GetViewMatrix();
    result.projection = renderCamera.GetProjectionMatrix();
    result.cameraPosition = renderCamera.GetPosition();
    result.lightDirection =
        scene.GetDirectionalLight().direction;
    result.particleRadius = settings.particleRadius
        * settings.renderParticleRadiusScale;
    result.bilateralIterations =
        settings.bilateralIterations;
    result.bilateralRadius = settings.bilateralRadius;
    result.bilateralSpatialSigma =
        settings.bilateralSpatialSigma;
    result.bilateralDepthSigma =
        settings.bilateralDepthSigma;
    result.normalSmoothingRadius =
        settings.normalSmoothingRadius;
    result.silhouetteSmoothingRadius =
        settings.silhouetteSmoothingRadius;
    result.minimumDensityRatio =
        settings.minimumRenderDensityRatio;
    result.surfaceCoverageThreshold =
        settings.surfaceCoverageThreshold;
    result.foamDensityRatio = std::max(
        settings.foamDensityThreshold,
        settings.minimumRenderDensityRatio);
    result.absorption = settings.absorption;
    result.waterColor = settings.waterColor;
    result.scatteringStrength = std::max(
        (settings.scattering.x + settings.scattering.y
            + settings.scattering.z)
            / 3.0f,
        0.0f);
    result.indexOfRefraction = settings.ior;
    result.refractionScale = settings.refractionScale;
    result.reflectionStrength =
        settings.reflectionStrength;
    result.thicknessScale = settings.thicknessScale;
    result.foamIntensity = settings.foamEnabled ? 1.0f : 0.0f;
    // The screen-space foam cleanup is a single morphology pass. Map the
    // user-facing cleanup level to the minimum 3x3 neighborhood support so
    // every level has a deterministic effect without introducing another
    // full-resolution ping-pong texture.
    result.foamNeighborhoodThreshold =
        settings.foamErosionIterations == 0u
        ? 0.0f
        : std::clamp(
            0.18f
                + 0.14f * static_cast<float>(
                    settings.foamErosionIterations),
            0.0f,
            0.88f);
    result.outlineWidth = settings.toonEdgeWidth;
    result.causticsIntensity =
        settings.causticsEnabled
            && settings.causticsIntensity > 0.0f
        ? 1.0f
        : 0.0f;
    result.toonDiffuseSteps = settings.toonBands;
    result.toonReflectionSteps = std::max(
        settings.toonBands - 1u,
        1u);
    result.shadingMode =
        settings.toonEnabled
            || settings.renderMode == FluidRenderMode::Toon
        ? ScreenSpaceFluidShadingMode::Toon
        : ScreenSpaceFluidShadingMode::Realistic;
    switch (settings.renderMode)
    {
    case FluidRenderMode::Realistic:
    case FluidRenderMode::Toon:
        result.displayMode =
            ScreenSpaceFluidDisplayMode::Composite;
        break;
    case FluidRenderMode::Particles:
        result.displayMode =
            ScreenSpaceFluidDisplayMode::Mask;
        break;
    case FluidRenderMode::Depth:
        result.displayMode =
            ScreenSpaceFluidDisplayMode::RawDepth;
        break;
    case FluidRenderMode::Thickness:
        result.displayMode =
            ScreenSpaceFluidDisplayMode::Thickness;
        break;
    case FluidRenderMode::Normals:
        result.displayMode =
            ScreenSpaceFluidDisplayMode::Normal;
        break;
    case FluidRenderMode::Foam:
        result.displayMode =
            ScreenSpaceFluidDisplayMode::Foam;
        break;
    }
    return result;
}

FluidCaustics::Parameters
FluidFeature::BuildCausticsParameters(
    const Scene::RenderSceneView& scene,
    const Scene::Camera& renderCamera,
    const FluidSettings& settings) const
{
    FluidCaustics::Parameters result{};
    result.projection = renderCamera.GetProjectionMatrix();
    const DirectX::XMFLOAT3 lightDirection =
        scene.GetDirectionalLight().direction;
    const DirectX::XMVECTOR lightDirectionView =
        DirectX::XMVector3Normalize(
            DirectX::XMVector3TransformNormal(
                DirectX::XMLoadFloat3(&lightDirection),
                renderCamera.GetViewMatrix()));
    DirectX::XMStoreFloat3(
        &result.lightDirectionView,
        lightDirectionView);
    const DirectX::XMFLOAT3 worldUp{0.0f, 1.0f, 0.0f};
    const DirectX::XMVECTOR receiverUpDirectionView =
        DirectX::XMVector3Normalize(
            DirectX::XMVector3TransformNormal(
                DirectX::XMLoadFloat3(&worldUp),
                renderCamera.GetViewMatrix()));
    DirectX::XMStoreFloat3(
        &result.receiverUpDirectionView,
        receiverUpDirectionView);
    result.indexOfRefraction = settings.ior;
    result.enabled = settings.causticsEnabled;
    result.intensity = settings.causticsIntensity;
    result.refractionScalePixels =
        settings.causticsRefractionScalePixels;
    result.depthAttenuation =
        settings.causticsDepthAttenuation;
    result.focusStrength = settings.causticsFocusStrength;
    result.focusPower = settings.causticsFocusPower;
    result.blurRadius = settings.causticsBlurRadius;
    result.blurSigma = settings.causticsBlurSigma;
    return result;
}
} // namespace Prism::Renderer
