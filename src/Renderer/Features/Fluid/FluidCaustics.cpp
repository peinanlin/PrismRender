#include "Renderer/Features/Fluid/FluidCaustics.h"

#include "Core/Assert.h"
#include "Renderer/Pipeline/PipelineCache.h"
#include "RHI/ICommandContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Prism::Renderer
{
void FluidCaustics::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderPath,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight)
{
    Core::Check(
        framesInFlight > 0,
        "Fluid caustics require at least one frame in flight.");

    m_device = &device;
    const RHI::ShaderBinary& generateShader =
        shaderManager.LoadShader(
            shaderPath,
            "GenerateCausticsCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& blurHorizontalShader =
        shaderManager.LoadShader(
            shaderPath,
            "BlurCausticsHorizontalCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& blurVerticalShader =
        shaderManager.LoadShader(
            shaderPath,
            "BlurCausticsVerticalCS",
            RHI::ShaderStage::Compute,
            shaderFormat);

    const std::array generateStages{
        RHI::ShaderLayoutStage{
            &generateShader.reflection,
            RHI::ShaderStage::Compute}};
    const std::array blurHorizontalStages{
        RHI::ShaderLayoutStage{
            &blurHorizontalShader.reflection,
            RHI::ShaderStage::Compute}};
    const std::array blurVerticalStages{
        RHI::ShaderLayoutStage{
            &blurVerticalShader.reflection,
            RHI::ShaderStage::Compute}};
    m_generateLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(generateStages, {}));
    m_blurHorizontalLayout =
        device.CreateDescriptorSetLayout(
            RHI::BuildDescriptorSetLayout(
                blurHorizontalStages,
                {}));
    m_blurVerticalLayout =
        device.CreateDescriptorSetLayout(
            RHI::BuildDescriptorSetLayout(
                blurVerticalStages,
                {}));

    RHI::ComputePipelineDescription pipeline{};
    pipeline.computeShader = generateShader;
    pipeline.descriptorSetLayout = m_generateLayout;
    m_generatePipeline = pipelineCache.GetOrCreateCompute(
        device,
        "Feature.Fluid.Caustics.Generate",
        pipeline);
    pipeline.computeShader = blurHorizontalShader;
    pipeline.descriptorSetLayout =
        m_blurHorizontalLayout;
    m_blurHorizontalPipeline =
        pipelineCache.GetOrCreateCompute(
            device,
            "Feature.Fluid.Caustics.BlurHorizontal",
            pipeline);
    pipeline.computeShader = blurVerticalShader;
    pipeline.descriptorSetLayout =
        m_blurVerticalLayout;
    m_blurVerticalPipeline =
        pipelineCache.GetOrCreateCompute(
            device,
            "Feature.Fluid.Caustics.BlurVertical",
            pipeline);

    m_frames.resize(framesInFlight);
    for (FrameResources& frame : m_frames)
    {
        RHI::BufferDescription description{};
        description.size = sizeof(Constants);
        description.stride = sizeof(Constants);
        description.usage = RHI::BufferUsage::Constant;
        description.memoryAccess =
            RHI::MemoryAccess::CpuToGpu;
        frame.constants = device.CreateBuffer(description);
    }
}

void FluidCaustics::Resize(
    const std::uint32_t width,
    const std::uint32_t height,
    std::shared_ptr<RHI::ITextureView> fluidDepth,
    std::shared_ptr<RHI::ITextureView> fluidNormal,
    std::shared_ptr<RHI::ITextureView> sceneDepth)
{
    Core::Check(
        IsInitialized()
            && width > 0
            && height > 0
            && fluidDepth != nullptr
            && fluidNormal != nullptr
            && sceneDepth != nullptr,
        "Fluid caustics resize requires complete sampled inputs.");

    m_width = width;
    m_height = height;
    m_fluidDepth = std::move(fluidDepth);
    m_fluidNormal = std::move(fluidNormal);
    m_sceneDepth = std::move(sceneDepth);

    const auto createOutput = [this](const char* debugName)
    {
        RHI::TextureDescription description{};
        description.width = m_width;
        description.height = m_height;
        description.format = RHI::Format::Rgba16Float;
        description.usage =
            RHI::TextureUsage::ShaderResource
            | RHI::TextureUsage::UnorderedAccess;
        std::shared_ptr<RHI::ITexture> texture =
            m_device->CreateTexture(description);
        texture->SetDebugName(debugName);
        return texture;
    };
    m_raw = createOutput("Fluid.Caustics.Raw");
    m_blurPing = createOutput("Fluid.Caustics.BlurPing");
    m_composite = createOutput("Fluid.Caustics.Composite");

    RHI::TextureViewDescription sampled{};
    sampled.type = RHI::TextureViewType::Sampled;
    RHI::TextureViewDescription storage{};
    storage.type = RHI::TextureViewType::Storage;
    m_rawSampledView = m_device->CreateTextureView(
        m_raw,
        sampled);
    m_rawStorageView = m_device->CreateTextureView(
        m_raw,
        storage);
    m_blurPingSampledView =
        m_device->CreateTextureView(
            m_blurPing,
            sampled);
    m_blurPingStorageView =
        m_device->CreateTextureView(
            m_blurPing,
            storage);
    m_compositeSampledView =
        m_device->CreateTextureView(
            m_composite,
            sampled);
    m_compositeStorageView =
        m_device->CreateTextureView(
            m_composite,
            storage);

    m_rawState = RHI::ResourceState::Undefined;
    m_blurPingState = RHI::ResourceState::Undefined;
    m_compositeState = RHI::ResourceState::Undefined;
    RebuildDescriptorSets();
}

void FluidCaustics::Update(
    const std::uint32_t frameIndex,
    const Parameters& parameters)
{
    Core::Check(
        IsReady() && frameIndex < m_frames.size(),
        "Fluid caustics update uses an invalid frame.");

    Constants constants{};
    DirectX::XMVECTOR determinant{};
    const DirectX::XMMATRIX inverseProjection =
        DirectX::XMMatrixInverse(
            &determinant,
            parameters.projection);
    DirectX::XMStoreFloat4x4(
        &constants.inverseProjection,
        DirectX::XMMatrixTranspose(inverseProjection));
    constants.resolution = {
        static_cast<float>(m_width),
        static_cast<float>(m_height)};
    constants.inverseResolution = {
        1.0f / static_cast<float>(m_width),
        1.0f / static_cast<float>(m_height)};
    constants.tint = {
        std::max(parameters.tint.x, 0.0f),
        std::max(parameters.tint.y, 0.0f),
        std::max(parameters.tint.z, 0.0f)};
    constants.intensity =
        std::clamp(parameters.intensity, 0.0f, 64.0f);
    const DirectX::XMVECTOR lightDirection =
        DirectX::XMVector3Normalize(
            DirectX::XMLoadFloat3(
                &parameters.lightDirectionView));
    DirectX::XMStoreFloat3(
        &constants.lightDirectionView,
        lightDirection);
    constants.indexOfRefraction = std::clamp(
        parameters.indexOfRefraction,
        1.0001f,
        3.0f);
    const DirectX::XMVECTOR receiverUpDirection =
        DirectX::XMVector3Normalize(
            DirectX::XMLoadFloat3(
                &parameters.receiverUpDirectionView));
    DirectX::XMStoreFloat3(
        &constants.receiverUpDirectionView,
        receiverUpDirection);
    constants.refractionScalePixels =
        std::clamp(
            parameters.refractionScalePixels,
            0.0f,
            128.0f);
    constants.depthAttenuation =
        std::clamp(
            parameters.depthAttenuation,
            0.0f,
            128.0f);
    constants.focusStrength =
        std::clamp(
            parameters.focusStrength,
            0.0f,
            32.0f);
    constants.focusPower =
        std::clamp(
            parameters.focusPower,
            0.1f,
            8.0f);
    constants.depthBias =
        std::clamp(parameters.depthBias, 0.0f, 0.25f);
    constants.blurSigma =
        std::clamp(parameters.blurSigma, 0.25f, 16.0f);
    constants.blurRadius = std::min(
        parameters.blurRadius,
        MaximumBlurRadius);
    constants.enabled = parameters.enabled ? 1u : 0u;
    m_frames[frameIndex].constants->Update(
        &constants,
        sizeof(constants));
}

void FluidCaustics::ExecuteGenerate(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        IsReady()
            && frameIndex < m_frames.size()
            && m_frames[frameIndex].generateSet != nullptr,
        "Fluid caustics generation uses an invalid frame.");
    Dispatch(
        commandContext,
        *m_generatePipeline,
        *m_frames[frameIndex].generateSet);
}

void FluidCaustics::ExecuteBlurHorizontal(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        IsReady()
            && frameIndex < m_frames.size()
            && m_frames[frameIndex].blurHorizontalSet
                   != nullptr,
        "Fluid caustics horizontal blur uses an invalid frame.");
    Dispatch(
        commandContext,
        *m_blurHorizontalPipeline,
        *m_frames[frameIndex].blurHorizontalSet);
}

void FluidCaustics::ExecuteBlurVertical(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        IsReady()
            && frameIndex < m_frames.size()
            && m_frames[frameIndex].blurVerticalSet
                   != nullptr,
        "Fluid caustics vertical blur uses an invalid frame.");
    Dispatch(
        commandContext,
        *m_blurVerticalPipeline,
        *m_frames[frameIndex].blurVerticalSet);
}

void FluidCaustics::AddPasses(
    RenderGraph& graph,
    const TextureHandle fluidDepth,
    const TextureHandle fluidNormal,
    const TextureHandle sceneDepth,
    TextureHandle& raw,
    TextureHandle& blurPing,
    TextureHandle& composite,
    RenderGraph::ParameterExecuteCallback generate,
    RenderGraph::ParameterExecuteCallback blurHorizontal,
    RenderGraph::ParameterExecuteCallback blurVertical,
    const RenderGraph::QueueClass queue)
{
    Core::Check(
        static_cast<bool>(generate)
            && static_cast<bool>(blurHorizontal)
            && static_cast<bool>(blurVertical),
        "Fluid caustics require all three execute callbacks.");

    auto generateParameters = graph.CreatePassParameters();
    generateParameters.ReadTexture(
        fluidDepth,
        RHI::ResourceState::ShaderResource);
    generateParameters.ReadTexture(
        fluidNormal,
        RHI::ResourceState::ShaderResource);
    generateParameters.ReadTexture(
        sceneDepth,
        RHI::ResourceState::ShaderResource);
    raw = generateParameters.WriteTexture(
        raw,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "Fluid.Caustics.Generate",
        std::move(generateParameters),
        std::move(generate),
        RenderGraph::PassOptions{
            queue,
            true,
            false,
            true});

    auto horizontalParameters = graph.CreatePassParameters();
    horizontalParameters.ReadTexture(
        raw,
        RHI::ResourceState::ShaderResource);
    blurPing = horizontalParameters.WriteTexture(
        blurPing,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "Fluid.Caustics.BlurHorizontal",
        std::move(horizontalParameters),
        std::move(blurHorizontal),
        RenderGraph::PassOptions{
            queue,
            true,
            false,
            true});

    auto verticalParameters = graph.CreatePassParameters();
    verticalParameters.ReadTexture(
        blurPing,
        RHI::ResourceState::ShaderResource);
    composite = verticalParameters.WriteTexture(
        composite,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "Fluid.Caustics.BlurVertical",
        std::move(verticalParameters),
        std::move(blurVertical),
        RenderGraph::PassOptions{
            queue,
            true,
            false,
            true});
}

void FluidCaustics::EndFrame(
    const bool passesExecuted,
    const bool compositeRead)
{
    if (passesExecuted)
    {
        m_rawState = RHI::ResourceState::ShaderResource;
        m_blurPingState = RHI::ResourceState::ShaderResource;
    }
    if (passesExecuted || compositeRead)
    {
        m_compositeState = RHI::ResourceState::ShaderResource;
    }
}

std::shared_ptr<RHI::ITexture>
FluidCaustics::GetRawTexture() const
{
    return m_raw;
}

std::shared_ptr<RHI::ITextureView>
FluidCaustics::GetRawSampledView() const
{
    return m_rawSampledView;
}

std::shared_ptr<RHI::ITexture>
FluidCaustics::GetBlurPingTexture() const
{
    return m_blurPing;
}

std::shared_ptr<RHI::ITextureView>
FluidCaustics::GetBlurPingSampledView() const
{
    return m_blurPingSampledView;
}

std::shared_ptr<RHI::ITexture>
FluidCaustics::GetCompositeTexture() const
{
    return m_composite;
}

std::shared_ptr<RHI::ITextureView>
FluidCaustics::GetCompositeSampledView() const
{
    return m_compositeSampledView;
}

RHI::ResourceState FluidCaustics::GetRawInitialState() const
{
    return m_rawState;
}

RHI::ResourceState
FluidCaustics::GetBlurPingInitialState() const
{
    return m_blurPingState;
}

RHI::ResourceState
FluidCaustics::GetCompositeInitialState() const
{
    return m_compositeState;
}

bool FluidCaustics::IsInitialized() const
{
    return m_device != nullptr
        && m_generateLayout != nullptr
        && m_blurHorizontalLayout != nullptr
        && m_blurVerticalLayout != nullptr
        && m_generatePipeline != nullptr
        && m_blurHorizontalPipeline != nullptr
        && m_blurVerticalPipeline != nullptr
        && !m_frames.empty();
}

bool FluidCaustics::IsReady() const
{
    return IsInitialized()
        && m_width > 0
        && m_height > 0
        && m_fluidDepth != nullptr
        && m_fluidNormal != nullptr
        && m_sceneDepth != nullptr
        && m_raw != nullptr
        && m_rawSampledView != nullptr
        && m_rawStorageView != nullptr
        && m_blurPing != nullptr
        && m_blurPingSampledView != nullptr
        && m_blurPingStorageView != nullptr
        && m_composite != nullptr
        && m_compositeSampledView != nullptr
        && m_compositeStorageView != nullptr;
}

void FluidCaustics::RebuildDescriptorSets()
{
    Core::Check(
        IsReady(),
        "Fluid caustics descriptors require complete resources.");
    for (FrameResources& frame : m_frames)
    {
        frame.generateSet = m_device->CreateDescriptorSet(
            m_generateLayout);
        frame.generateSet->WriteBuffer(
            0,
            frame.constants);
        frame.generateSet->WriteTextureView(
            16,
            m_fluidDepth);
        frame.generateSet->WriteTextureView(
            17,
            m_fluidNormal);
        frame.generateSet->WriteTextureView(
            18,
            m_sceneDepth);
        frame.generateSet->WriteTextureView(
            32,
            m_rawStorageView);

        frame.blurHorizontalSet =
            m_device->CreateDescriptorSet(
                m_blurHorizontalLayout);
        frame.blurHorizontalSet->WriteBuffer(
            0,
            frame.constants);
        frame.blurHorizontalSet->WriteTextureView(
            19,
            m_rawSampledView);
        frame.blurHorizontalSet->WriteTextureView(
            32,
            m_blurPingStorageView);

        frame.blurVerticalSet =
            m_device->CreateDescriptorSet(
                m_blurVerticalLayout);
        frame.blurVerticalSet->WriteBuffer(
            0,
            frame.constants);
        frame.blurVerticalSet->WriteTextureView(
            19,
            m_blurPingSampledView);
        frame.blurVerticalSet->WriteTextureView(
            32,
            m_compositeStorageView);
    }
}

void FluidCaustics::Dispatch(
    RHI::ICommandContext& commandContext,
    const RHI::IComputePipeline& pipeline,
    const RHI::IDescriptorSet& descriptorSet) const
{
    commandContext.BindComputePipeline(pipeline);
    commandContext.BindDescriptorSet(descriptorSet);
    commandContext.Dispatch(
        (m_width + ThreadGroupSize - 1u)
            / ThreadGroupSize,
        (m_height + ThreadGroupSize - 1u)
            / ThreadGroupSize,
        1);
}
} // namespace Prism::Renderer
