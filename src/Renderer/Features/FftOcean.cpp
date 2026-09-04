#include "Renderer/Features/FftOcean.h"

#include "Core/Assert.h"
#include "Renderer/Pipeline/PipelineCache.h"
#include "RHI/ICommandContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"

#include <array>
#include <cmath>

namespace Prism::Renderer
{
void FftOcean::Initialize(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderPath,
    const RHI::ShaderBinaryFormat shaderFormat,
    const std::uint32_t framesInFlight)
{
    Core::Check(
        framesInFlight > 0,
        "FFT ocean requires at least one frame in flight.");
    m_device = &device;
    const RHI::ShaderBinary& generationShader =
        shaderManager.LoadShader(
            shaderPath,
            "GenerateSpectrumCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& buildShader =
        shaderManager.LoadShader(
            shaderPath,
            "BuildOceanMapsCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const RHI::ShaderBinary& downsampleShader =
        shaderManager.LoadShader(
            shaderPath.parent_path()
                / "FftOceanDownsample.hlsl",
            "DownsampleOceanMapsCS",
            RHI::ShaderStage::Compute,
            shaderFormat);
    const std::array generationStages{
        RHI::ShaderLayoutStage{
            &generationShader.reflection,
            RHI::ShaderStage::Compute}};
    const std::array buildStages{
        RHI::ShaderLayoutStage{
            &buildShader.reflection,
            RHI::ShaderStage::Compute}};
    const std::array downsampleStages{
        RHI::ShaderLayoutStage{
            &downsampleShader.reflection,
            RHI::ShaderStage::Compute}};
    m_generationLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(generationStages, {}));
    m_buildLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(buildStages, {}));
    m_downsampleLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(downsampleStages, {}));

    RHI::ComputePipelineDescription pipeline{};
    pipeline.computeShader = generationShader;
    pipeline.descriptorSetLayout = m_generationLayout;
    m_generationPipeline = pipelineCache.GetOrCreateCompute(
        device,
        "Feature.FftOcean.Spectrum",
        pipeline);
    pipeline.computeShader = buildShader;
    pipeline.descriptorSetLayout = m_buildLayout;
    m_buildPipeline = pipelineCache.GetOrCreateCompute(
        device,
        "Feature.FftOcean.BuildMaps",
        pipeline);
    pipeline.computeShader = downsampleShader;
    pipeline.descriptorSetLayout = m_downsampleLayout;
    m_downsamplePipeline = pipelineCache.GetOrCreateCompute(
        device,
        "Feature.FftOcean.DownsampleMaps",
        pipeline);

    const auto createSpectrumTexture = [&device]()
    {
        RHI::TextureDescription description{};
        description.width = Resolution;
        description.height = Resolution;
        description.format = RHI::Format::Rgba16Float;
        description.usage =
            RHI::TextureUsage::ShaderResource
            | RHI::TextureUsage::UnorderedAccess;
        return device.CreateTexture(description);
    };
    const auto createSurfaceTexture = [&device]()
    {
        RHI::TextureDescription description{};
        description.width = Resolution;
        description.height = Resolution;
        description.mipLevels = MipLevelCount;
        description.format = RHI::Format::Rgba16Float;
        description.usage =
            RHI::TextureUsage::ShaderResource
            | RHI::TextureUsage::UnorderedAccess;
        return device.CreateTexture(description);
    };
    RHI::TextureViewDescription storage{};
    storage.type = RHI::TextureViewType::Storage;
    for (std::uint32_t index = 0; index < 2; ++index)
    {
        m_spectrumA[index] = createSpectrumTexture();
        m_spectrumB[index] = createSpectrumTexture();
        m_spectrumA[index]->SetDebugName(
            "FftOcean.SpectrumA" + std::to_string(index));
        m_spectrumB[index]->SetDebugName(
            "FftOcean.SpectrumB" + std::to_string(index));
        m_spectrumAStorage[index] = device.CreateTextureView(
            m_spectrumA[index], storage);
        m_spectrumBStorage[index] = device.CreateTextureView(
            m_spectrumB[index], storage);
    }
    m_displacement = createSurfaceTexture();
    m_normalFoam = createSurfaceTexture();
    m_displacement->SetDebugName("FftOcean.Displacement");
    m_normalFoam->SetDebugName("FftOcean.NormalFoam");
    for (std::uint32_t mipLevel = 0u;
         mipLevel < MipLevelCount;
         ++mipLevel)
    {
        storage.baseMipLevel = mipLevel;
        m_displacementStorage[mipLevel] =
            device.CreateTextureView(
                m_displacement, storage);
        m_normalFoamStorage[mipLevel] =
            device.CreateTextureView(
                m_normalFoam, storage);
    }

    m_frames.resize(framesInFlight);
    for (FrameResources& frame : m_frames)
    {
        frame.generationConstants = CreateConstantsBuffer(Constants{});
        frame.generationSet = device.CreateDescriptorSet(
            m_generationLayout);
        frame.generationSet->WriteBuffer(
            0, frame.generationConstants);
        frame.generationSet->WriteTextureView(
            32, m_spectrumAStorage[0]);
        frame.generationSet->WriteTextureView(
            33, m_spectrumBStorage[0]);
    }

    Core::Check(m_fft.Configure(Resolution),
        "The legacy ocean FFT resolution must be supported.");
    m_fft.InitializeGpu(device, shaderManager, pipelineCache,
        shaderPath.parent_path() / "Ocean/OceanFft.slang",
        shaderFormat);
    m_fft.BindTextureWorkingSet(m_spectrumAStorage, m_spectrumBStorage);
    Constants buildConstants{};
    m_buildConstants = CreateConstantsBuffer(buildConstants);
    m_buildSet = device.CreateDescriptorSet(m_buildLayout);
    m_buildSet->WriteBuffer(0, m_buildConstants);
    m_buildSet->WriteTextureView(32, m_spectrumAStorage[0]);
    m_buildSet->WriteTextureView(33, m_spectrumBStorage[0]);
    m_buildSet->WriteTextureView(36, m_displacementStorage[0]);
    m_buildSet->WriteTextureView(37, m_normalFoamStorage[0]);
    for (std::uint32_t mipLevel = 0u;
         mipLevel + 1u < MipLevelCount;
         ++mipLevel)
    {
        std::shared_ptr<RHI::IDescriptorSet>& set =
            m_downsampleSets[mipLevel];
        set = device.CreateDescriptorSet(m_downsampleLayout);
        set->WriteTextureView(
            32, m_displacementStorage[mipLevel]);
        set->WriteTextureView(
            33, m_normalFoamStorage[mipLevel]);
        set->WriteTextureView(
            34, m_displacementStorage[mipLevel + 1u]);
        set->WriteTextureView(
            35, m_normalFoamStorage[mipLevel + 1u]);
    }
}

void FftOcean::Update(
    const std::uint32_t frameIndex,
    const float timeSeconds,
    const float patchLength,
    const DirectX::XMFLOAT2& windDirection,
    const float windSpeed,
    const float amplitude,
    const float choppiness)
{
    OceanSettings settings = OceanSettings::WaveWorksReference();
    settings.implementation = OceanImplementation::LegacyFft;
    settings.simulationPeriodMeters = patchLength;
    settings.baseWind.direction = windDirection;
    settings.baseWind.speed = windSpeed;
    settings.baseWind.amplitudeMultiplier = amplitude;
    settings.lateralMultiplier = choppiness;
    Update(frameIndex, timeSeconds, settings);
}

void FftOcean::Update(
    const std::uint32_t frameIndex,
    const float timeSeconds,
    const OceanSettings& settings)
{
    Core::Check(
        IsInitialized() && frameIndex < m_frames.size(),
        "FFT ocean update uses an invalid frame.");
    const auto normalized = [](const DirectX::XMFLOAT2& value,
        const DirectX::XMFLOAT2 fallback)
    {
        const float length = std::sqrt(
            value.x * value.x + value.y * value.y);
        return length > 0.0001f
            ? DirectX::XMFLOAT2{value.x / length, value.y / length}
            : fallback;
    };
    Constants constants{};
    constants.timeSeconds = timeSeconds;
    constants.baseWindDirection = normalized(
        settings.baseWind.direction, {1.0f, 0.0f});
    constants.baseWindSpeed = std::max(settings.baseWind.speed, 0.0f);
    constants.baseFetchMeters = std::max(
        settings.baseWind.fetchKilometers * 1000.0f, 0.001f);
    constants.basePeaking = std::max(
        settings.baseWind.spectrumPeaking, 1.0f);
    constants.baseAmplitude = std::max(
        settings.baseWind.amplitudeMultiplier, 0.0f);
    constants.swellDirection = normalized(
        settings.swell.direction, {0.0f, 1.0f});
    constants.swellSpeed = std::max(settings.swell.speed, 0.0f);
    constants.swellFetchMeters = std::max(
        settings.swell.fetchKilometers * 1000.0f, 0.001f);
    constants.swellPeaking = std::max(
        settings.swell.spectrumPeaking, 1.0f);
    constants.swellAmplitude = std::max(
        settings.swell.amplitudeMultiplier, 0.0f);
    constants.patchLength = std::max(
        settings.implementation == OceanImplementation::SpectralOcean
            ? settings.simulationPeriodMeters
            : settings.local.domainSizeMeters,
        1.0f);
    constants.choppiness = std::max(settings.lateralMultiplier, 0.0f);
    constants.baseCutoffLength = std::max(
        settings.baseWind.smallWavesCutoffLength, 0.0f);
    // Keep the old scene's visible Phillips fallback during migration. A
    // negative cutoff-power sentinel is private to this compatibility path;
    // the spectral implementation always receives the validated JONSWAP
    // value and never enters this branch.
    constants.baseCutoffPower = settings.implementation
        == OceanImplementation::LegacyFft
        ? -1.0f
        : std::max(settings.baseWind.smallWavesCutoffPower, 0.0f);
    constants.swellCutoffLength = std::max(
        settings.swell.smallWavesCutoffLength, 0.0f);
    constants.swellCutoffPower = std::max(
        settings.swell.smallWavesCutoffPower, 0.0f);
    constants.baseDependency = std::max(
        settings.baseWind.dependency, 0.0f);
    constants.swellDependency = std::max(
        settings.swell.dependency, 0.0f);
    m_frames[frameIndex].generationConstants->Update(
        &constants, sizeof(constants));
    m_buildConstants->Update(
        &constants, sizeof(constants));
}

void FftOcean::Execute(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(
        IsInitialized() && frameIndex < m_frames.size(),
        "FFT ocean execution uses an invalid frame.");
    const std::uint32_t groups = (Resolution + 7u) / 8u;
    commandContext.BindComputePipeline(*m_generationPipeline);
    commandContext.BindDescriptorSet(
        *m_frames[frameIndex].generationSet);
    commandContext.Dispatch(groups, groups, 1);
    commandContext.GlobalBarrier({
        RHI::ResourceState::UnorderedAccess,
        RHI::ResourceState::UnorderedAccess});

    m_fft.ExecuteInverse2D(commandContext);

    commandContext.BindComputePipeline(*m_buildPipeline);
    commandContext.BindDescriptorSet(*m_buildSet);
    commandContext.Dispatch(groups, groups, 1);
    commandContext.GlobalBarrier({
        RHI::ResourceState::UnorderedAccess,
        RHI::ResourceState::UnorderedAccess});

    commandContext.BindComputePipeline(*m_downsamplePipeline);
    for (std::uint32_t mipLevel = 0u;
         mipLevel + 1u < MipLevelCount;
         ++mipLevel)
    {
        const std::uint32_t destinationResolution =
            std::max(Resolution >> (mipLevel + 1u), 1u);
        const std::uint32_t destinationGroups =
            (destinationResolution + 7u) / 8u;
        commandContext.BindDescriptorSet(
            *m_downsampleSets[mipLevel]);
        commandContext.Dispatch(
            destinationGroups,
            destinationGroups,
            1);
        commandContext.GlobalBarrier({
            RHI::ResourceState::UnorderedAccess,
            RHI::ResourceState::UnorderedAccess});
    }
}

FftOceanGraphContribution FftOcean::CreateRenderGraphContribution(
    const std::uint32_t frameIndex)
{
    Core::Check(
        IsInitialized() && frameIndex < m_frames.size(),
        "FFT ocean graph registration uses an invalid frame.");
    FftOceanGraphContribution contribution{};
    contribution.spectrumA = {
        m_spectrumA[0].get(), m_spectrumA[1].get()};
    contribution.spectrumB = {
        m_spectrumB[0].get(), m_spectrumB[1].get()};
    contribution.displacement = m_displacement.get();
    contribution.normalFoam = m_normalFoam.get();
    contribution.spectrumAStates = m_spectrumAStates;
    contribution.spectrumBStates = m_spectrumBStates;
    contribution.displacementState = m_displacementState;
    contribution.normalFoamState = m_normalFoamState;
    contribution.execute =
        [this, frameIndex](
            RHI::ICommandContext& commandContext,
            const RenderGraphPassResources&)
        {
            Execute(commandContext, frameIndex);
        };
    return contribution;
}

void FftOcean::AddPasses(
    RenderGraph& graph,
    std::array<TextureHandle, 2>& spectrumA,
    std::array<TextureHandle, 2>& spectrumB,
    TextureHandle& displacement,
    TextureHandle& normalFoam,
    RenderGraph::ParameterExecuteCallback execute)
{
    Core::Check(
        static_cast<bool>(execute),
        "FFT ocean requires a simulation callback.");
    auto parameters = graph.CreatePassParameters();
    for (TextureHandle& texture : spectrumA)
    {
        texture = parameters.WriteTexture(
            texture, RHI::ResourceState::UnorderedAccess);
    }
    for (TextureHandle& texture : spectrumB)
    {
        texture = parameters.WriteTexture(
            texture, RHI::ResourceState::UnorderedAccess);
    }
    displacement = parameters.WriteTexture(
        displacement, RHI::ResourceState::UnorderedAccess);
    normalFoam = parameters.WriteTexture(
        normalFoam, RHI::ResourceState::UnorderedAccess);
    graph.AddParameterPass(
        "FftOcean.SpectrumAndIfft",
        std::move(parameters),
        std::move(execute),
        RenderGraph::PassOptions{
            RenderGraph::QueueClass::Graphics,
            true,
            false,
            false});
}

void FftOcean::EndFrame(const bool enabled)
{
    if (!enabled)
    {
        return;
    }
    m_spectrumAStates.fill(
        RHI::ResourceState::UnorderedAccess);
    m_spectrumBStates.fill(
        RHI::ResourceState::UnorderedAccess);
    m_displacementState = RHI::ResourceState::ShaderResource;
    m_normalFoamState = RHI::ResourceState::ShaderResource;
}

const std::shared_ptr<RHI::ITexture>&
FftOcean::GetDisplacementTexture() const
{
    return m_displacement;
}

const std::shared_ptr<RHI::ITexture>&
FftOcean::GetNormalFoamTexture() const
{
    return m_normalFoam;
}

bool FftOcean::IsInitialized() const
{
    return m_device != nullptr
        && m_generationPipeline != nullptr
        && m_fft.IsGpuReady()
        && m_buildPipeline != nullptr
        && m_downsamplePipeline != nullptr
        && m_displacement != nullptr
        && m_normalFoam != nullptr;
}

std::shared_ptr<RHI::IBuffer>
FftOcean::CreateConstantsBuffer(
    const Constants& constants) const
{
    RHI::BufferDescription description{};
    description.size = sizeof(Constants);
    description.stride = sizeof(Constants);
    description.usage = RHI::BufferUsage::Constant;
    description.memoryAccess = RHI::MemoryAccess::CpuToGpu;
    return m_device->CreateBuffer(
        description,
        &constants);
}
} // namespace Prism::Renderer
