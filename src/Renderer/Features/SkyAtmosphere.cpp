#include "Renderer/Features/SkyAtmosphere.h"

#include "Core/Assert.h"
#include "Renderer/Pipeline/PipelineCache.h"
#include "Renderer/RenderSettings.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace Prism::Renderer
{
using namespace DirectX;

void SkyAtmosphere::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderPath,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight)
{
    Core::Check(
        framesInFlight > 0,
        "Sky atmosphere requires at least one frame in flight.");
    m_device = &device;

    const RHI::ShaderBinary& transmittanceShader =
        shaderManager.LoadShader(
            shaderPath,
            "TransmittanceCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& skyViewShader =
        shaderManager.LoadShader(
            shaderPath,
            "SkyViewCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const std::array transmittanceStages = {
        RHI::ShaderLayoutStage{
            &transmittanceShader.reflection,
            RHI::ShaderStage::Compute}};
    const std::array skyViewStages = {
        RHI::ShaderLayoutStage{
            &skyViewShader.reflection,
            RHI::ShaderStage::Compute}};
    m_transmittanceLayout =
        device.CreateDescriptorSetLayout(
            RHI::BuildDescriptorSetLayout(
                transmittanceStages,
                {}));
    m_skyViewLayout =
        device.CreateDescriptorSetLayout(
            RHI::BuildDescriptorSetLayout(
                skyViewStages,
                {}));

    RHI::ComputePipelineDescription pipeline{};
    pipeline.computeShader = transmittanceShader;
    pipeline.descriptorSetLayout =
        m_transmittanceLayout;
    m_transmittancePipeline =
        pipelineCache.GetOrCreateCompute(
            device,
            "Feature.SkyAtmosphere.Transmittance",
            pipeline);
    pipeline.computeShader = skyViewShader;
    pipeline.descriptorSetLayout = m_skyViewLayout;
    m_skyViewPipeline =
        pipelineCache.GetOrCreateCompute(
            device,
            "Feature.SkyAtmosphere.SkyView",
            pipeline);

    const auto createLut = [&device](
        const std::uint32_t width,
        const std::uint32_t height)
    {
        RHI::TextureDescription description{};
        description.width = width;
        description.height = height;
        description.format = RHI::Format::Rgba16Float;
        description.usage =
            RHI::TextureUsage::ShaderResource
            | RHI::TextureUsage::UnorderedAccess;
        return device.CreateTexture(description);
    };
    m_transmittanceTexture = createLut(
        TransmittanceWidth,
        TransmittanceHeight);
    m_skyViewTexture = createLut(
        SkyViewWidth,
        SkyViewHeight);

    RHI::TextureViewDescription sampled{};
    sampled.type = RHI::TextureViewType::Sampled;
    RHI::TextureViewDescription storage{};
    storage.type = RHI::TextureViewType::Storage;
    m_transmittanceSampledView =
        device.CreateTextureView(
            m_transmittanceTexture,
            sampled);
    m_transmittanceStorageView =
        device.CreateTextureView(
            m_transmittanceTexture,
            storage);
    m_skyViewSampledView =
        device.CreateTextureView(
            m_skyViewTexture,
            sampled);
    m_skyViewStorageView =
        device.CreateTextureView(
            m_skyViewTexture,
            storage);

    // Disabled passes never produce their LUT. All statically bound consumers
    // still need an initialized, shader-readable 2D image (also on Vulkan).
    RHI::TextureDescription fallbackDescription{};
    fallbackDescription.width = fallbackDescription.height = 1u;
    fallbackDescription.format = RHI::Format::Rgba8Unorm;
    fallbackDescription.usage = RHI::TextureUsage::ShaderResource;
    const std::uint32_t black = 0u;
    RHI::TextureInitialData fallbackData{};
    fallbackData.data = &black;
    fallbackData.rowPitch = fallbackData.slicePitch = sizeof(black);
    auto fallback = device.CreateTexture(fallbackDescription, &fallbackData);
    fallback->SetDebugName("SkyAtmosphere.DisabledSkyView");
    m_disabledSkyView = device.CreateTextureView(std::move(fallback), sampled);

    RHI::SamplerDescription sampler{};
    sampler.filter = RHI::Filter::Linear;
    sampler.addressU = RHI::AddressMode::ClampToEdge;
    sampler.addressV = RHI::AddressMode::ClampToEdge;
    sampler.addressW = RHI::AddressMode::ClampToEdge;
    sampler.maxLod = 0.0f;
    m_sampler = device.CreateSampler(sampler);

    m_frames.resize(framesInFlight);
    for (FrameResources& frame : m_frames)
    {
        RHI::BufferDescription buffer{};
        buffer.size = sizeof(Constants);
        buffer.stride = sizeof(Constants);
        buffer.usage = RHI::BufferUsage::Constant;
        buffer.memoryAccess =
            RHI::MemoryAccess::CpuToGpu;
        frame.constants = device.CreateBuffer(buffer);

        frame.transmittanceDescriptorSet =
            device.CreateDescriptorSet(
                m_transmittanceLayout);
        frame.transmittanceDescriptorSet
            ->WriteBuffer(0, frame.constants);
        frame.transmittanceDescriptorSet
            ->WriteTextureView(
                32,
                m_transmittanceStorageView);

        frame.skyViewDescriptorSet =
            device.CreateDescriptorSet(m_skyViewLayout);
        frame.skyViewDescriptorSet->WriteBuffer(
            0,
            frame.constants);
        frame.skyViewDescriptorSet->WriteTextureView(
            16,
            m_transmittanceSampledView);
        frame.skyViewDescriptorSet->WriteTextureView(
            33,
            m_skyViewStorageView);
        frame.skyViewDescriptorSet->WriteSampler(
            48,
            m_sampler);
    }
}

void SkyAtmosphere::Update(
    const std::uint32_t frameIndex,
    const RenderSettings& settings,
    const XMFLOAT3& cameraPosition,
    const XMFLOAT3& sunDirection,
    const float sunIntensity)
{
    Core::Check(
        IsInitialized()
            && frameIndex < m_frames.size(),
        "Sky atmosphere update uses an invalid frame.");
    const XMVECTOR normalizedSun = XMVector3Normalize(
        XMLoadFloat3(&sunDirection));
    XMFLOAT3 normalizedSunDirection{};
    XMStoreFloat3(
        &normalizedSunDirection,
        normalizedSun);

    Constants constants{};
    constants.planetParameters = {
        settings.atmospherePlanetRadiusKm,
        settings.atmospherePlanetRadiusKm
            + settings.atmosphereHeightKm,
        std::max(
            settings.atmosphereRayleighScaleHeightKm,
            0.01f),
        std::max(
            settings.atmosphereMieScaleHeightKm,
            0.01f)};
    constants.rayleighScattering = {
        settings.atmosphereRayleighScattering.x,
        settings.atmosphereRayleighScattering.y,
        settings.atmosphereRayleighScattering.z,
        std::max(sunIntensity, 0.0f)};
    constants.mieParameters = {
        std::max(
            settings.atmosphereMieScattering,
            0.0f),
        std::max(
            settings.atmosphereMieAbsorption,
            0.0f),
        std::clamp(
            settings.atmosphereMieAnisotropy,
            -0.99f,
            0.99f),
        std::max(
            settings.atmosphereMultipleScattering,
            0.0f)};
    constants.sunDirectionCameraAltitude = {
        normalizedSunDirection.x,
        normalizedSunDirection.y,
        normalizedSunDirection.z,
        std::max(cameraPosition.y * 0.001f, 0.001f)};
    constants.lutDimensions = {
        static_cast<float>(TransmittanceWidth),
        static_cast<float>(TransmittanceHeight),
        static_cast<float>(SkyViewWidth),
        static_cast<float>(SkyViewHeight)};
    m_frames[frameIndex].constants->Update(
        &constants,
        sizeof(constants));
}

void SkyAtmosphere::ExecuteTransmittance(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        IsInitialized()
            && frameIndex < m_frames.size(),
        "Sky atmosphere transmittance uses an invalid frame.");
    commandContext.BindComputePipeline(
        *m_transmittancePipeline);
    commandContext.BindDescriptorSet(
        *m_frames[frameIndex]
             .transmittanceDescriptorSet);
    commandContext.Dispatch(
        (TransmittanceWidth + 7u) / 8u,
        (TransmittanceHeight + 7u) / 8u,
        1);
}

void SkyAtmosphere::ExecuteSkyView(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        IsInitialized()
            && frameIndex < m_frames.size(),
        "Sky atmosphere view LUT uses an invalid frame.");
    commandContext.BindComputePipeline(
        *m_skyViewPipeline);
    commandContext.BindDescriptorSet(
        *m_frames[frameIndex]
             .skyViewDescriptorSet);
    commandContext.Dispatch(
        (SkyViewWidth + 7u) / 8u,
        (SkyViewHeight + 7u) / 8u,
        1);
}

SkyAtmosphereGraphContribution
SkyAtmosphere::CreateRenderGraphContribution(
    const std::uint32_t frameIndex)
{
    Core::Check(
        IsInitialized()
            && frameIndex < m_frames.size(),
        "Sky atmosphere graph registration uses an invalid frame.");
    SkyAtmosphereGraphContribution contribution{};
    contribution.transmittance = m_transmittanceTexture.get();
    contribution.skyView = m_skyViewTexture.get();
    contribution.transmittanceInitialState = m_transmittanceState;
    contribution.skyViewInitialState = m_skyViewState;
    contribution.transmittanceExecute =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            ExecuteTransmittance(
                commandContext,
                frameIndex);
        };
    contribution.skyViewExecute =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            ExecuteSkyView(
                commandContext,
                frameIndex);
        };
    return contribution;
}

void SkyAtmosphere::AddPasses(
    RenderGraph& graph,
    TextureHandle& transmittance,
    TextureHandle& skyView,
    RenderGraph::ParameterExecuteCallback
        transmittanceExecute,
    RenderGraph::ParameterExecuteCallback
        skyViewExecute)
{
    Core::Check(
        static_cast<bool>(transmittanceExecute)
            && static_cast<bool>(skyViewExecute),
        "Sky atmosphere requires both LUT callbacks.");
    auto transmittanceParameters =
        graph.CreatePassParameters();
    transmittance =
        transmittanceParameters.WriteTexture(
            transmittance,
            RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "AtmosphereTransmittance",
        std::move(transmittanceParameters),
        std::move(transmittanceExecute),
        RenderGraph::PassOptions{
            // A frame may begin with atmosphere generation when shadows are
            // disabled. Bootstrap on the graphics queue so queue-batch
            // execution always starts from the active primary command list;
            // the dependent SkyView pass can still overlap on compute.
            RenderGraph::QueueClass::Graphics,
            true,
            false,
            true});

    auto skyViewParameters =
        graph.CreatePassParameters();
    skyViewParameters.ReadTexture(
        transmittance,
        RHI::ResourceState::ShaderResource);
    skyView = skyViewParameters.WriteTexture(
        skyView,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "AtmosphereSkyView",
        std::move(skyViewParameters),
        std::move(skyViewExecute),
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Compute,
            true,
            false,
            true,
            RenderGraph::ParallelRecordingContract::AuditedIndependent()});
}

void SkyAtmosphere::EndFrame(const bool enabled)
{
    if (!enabled)
    {
        return;
    }
    m_transmittanceState =
        RHI::ResourceState::ShaderResource;
    m_skyViewState =
        RHI::ResourceState::ShaderResource;
}

bool SkyAtmosphere::IsInitialized() const
{
    return m_device != nullptr
        && m_transmittancePipeline != nullptr
        && m_skyViewPipeline != nullptr
        && m_transmittanceTexture != nullptr
        && m_skyViewTexture != nullptr;
}

std::shared_ptr<RHI::ITexture>
SkyAtmosphere::GetSkyViewTexture() const
{
    return m_skyViewTexture;
}

std::shared_ptr<RHI::ITextureView>
SkyAtmosphere::GetSkyViewSampledView(const bool enabled) const
{
    return enabled ? m_skyViewSampledView : m_disabledSkyView;
}
} // namespace Prism::Renderer
