#include "Renderer/Features/TemporalAntiAliasing.h"
#include "Renderer/Pipeline/PipelineCache.h"

#include "Core/Assert.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"

#include <array>
#include <cmath>
#include <utility>

namespace Prism::Renderer
{
namespace
{
float Halton(std::uint64_t index, const std::uint32_t base)
{
    float result = 0.0f;
    float fraction = 1.0f;
    while (index > 0)
    {
        fraction /= static_cast<float>(base);
        result += fraction
            * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}
} // namespace

void TemporalAntiAliasing::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderPath,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight)
{
    Core::Check(
        framesInFlight > 0,
        "Temporal AA requires at least one frame in flight.");
    m_device = &device;
    m_createdInitialState =
        RHI::ResourceState::Undefined;
    const RHI::ShaderBinary& shader =
        shaderManager.LoadShader(
            shaderPath,
            "TemporalResolveCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const std::array stages = {
        RHI::ShaderLayoutStage{
            &shader.reflection,
            RHI::ShaderStage::Compute}};
    m_descriptorSetLayout =
        device.CreateDescriptorSetLayout(
            RHI::BuildDescriptorSetLayout(
                stages,
                {}));
    RHI::ComputePipelineDescription pipeline{};
    pipeline.computeShader = shader;
    pipeline.descriptorSetLayout =
        m_descriptorSetLayout;
    m_pipeline =
        pipelineCache.GetOrCreateCompute(
            device,
            "Feature.TemporalAntiAliasing",
            pipeline);

    m_frames.resize(framesInFlight);
    for (FrameResources& frame : m_frames)
    {
        RHI::BufferDescription description{};
        description.size = sizeof(Constants);
        description.stride = sizeof(Constants);
        description.usage =
            RHI::BufferUsage::Constant;
        description.memoryAccess =
            RHI::MemoryAccess::CpuToGpu;
        frame.constants =
            device.CreateBuffer(description);
    }
}

void TemporalAntiAliasing::Resize(
    const std::uint32_t width,
    const std::uint32_t height,
    std::shared_ptr<RHI::ITexture> currentColor)
{
    Core::Check(
        m_device != nullptr
            && width > 0
            && height > 0
            && currentColor != nullptr,
        "Temporal AA resize requires initialized, non-empty resources.");
    m_width = width;
    m_height = height;
    m_currentColor = std::move(currentColor);

    RHI::TextureDescription motion{};
    motion.width = width;
    motion.height = height;
    motion.format = RHI::Format::Rg16Float;
    motion.usage =
        RHI::TextureUsage::RenderTarget
        | RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::UnorderedAccess;
    m_motionVectors =
        m_device->CreateTexture(motion);
    RHI::TextureViewDescription renderTarget{};
    renderTarget.type =
        RHI::TextureViewType::RenderTarget;
    m_motionVectorRenderTargetView =
        m_device->CreateTextureView(
            m_motionVectors,
            renderTarget);

    RHI::TextureDescription temporal{};
    temporal.width = width;
    temporal.height = height;
    temporal.format = RHI::Format::Rgba16Float;
    temporal.usage =
        RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::UnorderedAccess;
    m_resolved =
        m_device->CreateTexture(temporal);
    RHI::TextureViewDescription sampled{};
    sampled.type = RHI::TextureViewType::Sampled;
    m_resolvedSampledView =
        m_device->CreateTextureView(
            m_resolved,
            sampled);
    RHI::TextureViewDescription storage{};
    storage.type = RHI::TextureViewType::Storage;
    m_resolvedStorageView =
        m_device->CreateTextureView(
            m_resolved,
            storage);
    for (std::uint32_t index = 0;
         index < m_history.size();
         ++index)
    {
        m_history[index] =
            m_device->CreateTexture(temporal);
        m_historyStorageViews[index] =
            m_device->CreateTextureView(
                m_history[index],
                storage);
    }
    m_motionState = m_createdInitialState;
    m_resolvedState = m_createdInitialState;
    m_historyStates = {m_createdInitialState, m_createdInitialState};
    ResetHistory();
    RebuildDescriptorSets();
}

void TemporalAntiAliasing::Update(
    const std::uint32_t frameIndex,
    const bool enabled)
{
    Core::Check(
        IsInitialized()
            && frameIndex < m_frames.size(),
        "Temporal AA update uses an invalid frame.");
    Constants constants{};
    constants.resolution = {
        static_cast<float>(m_width),
        static_cast<float>(m_height)};
    constants.inverseResolution = {
        1.0f / static_cast<float>(m_width),
        1.0f / static_cast<float>(m_height)};
    constants.historyValid =
        m_historyValid ? 1u : 0u;
    constants.enabled = enabled ? 1u : 0u;
    m_frames[frameIndex].constants->Update(
        &constants,
        sizeof(constants));
}

void TemporalAntiAliasing::SetCurrentColor(
    const std::uint32_t frameIndex,
    std::shared_ptr<RHI::ITexture> currentColor)
{
    Core::Check(
        IsInitialized()
            && frameIndex < m_frames.size()
            && currentColor != nullptr,
        "Temporal AA input update requires a valid frame and texture.");
    m_currentColor = std::move(currentColor);
    for (const std::shared_ptr<RHI::IDescriptorSet>& descriptorSet :
         m_frames[frameIndex].descriptorSets)
    {
        Core::Check(
            descriptorSet != nullptr,
            "Temporal AA input update requires initialized descriptors.");
        descriptorSet->WriteTexture(
            16,
            m_currentColor);
    }
}

void TemporalAntiAliasing::Execute(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        IsInitialized()
            && frameIndex < m_frames.size(),
        "Temporal AA execution uses an invalid frame.");
    commandContext.BindComputePipeline(
        *m_pipeline);
    commandContext.BindDescriptorSet(
        *m_frames[frameIndex]
             .descriptorSets[m_historyReadIndex]);
    commandContext.Dispatch(
        (m_width + 7u) / 8u,
        (m_height + 7u) / 8u,
        1);
}

void TemporalAntiAliasing::EndFrame()
{
    const std::uint32_t writeIndex =
        1u - m_historyReadIndex;
    m_motionState =
        RHI::ResourceState::ShaderResource;
    m_resolvedState =
        RHI::ResourceState::ShaderResource;
    m_historyStates[m_historyReadIndex] =
        RHI::ResourceState::ShaderResource;
    m_historyStates[writeIndex] =
        RHI::ResourceState::UnorderedAccess;
    m_historyReadIndex = writeIndex;
    m_historyValid = true;
}

void TemporalAntiAliasing::ResetHistory()
{
    ++m_historyResetCallCount;
    m_historyReadIndex = 0;
    m_historyValid = false;
    // A logical camera/history reset does not recreate or transition textures.
    // Preserve their actual GPU states until Resize creates new resources.
}

bool TemporalAntiAliasing::IsInitialized() const
{
    return m_device != nullptr
        && m_pipeline != nullptr
        && m_motionVectors != nullptr
        && m_resolved != nullptr
        && m_history[0] != nullptr
        && m_history[1] != nullptr;
}

DirectX::XMFLOAT2
TemporalAntiAliasing::CalculateJitterNdc(
    const std::uint64_t sampleIndex) const
{
    if (m_width == 0 || m_height == 0)
    {
        return {};
    }
    const std::uint64_t sequenceIndex =
        sampleIndex % 8u + 1u;
    return {
        (Halton(sequenceIndex, 2) - 0.5f)
            * 2.0f / static_cast<float>(m_width),
        (Halton(sequenceIndex, 3) - 0.5f)
            * 2.0f / static_cast<float>(m_height)};
}

RHI::ITexture&
TemporalAntiAliasing::GetMotionVectorTexture() const
{
    Core::Check(
        m_motionVectors != nullptr,
        "Temporal AA motion vectors are unavailable.");
    return *m_motionVectors;
}

std::shared_ptr<RHI::ITexture>
TemporalAntiAliasing::GetMotionVectorTextureShared() const
{
    Core::Check(
        m_motionVectors != nullptr,
        "Temporal AA motion vectors are unavailable.");
    return m_motionVectors;
}

RHI::ITextureView&
TemporalAntiAliasing::GetMotionVectorRenderTargetView() const
{
    Core::Check(
        m_motionVectorRenderTargetView != nullptr,
        "Temporal AA motion-vector RTV is unavailable.");
    return *m_motionVectorRenderTargetView;
}

std::shared_ptr<RHI::ITexture>
TemporalAntiAliasing::GetResolvedTexture() const
{
    return m_resolved;
}

std::shared_ptr<RHI::ITextureView>
TemporalAntiAliasing::GetResolvedSampledView() const
{
    return m_resolvedSampledView;
}

RHI::ITexture&
TemporalAntiAliasing::GetHistoryReadTexture() const
{
    return *m_history[m_historyReadIndex];
}

RHI::ITexture&
TemporalAntiAliasing::GetHistoryWriteTexture() const
{
    return *m_history[1u - m_historyReadIndex];
}

RHI::ResourceState
TemporalAntiAliasing::GetMotionInitialState() const
{
    return m_motionState;
}

RHI::ResourceState
TemporalAntiAliasing::GetResolvedInitialState() const
{
    return m_resolvedState;
}

RHI::ResourceState
TemporalAntiAliasing::GetHistoryReadInitialState() const
{
    return m_historyStates[m_historyReadIndex];
}

RHI::ResourceState
TemporalAntiAliasing::GetHistoryWriteInitialState() const
{
    return m_historyStates[1u - m_historyReadIndex];
}

void TemporalAntiAliasing::RebuildDescriptorSets()
{
    Core::Check(
        m_currentColor != nullptr
            && m_motionVectors != nullptr
            && m_resolvedStorageView != nullptr,
        "Temporal AA descriptors require complete resources.");
    for (FrameResources& frame : m_frames)
    {
        for (std::uint32_t readIndex = 0;
             readIndex < 2;
             ++readIndex)
        {
            const std::uint32_t writeIndex =
                1u - readIndex;
            std::shared_ptr<RHI::IDescriptorSet>
                descriptorSet =
                    m_device->CreateDescriptorSet(
                        m_descriptorSetLayout);
            descriptorSet->WriteBuffer(
                0,
                frame.constants);
            descriptorSet->WriteTexture(
                16,
                m_currentColor);
            descriptorSet->WriteTexture(
                17,
                m_motionVectors);
            descriptorSet->WriteTexture(
                18,
                m_history[readIndex]);
            descriptorSet->WriteTextureView(
                32,
                m_resolvedStorageView);
            descriptorSet->WriteTextureView(
                33,
                m_historyStorageViews[writeIndex]);
            frame.descriptorSets[readIndex] =
                std::move(descriptorSet);
        }
    }
}
TemporalAntiAliasingGraphContribution
TemporalAntiAliasing::CreateRenderGraphContribution(
    const std::uint32_t frameIndex)
{
    TemporalAntiAliasingGraphContribution contribution{};
    contribution.motionVectors =
        &GetMotionVectorTexture();
    contribution.resolved =
        GetResolvedTexture().get();
    contribution.historyRead =
        &GetHistoryReadTexture();
    contribution.historyWrite =
        &GetHistoryWriteTexture();
    contribution.motionInitialState =
        GetMotionInitialState();
    contribution.resolvedInitialState =
        GetResolvedInitialState();
    contribution.historyReadInitialState =
        GetHistoryReadInitialState();
    contribution.historyWriteInitialState =
        GetHistoryWriteInitialState();
    contribution.execute =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            Execute(commandContext, frameIndex);
        };
    return contribution;
}

TextureHandle TemporalAntiAliasing::AddPasses(
    RenderGraph& graph,
    const TextureHandle currentColor,
    const TextureHandle motionVectors,
    const TextureHandle historyRead,
    TextureHandle& resolved,
    TextureHandle& historyWrite,
    RenderGraph::ParameterExecuteCallback execute)
{
    Core::Check(
        static_cast<bool>(execute),
        "Temporal AA requires an execute callback.");
    auto parameters = graph.CreatePassParameters();
    parameters.ReadTexture(
        currentColor,
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(
        motionVectors,
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(
        historyRead,
        RHI::ResourceState::ShaderResource);
    resolved = parameters.WriteTexture(
        resolved,
        RHI::ResourceState::UnorderedAccess);
    historyWrite = parameters.WriteTexture(
        historyWrite,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "TemporalResolve",
        std::move(parameters),
        std::move(execute),
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Compute,
            true,
            false,
            true,
            RenderGraph::ParallelRecordingContract::AuditedIndependent()});
    return resolved;
}
} // namespace Prism::Renderer
