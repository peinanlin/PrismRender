#include "Renderer/Features/ScreenSpaceEffects.h"
#include "Renderer/Pipeline/PipelineCache.h"

#include "Core/Assert.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"

#include <array>
#include <utility>

namespace Prism::Renderer
{
void ScreenSpaceEffects::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderPath,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight)
{
    Core::Check(
        framesInFlight > 0,
        "Screen-space effects require at least one frame in flight.");
    m_device = &device;
    m_createdInitialState =
        RHI::ResourceState::Undefined;

    // A disabled producer leaves its render target uninitialized. Static SSR
    // bindings must remain valid even when the shader skips the planar branch.
    RHI::TextureDescription fallback{};
    fallback.width = fallback.height = 1u;
    fallback.format = RHI::Format::Rgba8Unorm;
    fallback.usage = RHI::TextureUsage::ShaderResource;
    const std::uint32_t black = 0u;
    RHI::TextureInitialData data{};
    data.data = &black;
    data.rowPitch = data.slicePitch = sizeof(black);
    m_disabledPlanarReflection = device.CreateTexture(fallback, &data);
    m_disabledPlanarReflection->SetDebugName("ScreenSpaceEffects.DisabledPlanarReflection");

    const std::uint32_t white = 0xffffffffu;
    data.data = &white;
    auto disabledAo = device.CreateTexture(fallback, &data);
    disabledAo->SetDebugName("ScreenSpaceEffects.DisabledAmbientOcclusion");
    RHI::TextureViewDescription sampled{};
    sampled.type = RHI::TextureViewType::Sampled;
    m_disabledAmbientOcclusion = device.CreateTextureView(std::move(disabledAo), sampled);

    const RHI::ShaderBinary& gtaoShader =
        shaderManager.LoadShader(
            shaderPath,
            "GtaoCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& reflectionShader =
        shaderManager.LoadShader(
            shaderPath,
            "ScreenSpaceReflectionsCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const std::array gtaoStages = {
        RHI::ShaderLayoutStage{
            &gtaoShader.reflection,
            RHI::ShaderStage::Compute}};
    const std::array reflectionStages = {
        RHI::ShaderLayoutStage{
            &reflectionShader.reflection,
            RHI::ShaderStage::Compute}};
    m_gtaoLayout =
        device.CreateDescriptorSetLayout(
            RHI::BuildDescriptorSetLayout(
                gtaoStages,
                {}));
    m_reflectionLayout =
        device.CreateDescriptorSetLayout(
            RHI::BuildDescriptorSetLayout(
                reflectionStages,
                {}));

    RHI::ComputePipelineDescription pipeline{};
    pipeline.computeShader = gtaoShader;
    pipeline.descriptorSetLayout = m_gtaoLayout;
    m_gtaoPipeline =
        pipelineCache.GetOrCreateCompute(
            device,
            "Feature.GTAO",
            pipeline);
    pipeline.computeShader = reflectionShader;
    pipeline.descriptorSetLayout =
        m_reflectionLayout;
    m_reflectionPipeline =
        pipelineCache.GetOrCreateCompute(
            device,
            "Feature.ScreenSpaceReflections",
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

void ScreenSpaceEffects::Resize(
    const std::uint32_t width,
    const std::uint32_t height,
    std::shared_ptr<RHI::ITexture> positionRoughness,
    std::shared_ptr<RHI::ITexture> normalMetallic,
    std::shared_ptr<RHI::ITexture> hdrColor,
    std::shared_ptr<RHI::ITexture> hiZ,
    std::shared_ptr<RHI::ITexture> planarReflection)
{
    Core::Check(
        m_device != nullptr
            && width > 0
            && height > 0
            && positionRoughness != nullptr
            && normalMetallic != nullptr
            && hdrColor != nullptr
            && hiZ != nullptr
            && planarReflection != nullptr,
        "Screen-space resize requires complete render inputs.");
    m_width = width;
    m_height = height;
    m_positionRoughness =
        std::move(positionRoughness);
    m_normalMetallic =
        std::move(normalMetallic);
    m_hdrColor = std::move(hdrColor);
    m_hiZ = std::move(hiZ);
    m_planarReflection =
        std::move(planarReflection);

    RHI::TextureViewDescription fullHiZView{};
    fullHiZView.type =
        RHI::TextureViewType::Sampled;
    fullHiZView.mipLevelCount =
        m_hiZ->GetDescription().mipLevels;
    m_hiZFullSampledView =
        m_device->CreateTextureView(
            m_hiZ,
            fullHiZView);

    RHI::TextureDescription ao{};
    ao.width = width;
    ao.height = height;
    ao.format = RHI::Format::Rgba16Float;
    ao.usage =
        RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::UnorderedAccess;
    m_ambientOcclusion =
        m_device->CreateTexture(ao);
    m_ambientOcclusion->SetDebugName("ScreenSpaceEffects.AmbientOcclusion");

    RHI::TextureDescription composite{};
    composite.width = width;
    composite.height = height;
    composite.format =
        RHI::Format::Rgba16Float;
    composite.usage =
        RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::UnorderedAccess;
    m_composite =
        m_device->CreateTexture(composite);

    RHI::TextureViewDescription sampled{};
    sampled.type = RHI::TextureViewType::Sampled;
    RHI::TextureViewDescription storage{};
    storage.type = RHI::TextureViewType::Storage;
    m_ambientOcclusionSampledView =
        m_device->CreateTextureView(
            m_ambientOcclusion,
            sampled);
    m_ambientOcclusionStorageView =
        m_device->CreateTextureView(
            m_ambientOcclusion,
            storage);
    m_compositeSampledView =
        m_device->CreateTextureView(
            m_composite,
            sampled);
    m_compositeStorageView =
        m_device->CreateTextureView(
            m_composite,
            storage);
    m_ambientOcclusionState =
        m_createdInitialState;
    m_compositeState =
        m_createdInitialState;
    RebuildDescriptorSets();
}

void ScreenSpaceEffects::Update(
    const std::uint32_t frameIndex,
    const DirectX::XMMATRIX& viewProjection,
    const DirectX::XMMATRIX& planarViewProjection,
    const DirectX::XMFLOAT3& cameraPosition,
    const bool deferredEnabled,
    const bool gtaoEnabled,
    const bool reflectionsEnabled,
    const bool planarReflectionsEnabled,
    const float reflectionPlaneHeight,
    const float planarReflectionIntensity)
{
    Core::Check(
        frameIndex < m_frames.size()
            && m_width > 0
            && m_height > 0,
        "Screen-space update uses an invalid frame.");
    Constants constants{};
    DirectX::XMStoreFloat4x4(
        &constants.viewProjection,
        DirectX::XMMatrixTranspose(
            viewProjection));
    DirectX::XMStoreFloat4x4(
        &constants.planarViewProjection,
        DirectX::XMMatrixTranspose(
            planarViewProjection));
    constants.cameraPosition =
        cameraPosition;
    constants.deferredEnabled =
        deferredEnabled ? 1u : 0u;
    constants.resolution = {
        static_cast<float>(m_width),
        static_cast<float>(m_height)};
    constants.inverseResolution = {
        1.0f / static_cast<float>(m_width),
        1.0f / static_cast<float>(m_height)};
    constants.gtaoEnabled =
        gtaoEnabled && deferredEnabled
        ? 1u
        : 0u;
    constants.reflectionsEnabled =
        reflectionsEnabled && deferredEnabled
        ? 1u
        : 0u;
    constants.planarReflectionsEnabled =
        planarReflectionsEnabled && deferredEnabled
        ? 1u
        : 0u;
    constants.reflectionPlaneHeight =
        reflectionPlaneHeight;
    constants.planarReflectionIntensity =
        planarReflectionIntensity;
    m_frames[frameIndex].constants->Update(
        &constants,
        sizeof(constants));
    // Match the graph's producer predicate; BeginFrame has retired this slot.
    // Never overwrite descriptor sets belonging to other in-flight frames.
    m_frames[frameIndex].reflectionDescriptorSet->WriteTextureView(
        20, GetAmbientOcclusionSampledView(constants.gtaoEnabled != 0u));
    m_frames[frameIndex].reflectionDescriptorSet->WriteTexture(
        21,
        constants.planarReflectionsEnabled != 0u
            ? m_planarReflection : m_disabledPlanarReflection);
}

void ScreenSpaceEffects::ExecuteGtao(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        frameIndex < m_frames.size(),
        "GTAO execution uses an invalid frame.");
    commandContext.BindComputePipeline(
        *m_gtaoPipeline);
    commandContext.BindDescriptorSet(
        *m_frames[frameIndex]
             .gtaoDescriptorSet);
    commandContext.Dispatch(
        (m_width + 7u) / 8u,
        (m_height + 7u) / 8u,
        1);
}

void ScreenSpaceEffects::ExecuteReflections(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        frameIndex < m_frames.size(),
        "SSR execution uses an invalid frame.");
    commandContext.BindComputePipeline(
        *m_reflectionPipeline);
    commandContext.BindDescriptorSet(
        *m_frames[frameIndex]
             .reflectionDescriptorSet);
    commandContext.Dispatch(
        (m_width + 7u) / 8u,
        (m_height + 7u) / 8u,
        1);
}

void ScreenSpaceEffects::EndFrame(
    const bool gtaoExecuted,
    const bool compositeExecuted)
{
    if (gtaoExecuted)
    {
        m_ambientOcclusionState =
            RHI::ResourceState::ShaderResource;
    }
    if (compositeExecuted)
    {
        m_compositeState =
            RHI::ResourceState::ShaderResource;
    }
}

RHI::ITexture&
ScreenSpaceEffects::GetAmbientOcclusionTexture() const
{
    Core::Check(
        m_ambientOcclusion != nullptr,
        "GTAO output is unavailable.");
    return *m_ambientOcclusion;
}

std::shared_ptr<RHI::ITextureView>
ScreenSpaceEffects::GetAmbientOcclusionSampledView(const bool enabled) const
{
    return enabled ? m_ambientOcclusionSampledView : m_disabledAmbientOcclusion;
}

std::shared_ptr<RHI::ITexture>
ScreenSpaceEffects::GetCompositeTexture() const
{
    return m_composite;
}

std::shared_ptr<RHI::ITextureView>
ScreenSpaceEffects::GetCompositeSampledView() const
{
    return m_compositeSampledView;
}

RHI::ResourceState
ScreenSpaceEffects::GetAmbientOcclusionInitialState() const
{
    return m_ambientOcclusionState;
}

RHI::ResourceState
ScreenSpaceEffects::GetCompositeInitialState() const
{
    return m_compositeState;
}

void ScreenSpaceEffects::RebuildDescriptorSets()
{
    Core::Check(
        m_positionRoughness != nullptr
            && m_normalMetallic != nullptr
            && m_hdrColor != nullptr
            && m_hiZFullSampledView != nullptr
            && m_planarReflection != nullptr
            && m_ambientOcclusionStorageView
                   != nullptr
            && m_compositeStorageView != nullptr,
        "Screen-space descriptors require complete resources.");
    for (FrameResources& frame : m_frames)
    {
        frame.gtaoDescriptorSet =
            m_device->CreateDescriptorSet(
                m_gtaoLayout);
        frame.gtaoDescriptorSet->WriteBuffer(
            0,
            frame.constants);
        frame.gtaoDescriptorSet->WriteTexture(
            16,
            m_positionRoughness);
        frame.gtaoDescriptorSet->WriteTexture(
            17,
            m_normalMetallic);
        frame.gtaoDescriptorSet->WriteTextureView(
            32,
            m_ambientOcclusionStorageView);

        frame.reflectionDescriptorSet =
            m_device->CreateDescriptorSet(
                m_reflectionLayout);
        frame.reflectionDescriptorSet->WriteBuffer(
            0,
            frame.constants);
        frame.reflectionDescriptorSet->WriteTexture(
            16,
            m_positionRoughness);
        frame.reflectionDescriptorSet->WriteTexture(
            17,
            m_normalMetallic);
        frame.reflectionDescriptorSet->WriteTexture(
            18,
            m_hdrColor);
        frame.reflectionDescriptorSet->WriteTextureView(
            19,
            m_hiZFullSampledView);
        frame.reflectionDescriptorSet->WriteTextureView(
            20,
            m_disabledAmbientOcclusion);
        frame.reflectionDescriptorSet->WriteTexture(
            21,
            m_disabledPlanarReflection);
        frame.reflectionDescriptorSet->WriteTextureView(
            32,
            m_compositeStorageView);
    }
}
ScreenSpaceEffectsGraphContribution
ScreenSpaceEffects::CreateRenderGraphContribution(
    const std::uint32_t frameIndex)
{
    ScreenSpaceEffectsGraphContribution contribution{};
    contribution.ambientOcclusion =
        &GetAmbientOcclusionTexture();
    contribution.composite =
        GetCompositeTexture().get();
    contribution.ambientOcclusionInitialState =
        GetAmbientOcclusionInitialState();
    contribution.compositeInitialState =
        GetCompositeInitialState();
    contribution.gtao =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            ExecuteGtao(commandContext, frameIndex);
        };
    contribution.reflections =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            ExecuteReflections(
                commandContext,
                frameIndex);
        };
    return contribution;
}

void ScreenSpaceEffects::AddGtaoPasses(
    RenderGraph& graph,
    const TextureHandle positionRoughness,
    const TextureHandle normalMetallic,
    TextureHandle& ambientOcclusion,
    RenderGraph::ParameterExecuteCallback execute)
{
    Core::Check(
        static_cast<bool>(execute),
        "GTAO requires an execute callback.");
    auto parameters = graph.CreatePassParameters();
    parameters.ReadTexture(
        positionRoughness,
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(
        normalMetallic,
        RHI::ResourceState::ShaderResource);
    ambientOcclusion = parameters.WriteTexture(
        ambientOcclusion,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "GTAO",
        std::move(parameters),
        std::move(execute),
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Compute,
            true,
            false,
            true,
            RenderGraph::ParallelRecordingContract::AuditedIndependent()});
}

void ScreenSpaceEffects::AddCompositePasses(
    RenderGraph& graph,
    const TextureHandle hdrColor,
    const TextureHandle hiZ,
    const TextureHandle positionRoughness,
    const TextureHandle normalMetallic,
    const TextureHandle ambientOcclusion,
    const TextureHandle planarReflection,
    const bool readAmbientOcclusion,
    const bool readPlanarReflection,
    TextureHandle& output,
    RenderGraph::ParameterExecuteCallback execute)
{
    Core::Check(
        static_cast<bool>(execute),
        "Screen-space composition requires an execute callback.");
    auto parameters = graph.CreatePassParameters();
    parameters.ReadTexture(
        hdrColor,
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(
        hiZ,
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(
        positionRoughness,
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(
        normalMetallic,
        RHI::ResourceState::ShaderResource);
    if (readAmbientOcclusion)
    {
        parameters.ReadTexture(
            ambientOcclusion,
            RHI::ResourceState::ShaderResource);
    }
    if (readPlanarReflection)
    {
        parameters.ReadTexture(
            planarReflection,
            RHI::ResourceState::ShaderResource);
    }
    output = parameters.WriteTexture(
        output,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "ScreenSpaceReflections",
        std::move(parameters),
        std::move(execute),
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Compute,
            true,
            false,
            true,
            RenderGraph::ParallelRecordingContract::AuditedIndependent()});
}
} // namespace Prism::Renderer
