#include "Renderer/Features/Ocean/LocalWaveGpuResources.h"
#include "Renderer/Features/Ocean/LocalWaveSimulation.h"

#include "Asset/ShaderManager.h"
#include "Core/Assert.h"
#include "RHI/ICommandContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"
#include "Renderer/Pipeline/PipelineCache.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace Prism::Renderer
{
namespace
{
constexpr RHI::Format ScalarFormat = RHI::Format::R16Float;
constexpr RHI::Format VectorFormat = RHI::Format::Rgba16Float;

struct LocalWaveConstants
{
    std::uint32_t resolution = 0u;
    float deltaSeconds = 0.0f;
    float domainSizeMeters = 1.0f;
    float cellSizeMeters = 1.0f;
    float amplitudeMultiplier = 1.0f;
    float lateralMultiplier = 1.0f;
    float waveSpeedMetersPerSecond = 6.0f;
    float damping = 0.0f;
    float foamWhitecapsThreshold = 0.0f;
    float foamGenerationThreshold = 0.0f;
    float foamGenerationAmount = 0.0f;
    float foamDissipation = 0.0f;
    float foamFalloff = 1.0f;
    std::uint32_t resetHistory = 1u;
    std::uint32_t disturbanceCount = 0u;
    std::uint32_t padding = 0u;
    DirectX::XMFLOAT2 domainCenter{};
    DirectX::XMFLOAT2 domainPadding{};
};

constexpr std::size_t MaxDisturbances = 256u;
constexpr std::uint32_t LocalWavePersistenceFrames = 600u;

LocalWaveTextureResource CreateResource(
    RHI::IGraphicsDevice& device,
    const std::uint32_t gridSize,
    const RHI::Format format,
    const char* debugName)
{
    RHI::TextureDescription description{};
    description.width = gridSize;
    description.height = gridSize;
    description.arrayLayers = 1u;
    description.mipLevels = 1u;
    description.format = format;
    description.usage = RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::UnorderedAccess;

    LocalWaveTextureResource resource{};
    resource.texture = device.CreateTexture(description);
    resource.texture->SetDebugName(debugName);

    RHI::TextureViewDescription view{};
    view.type = RHI::TextureViewType::Sampled;
    view.format = format;
    view.arrayLayerCount = 1u;
    resource.sampled = device.CreateTextureView(resource.texture, view);
    view.type = RHI::TextureViewType::Storage;
    resource.storage = device.CreateTextureView(resource.texture, view);
    return resource;
}
} // namespace

std::uint32_t LocalWaveGpuResources::ClampGridSize(
    const std::uint32_t requested) noexcept
{
    constexpr std::array<std::uint32_t, 5> Supported{
        128u, 256u, 512u, 1024u, 2048u};
    for (const std::uint32_t size : Supported)
    {
        if (requested <= size)
        {
            return size;
        }
    }
    return Supported.back();
}

float LocalWaveGpuResources::EstimateAllocatedMegabytes(
    const std::uint32_t gridSize) noexcept
{
    // Six scalar ping-pong surfaces (height, velocity, foam) plus two
    // RGBA16F vector maps (displacement and gradient).
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(gridSize) * gridSize;
    const std::uint64_t bytes = pixels * (6u * 2u + 2u * 8u);
    return static_cast<float>(bytes) / (1024.0f * 1024.0f);
}

LocalWaveTextureResource LocalWaveGpuResources::CreateTexture(
    RHI::IGraphicsDevice& device,
    const std::uint32_t gridSize,
    const RHI::Format format,
    const char* debugName)
{
    return CreateResource(device, gridSize, format, debugName);
}

bool LocalWaveGpuResources::Configure(
    RHI::IGraphicsDevice& device,
    const OceanLocalWaveSettings& settings)
{
    const std::uint32_t gridSize = ClampGridSize(settings.gridSize);
    const float domainSize = settings.domainSizeMeters > 1.0f
        ? settings.domainSizeMeters : 1.0f;
    if (gridSize == m_gridSize
        && domainSize == m_domainSizeMeters
        && IsInitialized())
    {
        return true;
    }

    std::array<LocalWaveTextureResource, 2> height{};
    std::array<LocalWaveTextureResource, 2> velocity{};
    std::array<LocalWaveTextureResource, 2> foam{};
    for (std::uint32_t ping = 0u; ping < 2u; ++ping)
    {
        height[ping] = CreateTexture(
            device, gridSize, ScalarFormat,
            ping == 0u ? "Ocean.LocalWave.HeightA"
                       : "Ocean.LocalWave.HeightB");
        velocity[ping] = CreateTexture(
            device, gridSize, ScalarFormat,
            ping == 0u ? "Ocean.LocalWave.VelocityA"
                       : "Ocean.LocalWave.VelocityB");
        foam[ping] = CreateTexture(
            device, gridSize, ScalarFormat,
            ping == 0u ? "Ocean.LocalWave.FoamA"
                       : "Ocean.LocalWave.FoamB");
    }
    LocalWaveTextureResource displacement = CreateTexture(
        device, gridSize, VectorFormat, "Ocean.LocalWave.Displacement");
    LocalWaveTextureResource gradient = CreateTexture(
        device, gridSize, VectorFormat, "Ocean.LocalWave.Gradient");

    m_height = std::move(height);
    m_velocity = std::move(velocity);
    m_foam = std::move(foam);
    m_displacement = std::move(displacement);
    m_gradient = std::move(gradient);
    m_gridSize = gridSize;
    m_domainSizeMeters = domainSize;
    m_allocatedMegabytes = EstimateAllocatedMegabytes(gridSize);
    ++m_resourceGeneration;
    m_needsInitialization = true;
#ifndef PRISM_LOCAL_WAVE_RESOURCES_NO_GPU
    RebindDescriptorSets();
#endif
    return IsInitialized();
}

#ifndef PRISM_LOCAL_WAVE_RESOURCES_NO_GPU
void LocalWaveGpuResources::InitializeGpu(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderDirectory,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight)
{
    Core::Check(framesInFlight > 0u,
        "Local wave GPU resources require at least one frame in flight.");
    const RHI::ShaderBinary& shader = shaderManager.LoadShader(
        shaderDirectory / "LocalWave.slang", "SimulateLocalWaveCS",
        RHI::ShaderStage::Compute, shaderFormat);
    const std::array stages{RHI::ShaderLayoutStage{
        &shader.reflection, RHI::ShaderStage::Compute}};
    m_device = &device;
    m_layout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(stages, {}));
    RHI::ComputePipelineDescription description{};
    description.computeShader = shader;
    description.descriptorSetLayout = m_layout;
    m_computePipeline = pipelineCache.GetOrCreateCompute(
        device, "Feature.LocalWave.Simulation", description);
    m_framesInFlight = framesInFlight;
    m_frames.clear();
    m_frames.resize(framesInFlight);
    RHI::BufferDescription disturbanceDescription{};
    disturbanceDescription.size = sizeof(LocalWaveDisturbance)
        * MaxDisturbances;
    disturbanceDescription.stride = sizeof(LocalWaveDisturbance);
    disturbanceDescription.usage = RHI::BufferUsage::Storage;
    disturbanceDescription.memoryAccess = RHI::MemoryAccess::GpuOnly;
    std::array<LocalWaveDisturbance, MaxDisturbances> zeroDisturbances{};
    m_disturbanceBuffer = device.CreateBuffer(
        disturbanceDescription, zeroDisturbances.data());
    RHI::BufferDescription uploadDescription = disturbanceDescription;
    uploadDescription.usage = RHI::BufferUsage::CopySource;
    uploadDescription.memoryAccess = RHI::MemoryAccess::CpuToGpu;
    m_disturbanceUploadBuffer = device.CreateBuffer(
        uploadDescription, zeroDisturbances.data());
    for (FrameResources& frame : m_frames)
    {
        LocalWaveConstants constants{};
        RHI::BufferDescription bufferDescription{};
        bufferDescription.size = sizeof(constants);
        bufferDescription.stride = sizeof(constants);
        bufferDescription.usage = RHI::BufferUsage::Constant;
        bufferDescription.memoryAccess = RHI::MemoryAccess::CpuToGpu;
        frame.constants = device.CreateBuffer(bufferDescription, &constants);
        for (std::uint32_t writeIndex = 0u; writeIndex < 2u; ++writeIndex)
        {
            frame.sets[writeIndex] = device.CreateDescriptorSet(m_layout);
        }
    }
    RebindDescriptorSets();
}

void LocalWaveGpuResources::RebindDescriptorSets()
{
    if (m_device == nullptr || m_layout == nullptr || !IsInitialized())
    {
        return;
    }
    for (FrameResources& frame : m_frames)
    {
        for (std::uint32_t writeIndex = 0u; writeIndex < 2u; ++writeIndex)
        {
            const std::uint32_t readIndex = 1u - writeIndex;
            auto& set = frame.sets[writeIndex];
            set->WriteBuffer(0u, frame.constants);
            set->WriteBuffer(19u, m_disturbanceBuffer);
            set->WriteTextureView(16u, m_height[readIndex].sampled);
            set->WriteTextureView(17u, m_velocity[readIndex].sampled);
            set->WriteTextureView(18u, m_foam[readIndex].sampled);
            set->WriteTextureView(32u, m_height[writeIndex].storage);
            set->WriteTextureView(33u, m_velocity[writeIndex].storage);
            set->WriteTextureView(34u, m_foam[writeIndex].storage);
            set->WriteTextureView(35u, m_displacement.storage);
            set->WriteTextureView(36u, m_gradient.storage);
        }
    }
}

void LocalWaveGpuResources::Update(
    const std::uint32_t frameIndex,
    const float deltaSeconds,
    const OceanLocalWaveSettings& settings,
    const bool paused,
    const std::span<const LocalWaveDisturbance> disturbances)
{
    m_settings = settings;
    m_frameIndex = frameIndex;
    m_deltaSeconds = deltaSeconds < 0.0f ? 0.0f
        : (deltaSeconds > 0.25f ? 0.25f : deltaSeconds);
    if (!disturbances.empty())
    {
        m_activeFramesRemaining = LocalWavePersistenceFrames;
    }
    else if (!paused && m_activeFramesRemaining > 0u)
    {
        --m_activeFramesRemaining;
    }
    m_simulationScheduled = IsGpuReady() && IsInitialized()
        && !paused && settings.enabled
        && (m_needsInitialization || m_activeFramesRemaining > 0u);
    if (!m_simulationScheduled || frameIndex >= m_frames.size())
    {
        return;
    }
    LocalWaveConstants constants{};
    constants.resolution = m_gridSize;
    constants.deltaSeconds = m_deltaSeconds;
    constants.domainSizeMeters = m_domainSizeMeters;
    constants.cellSizeMeters = m_domainSizeMeters
        / static_cast<float>(std::max(m_gridSize - 1u, 1u));
    constants.amplitudeMultiplier = settings.amplitudeMultiplier;
    constants.lateralMultiplier = settings.lateralMultiplier;
    constants.waveSpeedMetersPerSecond = 6.0f;
    constants.damping = settings.foam.dissipationSpeed;
    constants.foamWhitecapsThreshold = settings.foam.whitecapsThreshold;
    constants.foamGenerationThreshold = settings.foam.generationThreshold;
    constants.foamGenerationAmount = settings.foam.generationAmount;
    constants.foamDissipation = settings.foam.dissipationSpeed;
    constants.foamFalloff = settings.foam.falloffSpeed;
    constants.resetHistory = m_needsInitialization ? 1u : 0u;
    const std::size_t disturbanceCount = std::min(
        disturbances.size(), MaxDisturbances);
    m_disturbanceCount = static_cast<std::uint32_t>(disturbanceCount);
    constants.disturbanceCount = static_cast<std::uint32_t>(
        disturbanceCount);
    constants.domainCenter = settings.domainCenter;
    if (disturbanceCount > 0u)
    {
        m_disturbanceUploadBuffer->Update(
            disturbances.data(),
            disturbanceCount * sizeof(LocalWaveDisturbance));
    }
    m_frames[frameIndex].constants->Update(&constants, sizeof(constants));
}

LocalWaveGraphCallbacks LocalWaveGpuResources::CreateGraphCallbacks(
    const std::uint32_t frameIndex)
{
    LocalWaveGraphCallbacks callbacks{};
    callbacks.asyncCompute = m_asyncComputeEnabled;
    if (!m_simulationScheduled || frameIndex >= m_frames.size())
    {
        return callbacks;
    }
    if (m_disturbanceCount > 0u)
    {
        callbacks.copyDisturbances = [this](RHI::ICommandContext& command,
            const RenderGraphPassResources&)
        {
            ExecuteCopy(command);
        };
    }
    callbacks.simulate = [this, frameIndex](RHI::ICommandContext& command,
        const RenderGraphPassResources&)
    {
        Execute(command, frameIndex);
    };
    return callbacks;
}

LocalWaveGraphContribution
LocalWaveGpuResources::CreateRenderGraphContribution(
    const std::uint32_t frameIndex)
{
    LocalWaveGraphContribution contribution{};
    if (!IsInitialized())
    {
        return contribution;
    }
    for (std::uint32_t index = 0u; index < 2u; ++index)
    {
        contribution.height[index] = m_height[index].texture.get();
        contribution.velocity[index] = m_velocity[index].texture.get();
        contribution.foam[index] = m_foam[index].texture.get();
    }
    contribution.displacement = m_displacement.texture.get();
    contribution.gradient = m_gradient.texture.get();
    contribution.disturbanceUpload = m_disturbanceUploadBuffer.get();
    contribution.disturbance = m_disturbanceBuffer.get();
    const auto initialState = m_needsInitialization
        ? RHI::ResourceState::Undefined : RHI::ResourceState::ShaderResource;
    contribution.heightStates = {initialState, initialState};
    // The latest simulation output remains UAV until the next simulation
    // reads it; surface shading consumes only displacement/gradient maps.
    if (!m_needsInitialization)
        contribution.heightStates[m_readIndex] = RHI::ResourceState::UnorderedAccess;
    contribution.velocityStates = contribution.heightStates;
    contribution.foamStates = contribution.heightStates;
    contribution.displacementState = initialState;
    contribution.gradientState = initialState;
    contribution.readIndex = m_readIndex;
    contribution.writeIndex = 1u - m_readIndex;
    contribution.callbacks = CreateGraphCallbacks(frameIndex);
    return contribution;
}

void LocalWaveGpuResources::EndFrame(const bool enabled) noexcept
{
    if (enabled && m_simulationScheduled)
    {
        m_readIndex = 1u - m_readIndex;
        m_needsInitialization = false;
    }
    m_simulationScheduled = false;
}

void LocalWaveGpuResources::AddPasses(
    RenderGraph& graph,
    LocalWaveGraphHandles& handles,
    const LocalWaveGraphCallbacks& callbacks)
{
    if (!callbacks.simulate)
    {
        return;
    }
    auto parameters = graph.CreatePassParameters();
    parameters.ReadTexture(handles.height[handles.readIndex],
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(handles.velocity[handles.readIndex],
        RHI::ResourceState::ShaderResource);
    parameters.ReadTexture(handles.foam[handles.readIndex],
        RHI::ResourceState::ShaderResource);
    if (callbacks.copyDisturbances)
    {
        auto copyParameters = graph.CreatePassParameters();
        copyParameters.ReadBuffer(handles.disturbanceUpload,
            RHI::ResourceState::CopySource);
        handles.disturbance = copyParameters.WriteBuffer(
            handles.disturbance, RHI::ResourceState::CopyDestination);
        graph.AddParameterPass("Ocean.LocalWave.DisturbanceUpload",
            std::move(copyParameters), callbacks.copyDisturbances,
            {RenderGraph::QueueClass::Graphics, true, false, false});
    }
    parameters.ReadBuffer(handles.disturbance,
        RHI::ResourceState::ShaderResource);
    handles.height[handles.writeIndex] = parameters.WriteTexture(
        handles.height[handles.writeIndex], RHI::ResourceState::UnorderedAccess);
    handles.velocity[handles.writeIndex] = parameters.WriteTexture(
        handles.velocity[handles.writeIndex], RHI::ResourceState::UnorderedAccess);
    handles.foam[handles.writeIndex] = parameters.WriteTexture(
        handles.foam[handles.writeIndex], RHI::ResourceState::UnorderedAccess);
    handles.displacement = parameters.WriteTexture(
        handles.displacement, RHI::ResourceState::UnorderedAccess);
    handles.gradient = parameters.WriteTexture(
        handles.gradient, RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass("Ocean.LocalWave.Simulation",
        std::move(parameters), callbacks.simulate,
        {callbacks.asyncCompute ? RenderGraph::QueueClass::Compute
            : RenderGraph::QueueClass::Graphics, true, false, false});
}

void LocalWaveGpuResources::Execute(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(IsGpuReady() && frameIndex < m_frames.size(),
        "Local wave simulation is not ready.");
    RHI::ScopedDebugLabel label(commandContext,
        "Ocean.LocalWave.Simulation");
    commandContext.BindComputePipeline(*m_computePipeline);
    commandContext.BindDescriptorSet(*m_frames[frameIndex].sets[1u - m_readIndex]);
    const std::uint32_t groups = (m_gridSize + 7u) / 8u;
    commandContext.Dispatch(groups, groups, 1u);
}

void LocalWaveGpuResources::ExecuteCopy(
    RHI::ICommandContext& commandContext) const
{
    if (m_disturbanceUploadBuffer == nullptr || m_disturbanceBuffer == nullptr)
    {
        return;
    }
    commandContext.CopyBuffer(
        *m_disturbanceUploadBuffer,
        *m_disturbanceBuffer,
        sizeof(LocalWaveDisturbance) * MaxDisturbances);
}
#endif

void LocalWaveGpuResources::Reset() noexcept
{
    if (!IsInitialized())
    {
        return;
    }
    m_height = {};
    m_velocity = {};
    m_displacement = {};
    m_gradient = {};
    m_foam = {};
    m_gridSize = 0u;
    m_domainSizeMeters = 0.0f;
    m_allocatedMegabytes = 0.0f;
    m_needsInitialization = true;
    m_activeFramesRemaining = 0u;
    m_disturbanceCount = 0u;
    m_simulationScheduled = false;
    ++m_resourceGeneration;
}
} // namespace Prism::Renderer
