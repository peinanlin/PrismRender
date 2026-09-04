#include "Renderer/Features/VarianceShadowMaps.h"
#include "Renderer/Pipeline/PipelineCache.h"

#include "Core/Assert.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"

#include <array>
#include <utility>

namespace Prism::Renderer
{
void VarianceShadowMaps::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderPath,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight,
    std::shared_ptr<RHI::ITexture> shadowDepth)
{
    Core::Check(
        framesInFlight > 0 && shadowDepth != nullptr,
        "Variance shadows require frames and a depth source.");
    const RHI::ShaderBinary& convertShader =
        shaderManager.LoadShader(
            shaderPath,
            "ConvertShadowMomentsCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& blurShader =
        shaderManager.LoadShader(
            shaderPath,
            "BlurShadowMomentsCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const std::array stages = {
        RHI::ShaderLayoutStage{
            &convertShader.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &blurShader.reflection,
            RHI::ShaderStage::Compute}};
    m_layout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(stages, {}));

    RHI::ComputePipelineDescription pipeline{};
    pipeline.computeShader = convertShader;
    pipeline.descriptorSetLayout = m_layout;
    m_convertPipeline = pipelineCache.GetOrCreateCompute(
        device,
        "Feature.VarianceShadow.Convert",
        pipeline);
    pipeline.computeShader = blurShader;
    m_blurPipeline = pipelineCache.GetOrCreateCompute(
        device,
        "Feature.VarianceShadow.Blur",
        pipeline);

    RHI::TextureDescription moments{};
    moments.width = Resolution;
    moments.height = Resolution;
    moments.arrayLayers = CascadeCount;
    // VSM/EVSM evaluate E[x^2] - E[x]^2. Half precision loses too many
    // mantissa bits in that subtraction and produces quantized variance bands,
    // especially after the separable blur. Keep the moments in fp32; the
    // 1024x1024 resolution already bounds the memory and bandwidth cost.
    moments.format = RHI::Format::Rgba32Float;
    moments.usage = RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::UnorderedAccess;
    m_moments = device.CreateTexture(moments);
    m_scratch = device.CreateTexture(moments);

    RHI::TextureViewDescription sampled{};
    sampled.type = RHI::TextureViewType::Sampled;
    sampled.arrayLayerCount = CascadeCount;
    RHI::TextureViewDescription storage = sampled;
    storage.type = RHI::TextureViewType::Storage;
    m_momentsSampledView = device.CreateTextureView(m_moments, sampled);
    m_momentsStorageView = device.CreateTextureView(m_moments, storage);
    m_scratchSampledView = device.CreateTextureView(m_scratch, sampled);
    m_scratchStorageView = device.CreateTextureView(m_scratch, storage);

    m_momentsState = RHI::ResourceState::Undefined;
    m_scratchState = RHI::ResourceState::Undefined;

    m_frames.resize(framesInFlight);
    for (FrameResources& frame : m_frames)
    {
        for (std::shared_ptr<RHI::IBuffer>& buffer : frame.constants)
        {
            RHI::BufferDescription description{};
            description.size = sizeof(Constants);
            description.usage = RHI::BufferUsage::Constant;
            description.memoryAccess = RHI::MemoryAccess::CpuToGpu;
            buffer = device.CreateBuffer(description);
        }

        for (std::uint32_t pass = 0; pass < 3; ++pass)
        {
            std::shared_ptr<RHI::IDescriptorSet> descriptorSet =
                device.CreateDescriptorSet(m_layout);
            descriptorSet->WriteBuffer(0, frame.constants[pass]);
            descriptorSet->WriteTexture(16, shadowDepth);
            descriptorSet->WriteTextureView(
                17,
                pass == 2u
                    ? m_scratchSampledView
                    : m_momentsSampledView);
            descriptorSet->WriteTextureView(
                32,
                pass == 1u
                    ? m_scratchStorageView
                    : m_momentsStorageView);
            frame.descriptorSets[pass] = std::move(descriptorSet);
        }
    }
}

void VarianceShadowMaps::Update(
    const std::uint32_t frameIndex,
    const bool exponential)
{
    Core::Check(
        frameIndex < m_frames.size(),
        "Variance shadow update uses an invalid frame.");
    const std::array<Constants, 3> constants = {{
        {0u, 0u, exponential ? 1u : 0u, 0u},
        {1u, 0u, exponential ? 1u : 0u, 0u},
        {0u, 1u, exponential ? 1u : 0u, 0u}}};
    for (std::uint32_t pass = 0; pass < constants.size(); ++pass)
    {
        m_frames[frameIndex].constants[pass]->Update(
            &constants[pass],
            sizeof(Constants));
    }
}

void VarianceShadowMaps::SetShadowDepth(
    std::shared_ptr<RHI::ITexture> shadowDepth)
{
    Core::Check(
        shadowDepth != nullptr,
        "Variance shadows require a valid replacement depth texture.");
    for (FrameResources& frame : m_frames)
    {
        for (std::shared_ptr<RHI::IDescriptorSet>& descriptorSet :
             frame.descriptorSets)
        {
            descriptorSet->WriteTexture(16, shadowDepth);
        }
    }
}

void VarianceShadowMaps::ExecuteConvert(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Dispatch(commandContext, frameIndex, 0u, m_convertPipeline);
}

void VarianceShadowMaps::ExecuteHorizontalBlur(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Dispatch(commandContext, frameIndex, 1u, m_blurPipeline);
}

void VarianceShadowMaps::ExecuteVerticalBlur(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Dispatch(commandContext, frameIndex, 2u, m_blurPipeline);
}

void VarianceShadowMaps::EndFrame(const bool enabled)
{
    // Lighting keeps the moments descriptor resident even in PCF/off mode.
    m_momentsState = RHI::ResourceState::ShaderResource;
    if (enabled)
    {
        m_momentsState = RHI::ResourceState::ShaderResource;
        m_scratchState = RHI::ResourceState::ShaderResource;
    }
}

RHI::ITexture& VarianceShadowMaps::GetMomentsTexture() const
{
    return *m_moments;
}

RHI::ITexture& VarianceShadowMaps::GetScratchTexture() const
{
    return *m_scratch;
}

std::shared_ptr<RHI::ITextureView>
VarianceShadowMaps::GetMomentsSampledView() const
{
    return m_momentsSampledView;
}

RHI::ResourceState VarianceShadowMaps::GetMomentsInitialState() const
{
    return m_momentsState;
}

RHI::ResourceState VarianceShadowMaps::GetScratchInitialState() const
{
    return m_scratchState;
}

void VarianceShadowMaps::Dispatch(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex,
    const std::uint32_t passIndex,
    const std::shared_ptr<RHI::IComputePipeline>& pipeline) const
{
    Core::Check(
        frameIndex < m_frames.size()
            && passIndex < 3u
            && pipeline != nullptr,
        "Variance shadow dispatch uses invalid resources.");
    commandContext.BindComputePipeline(*pipeline);
    commandContext.BindDescriptorSet(
        *m_frames[frameIndex].descriptorSets[passIndex]);
    commandContext.Dispatch(
        (Resolution + 7u) / 8u,
        (Resolution + 7u) / 8u,
        CascadeCount);
}
VarianceShadowMapsGraphContribution
VarianceShadowMaps::CreateRenderGraphContribution(
    const std::uint32_t frameIndex)
{
    VarianceShadowMapsGraphContribution contribution{};
    contribution.moments =
        &GetMomentsTexture();
    contribution.scratch =
        &GetScratchTexture();
    contribution.momentsInitialState =
        GetMomentsInitialState();
    contribution.scratchInitialState =
        GetScratchInitialState();
    contribution.convert =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            ExecuteConvert(commandContext, frameIndex);
        };
    contribution.horizontal =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            ExecuteHorizontalBlur(
                commandContext,
                frameIndex);
        };
    contribution.vertical =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            ExecuteVerticalBlur(
                commandContext,
                frameIndex);
        };
    return contribution;
}

void VarianceShadowMaps::AddPasses(
    RenderGraph& graph,
    const TextureHandle shadowMap,
    TextureHandle& shadowMoments,
    TextureHandle& shadowMomentsScratch,
    RenderGraph::ParameterExecuteCallback convert,
    RenderGraph::ParameterExecuteCallback horizontal,
    RenderGraph::ParameterExecuteCallback vertical)
{
    Core::Check(
        convert && horizontal && vertical,
        "Variance shadows require all pass callbacks.");
    auto convertParameters = graph.CreatePassParameters();
    convertParameters.ReadTexture(
        shadowMap,
        RHI::ResourceState::ShaderResource);
    shadowMoments = convertParameters.WriteTexture(
        shadowMoments,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "ShadowMoments",
        std::move(convertParameters),
        std::move(convert),
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Compute,
            true,
            false,
            true,
            RenderGraph::ParallelRecordingContract::AuditedIndependent()});

    auto horizontalParameters =
        graph.CreatePassParameters();
    horizontalParameters.ReadTexture(
        shadowMoments,
        RHI::ResourceState::ShaderResource);
    shadowMomentsScratch =
        horizontalParameters.WriteTexture(
            shadowMomentsScratch,
            RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "ShadowMomentsHorizontal",
        std::move(horizontalParameters),
        std::move(horizontal),
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Compute,
            true,
            false,
            true,
            RenderGraph::ParallelRecordingContract::AuditedIndependent()});

    auto verticalParameters =
        graph.CreatePassParameters();
    verticalParameters.ReadTexture(
        shadowMomentsScratch,
        RHI::ResourceState::ShaderResource);
    shadowMoments = verticalParameters.WriteTexture(
        shadowMoments,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "ShadowMomentsVertical",
        std::move(verticalParameters),
        std::move(vertical),
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Compute,
            true,
            false,
            true,
            RenderGraph::ParallelRecordingContract::AuditedIndependent()});
}
} // namespace Prism::Renderer
