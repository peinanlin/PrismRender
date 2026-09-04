#include "Renderer/Features/InteractiveTerrain.h"

#include "Core/Assert.h"
#include "Renderer/Pipeline/PipelineCache.h"
#include "RHI/ICommandContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Prism::Renderer
{
void InteractiveTerrain::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderPath,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight)
{
    if (IsInitialized())
    {
        return;
    }
    Core::Check(
        framesInFlight > 0,
        "Interactive terrain requires at least one frame in flight.");
    m_device = &device;

    const RHI::ShaderBinary& brushShader = shaderManager.LoadShader(
        shaderPath,
        "TerrainBrushCS",
        RHI::ShaderStage::Compute,
        shaderFormat);
    const RHI::ShaderBinary& erosionShader = shaderManager.LoadShader(
        shaderPath,
        "TerrainErosionCS",
        RHI::ShaderStage::Compute,
        shaderFormat);
    const std::array stages{
        RHI::ShaderLayoutStage{
            &brushShader.reflection,
            RHI::ShaderStage::Compute},
        RHI::ShaderLayoutStage{
            &erosionShader.reflection,
            RHI::ShaderStage::Compute}};
    m_layout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(stages, {}));

    RHI::ComputePipelineDescription pipeline{};
    pipeline.descriptorSetLayout = m_layout;
    pipeline.computeShader = brushShader;
    m_brushPipeline = pipelineCache.GetOrCreateCompute(
        device,
        "Feature.InteractiveTerrain.Brush",
        pipeline);
    pipeline.computeShader = erosionShader;
    m_erosionPipeline = pipelineCache.GetOrCreateCompute(
        device,
        "Feature.InteractiveTerrain.Erosion",
        pipeline);

    RHI::TextureDescription textureDescription{};
    textureDescription.width = Resolution;
    textureDescription.height = Resolution;
    textureDescription.format = RHI::Format::Rgba16Float;
    textureDescription.usage =
        RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::UnorderedAccess;
    m_rawHeight = device.CreateTexture(textureDescription);
    m_erodedHeight = device.CreateTexture(textureDescription);
    m_rawHeight->SetDebugName("InteractiveTerrain.RawHeight");
    m_erodedHeight->SetDebugName("InteractiveTerrain.ErodedHeight");

    RHI::TextureViewDescription storageDescription{};
    storageDescription.type = RHI::TextureViewType::Storage;
    m_rawHeightStorage = device.CreateTextureView(
        m_rawHeight,
        storageDescription);
    m_erodedHeightStorage = device.CreateTextureView(
        m_erodedHeight,
        storageDescription);

    m_frames.resize(framesInFlight);
    for (FrameResources& frame : m_frames)
    {
        frame.constants = CreateConstantsBuffer();
        frame.descriptorSet = device.CreateDescriptorSet(m_layout);
        frame.descriptorSet->WriteBuffer(0, frame.constants);
        frame.descriptorSet->WriteTextureView(32, m_rawHeightStorage);
        frame.descriptorSet->WriteTextureView(33, m_erodedHeightStorage);
    }
}

void InteractiveTerrain::UpdateCommand(
    const InteractiveTerrainCommand& command)
{
    if (command.revision < m_pendingCommand.revision)
    {
        return;
    }
    m_pendingCommand = command;
}

void InteractiveTerrain::Execute(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex)
{
    Core::Check(
        IsInitialized() && frameIndex < m_frames.size(),
        "Interactive terrain execution uses an invalid frame.");
    const bool commandPending = m_needsInitialization
        || m_pendingCommand.revision != m_lastExecutedRevision;
    if (!commandPending)
    {
        return;
    }

    const bool reset = m_needsInitialization
        || m_pendingCommand.reset;
    const bool fullDispatch = reset
        || m_pendingCommand.fullUpdate;
    const float radius = std::clamp(
        m_pendingCommand.brushRadius,
        1.0f / static_cast<float>(Resolution),
        0.5f);
    std::uint32_t originX = 0;
    std::uint32_t originY = 0;
    std::uint32_t extentX = Resolution;
    std::uint32_t extentY = Resolution;
    if (!fullDispatch)
    {
        const float minimumU = std::clamp(
            m_pendingCommand.brushUv.x - radius,
            0.0f,
            1.0f);
        const float minimumV = std::clamp(
            m_pendingCommand.brushUv.y - radius,
            0.0f,
            1.0f);
        const float maximumU = std::clamp(
            m_pendingCommand.brushUv.x + radius,
            0.0f,
            1.0f);
        const float maximumV = std::clamp(
            m_pendingCommand.brushUv.y + radius,
            0.0f,
            1.0f);
        originX = static_cast<std::uint32_t>(std::floor(
            minimumU * static_cast<float>(Resolution)));
        originY = static_cast<std::uint32_t>(std::floor(
            minimumV * static_cast<float>(Resolution)));
        const std::uint32_t maximumX = std::min(
            static_cast<std::uint32_t>(std::ceil(
                maximumU * static_cast<float>(Resolution))) + 1u,
            Resolution);
        const std::uint32_t maximumY = std::min(
            static_cast<std::uint32_t>(std::ceil(
                maximumV * static_cast<float>(Resolution))) + 1u,
            Resolution);
        extentX = std::max(maximumX - originX, 1u);
        extentY = std::max(maximumY - originY, 1u);
    }

    Constants constants{};
    constants.dispatchParams = {
        originX,
        originY,
        reset ? 1u : 0u,
        m_pendingCommand.brushActive ? 1u : 0u};
    constants.brushParams = {
        m_pendingCommand.brushUv.x,
        m_pendingCommand.brushUv.y,
        radius,
        m_pendingCommand.brushDelta};
    constants.erosionParams = {
        m_pendingCommand.erosionEnabled ? 1.0f : 0.0f,
        static_cast<float>(Resolution),
        0.0f,
        0.0f};
    FrameResources& frame = m_frames[frameIndex];
    frame.constants->Update(&constants, sizeof(constants));

    const std::uint32_t groupCountX = (extentX + 7u) / 8u;
    const std::uint32_t groupCountY = (extentY + 7u) / 8u;
    commandContext.BindComputePipeline(*m_brushPipeline);
    commandContext.BindDescriptorSet(*frame.descriptorSet);
    commandContext.Dispatch(groupCountX, groupCountY, 1u);
    commandContext.GlobalBarrier({
        RHI::ResourceState::UnorderedAccess,
        RHI::ResourceState::UnorderedAccess});
    commandContext.BindComputePipeline(*m_erosionPipeline);
    commandContext.BindDescriptorSet(*frame.descriptorSet);
    commandContext.Dispatch(groupCountX, groupCountY, 1u);

    m_lastExecutedRevision = m_pendingCommand.revision;
    m_needsInitialization = false;
}

InteractiveTerrainGraphContribution
InteractiveTerrain::CreateRenderGraphContribution(
    const std::uint32_t frameIndex)
{
    Core::Check(
        IsInitialized() && frameIndex < m_frames.size(),
        "Interactive terrain graph registration uses an invalid frame.");
    InteractiveTerrainGraphContribution contribution{};
    contribution.rawHeight = m_rawHeight.get();
    contribution.erodedHeight = m_erodedHeight.get();
    contribution.rawHeightInitialState = m_rawHeightState;
    contribution.erodedHeightInitialState = m_erodedHeightState;
    contribution.execute =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            Execute(commandContext, frameIndex);
        };
    return contribution;
}

void InteractiveTerrain::AddPasses(
    RenderGraph& graph,
    TextureHandle& rawHeight,
    TextureHandle& erodedHeight,
    RenderGraph::ParameterExecuteCallback execute)
{
    Core::Check(
        static_cast<bool>(execute),
        "Interactive terrain requires a compute callback.");
    auto parameters = graph.CreatePassParameters();
    rawHeight = parameters.WriteTexture(
        rawHeight,
        RHI::ResourceState::UnorderedAccess);
    erodedHeight = parameters.WriteTexture(
        erodedHeight,
        RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "InteractiveTerrain.BrushAndErosion",
        std::move(parameters),
        std::move(execute),
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Graphics,
            true,
            false,
            false});
}

void InteractiveTerrain::EndFrame(const bool enabled)
{
    if (!enabled)
    {
        return;
    }
    m_rawHeightState = RHI::ResourceState::UnorderedAccess;
    m_erodedHeightState = RHI::ResourceState::ShaderResource;
}

void InteractiveTerrain::RequestReset() noexcept
{
    // Scene-local command revisions can restart from zero. Clear the shared
    // command watermark and rebuild the existing height resources on their
    // next enabled graph execution; no GPU resource is replaced here.
    m_pendingCommand = {};
    m_lastExecutedRevision = 0u;
    m_needsInitialization = true;
}

const std::shared_ptr<RHI::ITexture>&
InteractiveTerrain::GetHeightTexture() const
{
    return m_erodedHeight;
}

bool InteractiveTerrain::IsInitialized() const
{
    return m_device != nullptr
        && m_layout != nullptr
        && m_brushPipeline != nullptr
        && m_erosionPipeline != nullptr
        && m_rawHeight != nullptr
        && m_erodedHeight != nullptr;
}

std::shared_ptr<RHI::IBuffer>
InteractiveTerrain::CreateConstantsBuffer() const
{
    Core::Check(
        m_device != nullptr,
        "Interactive terrain constants require a device.");
    Constants constants{};
    RHI::BufferDescription description{};
    description.size = sizeof(Constants);
    description.usage = RHI::BufferUsage::Constant;
    description.memoryAccess = RHI::MemoryAccess::CpuToGpu;
    return m_device->CreateBuffer(description, &constants);
}
} // namespace Prism::Renderer
