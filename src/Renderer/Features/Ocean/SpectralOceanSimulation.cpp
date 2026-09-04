#include "Renderer/Features/Ocean/SpectralOceanSimulation.h"

#include "Asset/ShaderManager.h"
#include "Core/Assert.h"
#include "RHI/ICommandContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"
#include "Renderer/Features/Ocean/OceanFft.h"
#include "Renderer/Features/Ocean/OceanSpectrumMath.h"
#include "Renderer/Pipeline/PipelineCache.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <string>
#include <utility>

namespace Prism::Renderer
{
namespace
{
constexpr RHI::Format PublishedMapFormat = RHI::Format::Rgba16Float;

struct alignas(16) InitialSpectrumConstants
{
    std::uint32_t resolution = 0u;
    std::uint32_t randomSeed = 0u;
    std::uint32_t sampleCount = 0u;
    std::uint32_t padding = 0u;
    DirectX::XMFLOAT4 baseDirectionSpeedFetch{};
    DirectX::XMFLOAT4 basePeakingAmplitudeCutoff{};
    DirectX::XMFLOAT4 baseDependencyPadding{};
    DirectX::XMFLOAT4 swellDirectionSpeedFetch{};
    DirectX::XMFLOAT4 swellPeakingAmplitudeCutoff{};
    DirectX::XMFLOAT4 swellDependencyPadding{};
    DirectX::XMFLOAT4 cascadePatchLengths{};
    DirectX::XMFLOAT4 cascadeLowerWavelengths{};
    DirectX::XMFLOAT4 cascadeUpperWavelengths{};
};

struct alignas(16) EvolutionConstants
{
    std::uint32_t resolution = 0u;
    std::uint32_t activeCascadeMask = 0u;
    float absoluteTimeSeconds = 0.0f;
    float lateralMultiplier = 1.0f;
    DirectX::XMFLOAT4 cascadePatchLengths{};
};

struct alignas(16) MapConstants
{
    std::uint32_t resolution = 0u;
    std::uint32_t mipLevel = 0u;
    std::uint32_t destinationResolution = 0u;
    std::uint32_t padding = 0u;
    DirectX::XMFLOAT4 cascadePatchLengths{};
};

struct alignas(16) FoamConstants
{
    std::uint32_t resolution = 0u;
    std::uint32_t resetHistory = 0u;
    float deltaSeconds = 0.0f;
    float whitecapsThreshold = 0.0f;
    float generationThreshold = 0.0f;
    float generationAmount = 0.0f;
    float dissipationSpeed = 0.0f;
    float falloffSpeed = 0.0f;
    DirectX::XMFLOAT4 cascadePatchLengths{};
};

std::uint64_t MipPixelCount(const std::uint32_t resolution,
    const std::uint32_t mipLevelCount) noexcept
{
    std::uint64_t pixels = 0u;
    for (std::uint32_t mip = 0u; mip < mipLevelCount; ++mip)
    {
        const std::uint64_t extent = std::max(resolution >> mip, 1u);
        pixels += extent * extent;
    }
    return pixels;
}

std::shared_ptr<RHI::IBuffer> CreateConstantBuffer(
    RHI::IGraphicsDevice& device, const void* data, const std::size_t size)
{
    RHI::BufferDescription description{};
    description.size = size;
    description.stride = static_cast<std::uint32_t>(size);
    description.usage = RHI::BufferUsage::Constant;
    description.memoryAccess = RHI::MemoryAccess::CpuToGpu;
    return device.CreateBuffer(description, data);
}

OceanSpectrumWorkingResource CreateWorkingResource(
    RHI::IGraphicsDevice& device, const std::uint32_t resolution,
    const char* debugName)
{
    RHI::TextureDescription description{};
    description.width = resolution;
    description.height = resolution;
    description.arrayLayers = SpectralOceanSimulation::CascadeCount;
    description.mipLevels = 1u;
    description.format = SpectralOceanSimulation::PrecisionPolicy().workingFormat;
    description.usage = RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::UnorderedAccess;

    OceanSpectrumWorkingResource resource{};
    resource.texture = device.CreateTexture(description);
    resource.texture->SetDebugName(debugName);
    RHI::TextureViewDescription arrayView{};
    arrayView.type = RHI::TextureViewType::Sampled;
    arrayView.format = description.format;
    arrayView.arrayLayerCount = SpectralOceanSimulation::CascadeCount;
    resource.sampledArray = device.CreateTextureView(resource.texture, arrayView);
    arrayView.type = RHI::TextureViewType::Storage;
    resource.storageArray = device.CreateTextureView(resource.texture, arrayView);
    for (std::uint32_t cascade = 0u;
         cascade < SpectralOceanSimulation::CascadeCount; ++cascade)
    {
        RHI::TextureViewDescription slice{};
        slice.type = RHI::TextureViewType::Sampled;
        slice.format = description.format;
        slice.baseArrayLayer = cascade;
        resource.sampledSlices[cascade] =
            device.CreateTextureView(resource.texture, slice);
        slice.type = RHI::TextureViewType::Storage;
        resource.storageSlices[cascade] =
            device.CreateTextureView(resource.texture, slice);
    }
    return resource;
}

OceanPublishedMapResource CreatePublishedMap(RHI::IGraphicsDevice& device,
    const std::uint32_t resolution, const std::uint32_t mipLevelCount,
    const char* debugName)
{
    RHI::TextureDescription description{};
    description.width = resolution;
    description.height = resolution;
    description.arrayLayers = SpectralOceanSimulation::CascadeCount;
    description.mipLevels = mipLevelCount;
    description.format = PublishedMapFormat;
    description.usage = RHI::TextureUsage::ShaderResource
        | RHI::TextureUsage::UnorderedAccess;
    OceanPublishedMapResource resource{};
    resource.texture = device.CreateTexture(description);
    resource.texture->SetDebugName(debugName);

    RHI::TextureViewDescription fullView{};
    fullView.type = RHI::TextureViewType::Sampled;
    fullView.format = description.format;
    fullView.mipLevelCount = mipLevelCount;
    fullView.arrayLayerCount = SpectralOceanSimulation::CascadeCount;
    resource.sampledArray = device.CreateTextureView(resource.texture, fullView);
    for (std::uint32_t cascade = 0u;
         cascade < SpectralOceanSimulation::CascadeCount; ++cascade)
    {
        RHI::TextureViewDescription slice = fullView;
        slice.baseArrayLayer = cascade;
        slice.arrayLayerCount = 1u;
        resource.sampledSlices[cascade] =
            device.CreateTextureView(resource.texture, slice);
    }
    resource.sampledMips.reserve(mipLevelCount);
    resource.storageMips.reserve(mipLevelCount);
    for (std::uint32_t mip = 0u; mip < mipLevelCount; ++mip)
    {
        RHI::TextureViewDescription mipView{};
        mipView.type = RHI::TextureViewType::Sampled;
        mipView.format = description.format;
        mipView.baseMipLevel = mip;
        mipView.arrayLayerCount = SpectralOceanSimulation::CascadeCount;
        resource.sampledMips.push_back(
            device.CreateTextureView(resource.texture, mipView));
        mipView.type = RHI::TextureViewType::Storage;
        resource.storageMips.push_back(
            device.CreateTextureView(resource.texture, mipView));
    }
    return resource;
}

std::shared_ptr<RHI::IComputePipeline> CreateComputePipeline(
    RHI::IGraphicsDevice& device, PipelineCache& pipelineCache,
    const std::string& name, const RHI::ShaderBinary& shader,
    const std::shared_ptr<RHI::IDescriptorSetLayout>& layout)
{
    RHI::ComputePipelineDescription description{};
    description.computeShader = shader;
    description.descriptorSetLayout = layout;
    return pipelineCache.GetOrCreateCompute(device, name, description);
}

DirectX::XMFLOAT4 CascadeValues(
    const std::array<OceanCascadeDescription, 4>& cascades,
    const float OceanCascadeDescription::* member)
{
    return {cascades[0].*member, cascades[1].*member,
        cascades[2].*member, cascades[3].*member};
}
} // namespace

struct SpectralOceanSimulation::GpuState
{
    struct FrameResources
    {
        std::shared_ptr<RHI::IBuffer> initialConstants;
        std::shared_ptr<RHI::IBuffer> evolutionConstants;
        std::shared_ptr<RHI::IBuffer> foamConstants;
        std::shared_ptr<RHI::IDescriptorSet> initialSet;
        std::shared_ptr<RHI::IDescriptorSet> evolutionSet;
        std::array<std::shared_ptr<RHI::IDescriptorSet>, 2> foamSets;
    };

    OceanSimulationQuality quality = OceanSimulationQuality::Normal;
    std::uint32_t resolution = 0u;
    std::uint32_t mipLevelCount = 0u;
    std::uint64_t generation = 0u;
    bool pipelinesReady = false;
    OceanSpectrumWorkingResource initialSpectrum;
    std::array<OceanSpectrumWorkingResource, WorkingPingCount> spectrumA;
    std::array<OceanSpectrumWorkingResource, WorkingPingCount> spectrumB;
    OceanPublishedMapResource displacement;
    OceanPublishedMapResource gradient;
    OceanPublishedMapResource moments;
    std::array<OceanPublishedMapResource, 2> foamHistory;
    OceanFft fft;

    std::shared_ptr<RHI::IDescriptorSetLayout> initialLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> evolutionLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> buildLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> mipLayout;
    std::shared_ptr<RHI::IDescriptorSetLayout> foamLayout;
    std::shared_ptr<RHI::IComputePipeline> initialPipeline;
    std::shared_ptr<RHI::IComputePipeline> evolutionPipeline;
    std::shared_ptr<RHI::IComputePipeline> buildPipeline;
    std::shared_ptr<RHI::IComputePipeline> mipPipeline;
    std::shared_ptr<RHI::IComputePipeline> foamPipeline;
    std::vector<FrameResources> frames;
    std::shared_ptr<RHI::IBuffer> buildConstants;
    std::shared_ptr<RHI::IDescriptorSet> buildSet;
    std::vector<std::shared_ptr<RHI::IBuffer>> mipConstants;
    std::array<std::vector<std::shared_ptr<RHI::IDescriptorSet>>, 2> mipSets;
    RHI::ResourceState initialState = RHI::ResourceState::Undefined;
    std::array<RHI::ResourceState, WorkingPingCount> spectrumAStates{
        RHI::ResourceState::Undefined, RHI::ResourceState::Undefined};
    std::array<RHI::ResourceState, WorkingPingCount> spectrumBStates{
        RHI::ResourceState::Undefined, RHI::ResourceState::Undefined};
    RHI::ResourceState displacementState = RHI::ResourceState::Undefined;
    RHI::ResourceState gradientState = RHI::ResourceState::Undefined;
    RHI::ResourceState momentState = RHI::ResourceState::Undefined;
    std::array<RHI::ResourceState, 2> foamStates{
        RHI::ResourceState::Undefined, RHI::ResourceState::Undefined};
};

SpectralOceanSimulation::~SpectralOceanSimulation() = default;
SpectralOceanSimulation::SpectralOceanSimulation(
    SpectralOceanSimulation&&) noexcept = default;
SpectralOceanSimulation& SpectralOceanSimulation::operator=(
    SpectralOceanSimulation&&) noexcept = default;

std::shared_ptr<SpectralOceanSimulation::GpuState>
SpectralOceanSimulation::CreateResourceOnlyState(RHI::IGraphicsDevice& device,
    const OceanSimulationQuality quality, const std::uint64_t generation)
{
    auto state = std::make_shared<GpuState>();
    state->quality = quality;
    state->resolution = OceanSettings::ResolutionForQuality(quality);
    state->mipLevelCount = std::bit_width(state->resolution);
    state->generation = generation;
    state->initialSpectrum = CreateWorkingResource(device,
        state->resolution, "SpectralOcean.InitialSpectrum");
    for (std::uint32_t ping = 0u; ping < WorkingPingCount; ++ping)
    {
        const std::string aName =
            "SpectralOcean.SpectrumA" + std::to_string(ping);
        const std::string bName =
            "SpectralOcean.SpectrumB" + std::to_string(ping);
        state->spectrumA[ping] = CreateWorkingResource(
            device, state->resolution, aName.c_str());
        state->spectrumB[ping] = CreateWorkingResource(
            device, state->resolution, bName.c_str());
    }
    state->displacement = CreatePublishedMap(device, state->resolution,
        state->mipLevelCount, "SpectralOcean.Displacement");
    state->gradient = CreatePublishedMap(device, state->resolution,
        state->mipLevelCount, "SpectralOcean.GradientNormalFolding");
    state->moments = CreatePublishedMap(device, state->resolution,
        state->mipLevelCount, "SpectralOcean.SlopeMoments");
    state->foamHistory[0] = CreatePublishedMap(device, state->resolution,
        state->mipLevelCount, "SpectralOcean.FoamHistory0");
    state->foamHistory[1] = CreatePublishedMap(device, state->resolution,
        state->mipLevelCount, "SpectralOcean.FoamHistory1");
    return state;
}

std::shared_ptr<SpectralOceanSimulation::GpuState>
SpectralOceanSimulation::CreateGpuState(const OceanSimulationQuality quality,
    const std::uint64_t generation) const
{
    Core::Check(m_device != nullptr && m_shaderManager != nullptr
            && m_pipelineCache != nullptr && m_framesInFlight > 0u,
        "Spectral ocean GPU dependencies are not initialized.");
    std::shared_ptr<GpuState> state = CreateResourceOnlyState(
        *m_device, quality, generation);
    Asset::ShaderManager& shaders = *m_shaderManager;
    const std::filesystem::path oceanDirectory =
        m_shaderDirectory / "Ocean";
    const RHI::ShaderBinary& initialShader = shaders.LoadShader(
        oceanDirectory / "OceanInitialSpectrum.slang",
        "GenerateInitialSpectrumCS", RHI::ShaderStage::Compute,
        m_shaderFormat);
    const RHI::ShaderBinary& evolutionShader = shaders.LoadShader(
        oceanDirectory / "OceanSpectrumEvolution.slang",
        "EvolveSpectrumCS", RHI::ShaderStage::Compute, m_shaderFormat);
    const RHI::ShaderBinary& buildShader = shaders.LoadShader(
        oceanDirectory / "OceanBuildMaps.slang", "BuildOceanMapsCS",
        RHI::ShaderStage::Compute, m_shaderFormat);
    const RHI::ShaderBinary& mipShader = shaders.LoadShader(
        oceanDirectory / "OceanBuildMaps.slang", "DownsampleOceanMapsCS",
        RHI::ShaderStage::Compute, m_shaderFormat);
    const RHI::ShaderBinary& foamShader = shaders.LoadShader(
        oceanDirectory / "OceanFoam.slang", "UpdateSpectralFoamCS",
        RHI::ShaderStage::Compute, m_shaderFormat);
    const auto createLayout = [this](const RHI::ShaderBinary& shader)
    {
        const std::array stages{RHI::ShaderLayoutStage{
            &shader.reflection, RHI::ShaderStage::Compute}};
        return m_device->CreateDescriptorSetLayout(
            RHI::BuildDescriptorSetLayout(stages, {}));
    };
    state->initialLayout = createLayout(initialShader);
    state->evolutionLayout = createLayout(evolutionShader);
    state->buildLayout = createLayout(buildShader);
    state->mipLayout = createLayout(mipShader);
    state->foamLayout = createLayout(foamShader);
    state->initialPipeline = CreateComputePipeline(*m_device,
        *m_pipelineCache, "Feature.SpectralOcean.InitialSpectrum",
        initialShader, state->initialLayout);
    state->evolutionPipeline = CreateComputePipeline(*m_device,
        *m_pipelineCache, "Feature.SpectralOcean.Evolution",
        evolutionShader, state->evolutionLayout);
    state->buildPipeline = CreateComputePipeline(*m_device,
        *m_pipelineCache, "Feature.SpectralOcean.BuildMaps",
        buildShader, state->buildLayout);
    state->mipPipeline = CreateComputePipeline(*m_device,
        *m_pipelineCache, "Feature.SpectralOcean.Mips",
        mipShader, state->mipLayout);
    state->foamPipeline = CreateComputePipeline(*m_device,
        *m_pipelineCache, "Feature.SpectralOcean.Foam",
        foamShader, state->foamLayout);

    Core::Check(state->fft.Configure(state->resolution),
        "Spectral ocean quality selected an unsupported FFT resolution.");
    state->fft.InitializeGpu(*m_device, *m_shaderManager, *m_pipelineCache,
        oceanDirectory / "OceanFft.slang", m_shaderFormat);
    state->fft.BindTextureArrayWorkingSet(
        {state->spectrumA[0].storageArray,
            state->spectrumA[1].storageArray},
        {state->spectrumB[0].storageArray,
            state->spectrumB[1].storageArray}, CascadeCount);

    state->frames.resize(m_framesInFlight);
    for (GpuState::FrameResources& frame : state->frames)
    {
        const InitialSpectrumConstants initial{};
        frame.initialConstants = CreateConstantBuffer(
            *m_device, &initial, sizeof(initial));
        const EvolutionConstants evolution{};
        frame.evolutionConstants = CreateConstantBuffer(
            *m_device, &evolution, sizeof(evolution));
        const FoamConstants foam{};
        frame.foamConstants = CreateConstantBuffer(
            *m_device, &foam, sizeof(foam));
        frame.initialSet = m_device->CreateDescriptorSet(state->initialLayout);
        frame.initialSet->WriteBuffer(0u, frame.initialConstants);
        frame.initialSet->WriteTextureView(32u,
            state->initialSpectrum.storageArray);
        frame.evolutionSet = m_device->CreateDescriptorSet(
            state->evolutionLayout);
        frame.evolutionSet->WriteBuffer(0u, frame.evolutionConstants);
        frame.evolutionSet->WriteTextureView(16u,
            state->initialSpectrum.sampledArray);
        frame.evolutionSet->WriteTextureView(32u,
            state->spectrumA[0].storageArray);
        frame.evolutionSet->WriteTextureView(33u,
            state->spectrumB[0].storageArray);
        for (std::uint32_t writeIndex = 0u; writeIndex < 2u; ++writeIndex)
        {
            const std::uint32_t readIndex = 1u - writeIndex;
            frame.foamSets[writeIndex] = m_device->CreateDescriptorSet(
                state->foamLayout);
            frame.foamSets[writeIndex]->WriteBuffer(0u,
                frame.foamConstants);
            frame.foamSets[writeIndex]->WriteTextureView(16u,
                state->foamHistory[readIndex].sampledMips[0]);
            frame.foamSets[writeIndex]->WriteTextureView(17u,
                state->gradient.sampledMips[0]);
            frame.foamSets[writeIndex]->WriteTextureView(18u,
                state->displacement.sampledMips[0]);
            frame.foamSets[writeIndex]->WriteTextureView(32u,
                state->foamHistory[writeIndex].storageMips[0]);
        }
    }

    const auto patchLengths = OceanSettings::ReferenceCascadePatchLengths();
    MapConstants mapConstants{};
    mapConstants.resolution = state->resolution;
    mapConstants.destinationResolution = state->resolution;
    mapConstants.cascadePatchLengths = {patchLengths[0], patchLengths[1],
        patchLengths[2], patchLengths[3]};
    state->buildConstants = CreateConstantBuffer(
        *m_device, &mapConstants, sizeof(mapConstants));
    state->buildSet = m_device->CreateDescriptorSet(state->buildLayout);
    state->buildSet->WriteBuffer(0u, state->buildConstants);
    state->buildSet->WriteTextureView(16u,
        state->spectrumA[0].sampledArray);
    state->buildSet->WriteTextureView(17u,
        state->spectrumB[0].sampledArray);
    state->buildSet->WriteTextureView(32u,
        state->displacement.storageMips[0]);
    state->buildSet->WriteTextureView(33u,
        state->gradient.storageMips[0]);
    state->buildSet->WriteTextureView(34u,
        state->moments.storageMips[0]);

    state->mipConstants.reserve(state->mipLevelCount - 1u);
    for (auto& sets : state->mipSets)
    {
        sets.reserve(state->mipLevelCount - 1u);
    }
    for (std::uint32_t sourceMip = 0u;
         sourceMip + 1u < state->mipLevelCount; ++sourceMip)
    {
        MapConstants mipConstants = mapConstants;
        mipConstants.mipLevel = sourceMip + 1u;
        mipConstants.destinationResolution = std::max(
            state->resolution >> (sourceMip + 1u), 1u);
        auto constantBuffer = CreateConstantBuffer(
            *m_device, &mipConstants, sizeof(mipConstants));
        state->mipConstants.push_back(std::move(constantBuffer));
        for (std::uint32_t foamIndex = 0u; foamIndex < 2u; ++foamIndex)
        {
            auto set = m_device->CreateDescriptorSet(state->mipLayout);
            set->WriteBuffer(0u, state->mipConstants.back());
            set->WriteTextureView(35u,
                state->displacement.storageMips[sourceMip]);
            set->WriteTextureView(36u,
                state->gradient.storageMips[sourceMip]);
            set->WriteTextureView(37u,
                state->moments.storageMips[sourceMip]);
            set->WriteTextureView(38u,
                state->foamHistory[foamIndex].storageMips[sourceMip]);
            set->WriteTextureView(32u,
                state->displacement.storageMips[sourceMip + 1u]);
            set->WriteTextureView(33u,
                state->gradient.storageMips[sourceMip + 1u]);
            set->WriteTextureView(34u,
                state->moments.storageMips[sourceMip + 1u]);
            set->WriteTextureView(39u,
                state->foamHistory[foamIndex].storageMips[sourceMip + 1u]);
            state->mipSets[foamIndex].push_back(std::move(set));
        }
    }
    state->pipelinesReady = true;
    return state;
}

void SpectralOceanSimulation::InitializeWorkingResources(
    RHI::IGraphicsDevice& device, const OceanSimulationQuality quality)
{
    Reset();
    m_device = &device;
    m_resourceGeneration = 1u;
    m_historyVersion = 1u;
    m_gpu = CreateResourceOnlyState(device, quality, m_resourceGeneration);
    RefreshResourceStatistics(false);
}

void SpectralOceanSimulation::InitializeGpu(RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager, PipelineCache& pipelineCache,
    const std::filesystem::path& shaderDirectory,
    const RHI::ShaderBinaryFormat shaderFormat,
    const OceanSimulationQuality quality,
    const std::uint32_t framesInFlight)
{
    Core::Check(framesInFlight > 0u,
        "Spectral ocean requires at least one frame in flight.");
    Reset();
    m_device = &device;
    m_shaderManager = &shaderManager;
    m_pipelineCache = &pipelineCache;
    m_shaderDirectory = shaderDirectory;
    m_shaderFormat = shaderFormat;
    m_framesInFlight = framesInFlight;
    m_resourceGeneration = 1u;
    m_historyVersion = 1u;
    m_gpu = CreateGpuState(quality, m_resourceGeneration);
    RefreshResourceStatistics(true);
}

void SpectralOceanSimulation::Reset() noexcept
{
    m_gpu.reset();
    m_retiredStates.clear();
    m_spectrumGenerator.Reset();
    m_device = nullptr;
    m_shaderManager = nullptr;
    m_pipelineCache = nullptr;
    m_shaderDirectory.clear();
    m_framesInFlight = 0u;
    m_activeCascadeMask = AllCascadesMask;
    m_frameSerial = 0u;
    m_resourceGeneration = 0u;
    m_historyVersion = 0u;
    m_pendingSpectrumVersion = 0u;
    m_recordedSpectrumVersion = 0u;
    m_activeSpectrumVersion = 0u;
    m_foamReadIndex = 0u;
    m_foamWriteIndex = 1u;
    m_foamResetPending = true;
    m_hasPreviousFoamTime = false;
    m_previousFoamTimeSeconds = 0.0;
    m_foamDeltaSeconds = 0.0f;
    m_statistics = {};
}

bool SpectralOceanSimulation::ConfigureSpectrum(
    const OceanSettings& settings, const std::uint32_t randomSeed) noexcept
{
    const bool changed = m_spectrumGenerator.Configure(settings, randomSeed);
    if (changed)
    {
        ++m_pendingSpectrumVersion;
    }
    return changed;
}

bool SpectralOceanSimulation::IsInitialSpectrumRebuildPending() const noexcept
{
    return m_spectrumGenerator.IsSpectrumDirty();
}

void SpectralOceanSimulation::MarkInitialSpectrumCommitted() noexcept
{
    m_spectrumGenerator.MarkSpectrumCommitted();
    m_activeSpectrumVersion = m_pendingSpectrumVersion;
}

bool SpectralOceanSimulation::Update(const std::uint32_t frameIndex,
    const double absoluteTimeSeconds, const OceanSettings& settings,
    const std::uint32_t randomSeed)
{
    Core::Check(IsGpuReady() && frameIndex < m_framesInFlight,
        "Spectral ocean update uses an invalid frame or uninitialized GPU path.");
    OceanSettings validatedSettings = settings;
    (void)validatedSettings.ValidateAndNormalize();
    ++m_frameSerial;
    RetireExpiredResourceSets();
    const bool spectrumChanged = ConfigureSpectrum(
        validatedSettings, randomSeed);
    bool resourceSetChanged = false;
    if (m_gpu->quality != validatedSettings.quality)
    {
        RetiredState retired{};
        retired.resources = m_gpu;
        retired.releaseAfterFrame = m_frameSerial + m_framesInFlight;
        m_retiredStates.push_back(std::move(retired));
        ++m_resourceGeneration;
        ++m_historyVersion;
        m_gpu = CreateGpuState(validatedSettings.quality,
            m_resourceGeneration);
        m_foamReadIndex = 0u;
        m_foamWriteIndex = 1u;
        m_foamResetPending = true;
        m_hasPreviousFoamTime = false;
        m_foamDeltaSeconds = 0.0f;
        resourceSetChanged = true;
    }
    else if (spectrumChanged)
    {
        InvalidateFoamHistory();
    }

    const double scaledTime = std::isfinite(absoluteTimeSeconds)
        ? absoluteTimeSeconds * std::max(
            static_cast<double>(validatedSettings.timeScale), 0.0)
        : 0.0;
    if (m_hasPreviousFoamTime
        && scaledTime + 1.0e-9 < m_previousFoamTimeSeconds)
    {
        InvalidateFoamHistory();
    }
    m_foamDeltaSeconds = m_hasPreviousFoamTime
        ? static_cast<float>(std::clamp(
            scaledTime - m_previousFoamTimeSeconds, 0.0, 1.0))
        : (1.0f / 60.0f);
    m_previousFoamTimeSeconds = scaledTime;
    m_hasPreviousFoamTime = true;
    m_foamWriteIndex = 1u - m_foamReadIndex;
    UpdateGpuConstants(frameIndex, absoluteTimeSeconds, validatedSettings);
    RefreshResourceStatistics(IsInitialSpectrumRebuildPending());
    return resourceSetChanged;
}

void SpectralOceanSimulation::RetireExpiredResourceSets()
{
    std::erase_if(m_retiredStates, [this](const RetiredState& retired)
    {
        return retired.releaseAfterFrame <= m_frameSerial;
    });
}

void SpectralOceanSimulation::UpdateGpuConstants(
    const std::uint32_t frameIndex, const double absoluteTimeSeconds,
    const OceanSettings& settings)
{
    Core::Check(m_gpu != nullptr && frameIndex < m_gpu->frames.size(),
        "Spectral ocean constant update uses an invalid frame.");
    const auto& cascades = m_spectrumGenerator.GetCascades();
    InitialSpectrumConstants initial{};
    initial.resolution = m_gpu->resolution;
    initial.randomSeed = m_spectrumGenerator.RandomSeed();
    const float baseWindSpeedMetersPerSecond = settings.useBeaufortScale
        ? BeaufortToMetersPerSecond(settings.baseWind.speed)
        : settings.baseWind.speed;
    initial.baseDirectionSpeedFetch = {settings.baseWind.direction.x,
        settings.baseWind.direction.y, baseWindSpeedMetersPerSecond,
        settings.baseWind.fetchKilometers * 1000.0f};
    initial.basePeakingAmplitudeCutoff = {
        settings.baseWind.spectrumPeaking,
        settings.baseWind.amplitudeMultiplier,
        settings.baseWind.smallWavesCutoffLength,
        settings.baseWind.smallWavesCutoffPower};
    initial.baseDependencyPadding = {settings.baseWind.dependency, 0, 0, 0};
    initial.swellDirectionSpeedFetch = {settings.swell.direction.x,
        settings.swell.direction.y, settings.swell.speed,
        settings.swell.fetchKilometers * 1000.0f};
    initial.swellPeakingAmplitudeCutoff = {settings.swell.spectrumPeaking,
        settings.swell.amplitudeMultiplier,
        settings.swell.smallWavesCutoffLength,
        settings.swell.smallWavesCutoffPower};
    initial.swellDependencyPadding = {settings.swell.dependency, 0, 0, 0};
    initial.cascadePatchLengths = CascadeValues(cascades,
        &OceanCascadeDescription::patchLengthMeters);
    initial.cascadeLowerWavelengths = CascadeValues(cascades,
        &OceanCascadeDescription::wavelengthMinMeters);
    initial.cascadeUpperWavelengths = CascadeValues(cascades,
        &OceanCascadeDescription::wavelengthMaxMeters);
    m_gpu->frames[frameIndex].initialConstants->Update(
        &initial, sizeof(initial));

    EvolutionConstants evolution{};
    evolution.resolution = m_gpu->resolution;
    evolution.activeCascadeMask = m_activeCascadeMask;
    evolution.absoluteTimeSeconds = std::isfinite(absoluteTimeSeconds)
        ? static_cast<float>(absoluteTimeSeconds * settings.timeScale) : 0.0f;
    evolution.lateralMultiplier = settings.lateralMultiplier;
    evolution.cascadePatchLengths = initial.cascadePatchLengths;
    m_gpu->frames[frameIndex].evolutionConstants->Update(
        &evolution, sizeof(evolution));

    FoamConstants foam{};
    foam.resolution = m_gpu->resolution;
    foam.resetHistory = m_foamResetPending ? 1u : 0u;
    foam.deltaSeconds = m_foamDeltaSeconds;
    foam.whitecapsThreshold = settings.foam.whitecapsThreshold;
    foam.generationThreshold = settings.foam.generationThreshold;
    foam.generationAmount = settings.foam.generationAmount;
    foam.dissipationSpeed = settings.foam.dissipationSpeed;
    foam.falloffSpeed = settings.foam.falloffSpeed;
    foam.cascadePatchLengths = initial.cascadePatchLengths;
    m_gpu->frames[frameIndex].foamConstants->Update(&foam, sizeof(foam));
}

void SpectralOceanSimulation::SetActiveCascadeMask(
    const std::uint32_t mask) noexcept
{
    m_activeCascadeMask = mask & AllCascadesMask;
}

std::uint32_t SpectralOceanSimulation::ActiveCascadeMask() const noexcept
{
    return m_activeCascadeMask;
}

SpectralOceanGraphCallbacks SpectralOceanSimulation::CreateGraphCallbacks(
    const std::uint32_t frameIndex)
{
    Core::Check(IsGpuReady() && frameIndex < m_framesInFlight,
        "Spectral ocean graph callbacks use an invalid frame.");
    SpectralOceanGraphCallbacks callbacks{};
    callbacks.asyncCompute = m_asyncComputeEnabled;
    callbacks.initialSpectrum = [this, frameIndex](RHI::ICommandContext& command,
        const RenderGraphPassResources&) { ExecuteInitialSpectrum(command, frameIndex); };
    callbacks.evolution = [this, frameIndex](RHI::ICommandContext& command,
        const RenderGraphPassResources&) { ExecuteEvolution(command, frameIndex); };
    callbacks.horizontalFft = [this](RHI::ICommandContext& command,
        const RenderGraphPassResources&) { ExecuteHorizontalFft(command); };
    callbacks.verticalFft = [this](RHI::ICommandContext& command,
        const RenderGraphPassResources&) { ExecuteVerticalFft(command); };
    callbacks.outputMaps = [this](RHI::ICommandContext& command,
        const RenderGraphPassResources&) { ExecuteOutputMaps(command); };
    callbacks.foam = [this, frameIndex](RHI::ICommandContext& command,
        const RenderGraphPassResources&) { ExecuteFoam(command, frameIndex); };
    callbacks.mips = [this](RHI::ICommandContext& command,
        const RenderGraphPassResources&) { ExecuteMips(command); };
    return callbacks;
}

SpectralOceanGraphContribution
SpectralOceanSimulation::CreateRenderGraphContribution(
    const std::uint32_t frameIndex)
{
    Core::Check(IsGpuReady() && frameIndex < m_framesInFlight,
        "Spectral ocean graph registration uses an invalid frame.");
    SpectralOceanGraphContribution contribution{};
    contribution.initialSpectrum = m_gpu->initialSpectrum.texture.get();
    contribution.spectrumA = {m_gpu->spectrumA[0].texture.get(),
        m_gpu->spectrumA[1].texture.get()};
    contribution.spectrumB = {m_gpu->spectrumB[0].texture.get(),
        m_gpu->spectrumB[1].texture.get()};
    contribution.displacement = m_gpu->displacement.texture.get();
    contribution.gradientFoam = m_gpu->gradient.texture.get();
    contribution.slopeMoments = m_gpu->moments.texture.get();
    contribution.foamHistory = {
        m_gpu->foamHistory[0].texture.get(),
        m_gpu->foamHistory[1].texture.get()};
    contribution.initialSpectrumState = m_gpu->initialState;
    contribution.spectrumAStates = m_gpu->spectrumAStates;
    contribution.spectrumBStates = m_gpu->spectrumBStates;
    contribution.displacementState = m_gpu->displacementState;
    contribution.gradientFoamState = m_gpu->gradientState;
    contribution.slopeMomentsState = m_gpu->momentState;
    contribution.foamHistoryStates = m_gpu->foamStates;
    contribution.foamReadIndex = m_foamReadIndex;
    contribution.foamWriteIndex = m_foamWriteIndex;
    contribution.callbacks = CreateGraphCallbacks(frameIndex);
    contribution.rebuildInitialSpectrum =
        IsInitialSpectrumRebuildPending();
    return contribution;
}

void SpectralOceanSimulation::EndFrame(const bool enabled) noexcept
{
    if (!enabled || m_gpu == nullptr)
    {
        m_recordedSpectrumVersion = 0u;
        return;
    }
    // The surface sees the new maps only after the graph's complete ordered
    // H0/evolution/IFFT/map chain. Commit the CPU version at that same frame
    // boundary, and never let an older recorded version clear a newer edit.
    if (m_recordedSpectrumVersion != 0u
        && m_recordedSpectrumVersion == m_pendingSpectrumVersion)
    {
        MarkInitialSpectrumCommitted();
    }
    m_recordedSpectrumVersion = 0u;
    m_gpu->initialState = RHI::ResourceState::ShaderResource;
    m_gpu->spectrumAStates = {RHI::ResourceState::ShaderResource,
        RHI::ResourceState::UnorderedAccess};
    m_gpu->spectrumBStates = {RHI::ResourceState::ShaderResource,
        RHI::ResourceState::UnorderedAccess};
    m_gpu->displacementState = RHI::ResourceState::ShaderResource;
    m_gpu->gradientState = RHI::ResourceState::ShaderResource;
    m_gpu->momentState = RHI::ResourceState::ShaderResource;
    m_gpu->foamStates = {RHI::ResourceState::ShaderResource,
        RHI::ResourceState::ShaderResource};
    m_foamReadIndex = m_foamWriteIndex;
    m_foamResetPending = false;
}

void SpectralOceanSimulation::AddPasses(RenderGraph& graph,
    SpectralOceanGraphHandles& handles, const bool rebuildInitialSpectrum,
    const SpectralOceanGraphCallbacks& callbacks)
{
    Core::Check(static_cast<bool>(callbacks.evolution)
            && static_cast<bool>(callbacks.horizontalFft)
            && static_cast<bool>(callbacks.verticalFft)
            && static_cast<bool>(callbacks.outputMaps)
            && static_cast<bool>(callbacks.foam)
            && static_cast<bool>(callbacks.mips),
        "Spectral ocean graph requires every per-frame stage callback.");
    const RenderGraph::PassOptions options{
        callbacks.asyncCompute ? RenderGraph::QueueClass::Compute
            : RenderGraph::QueueClass::Graphics,
        true, false, false};
    if (rebuildInitialSpectrum)
    {
        Core::Check(static_cast<bool>(callbacks.initialSpectrum),
            "A dirty spectral cache requires an initial-spectrum callback.");
        auto parameters = graph.CreatePassParameters();
        handles.initialSpectrum = parameters.WriteTexture(
            handles.initialSpectrum, RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass("SpectralOcean.InitialSpectrum",
            std::move(parameters), callbacks.initialSpectrum, options);
    }
    {
        auto parameters = graph.CreatePassParameters();
        parameters.ReadTexture(handles.initialSpectrum,
            RHI::ResourceState::ShaderResource);
        handles.spectrumA[0] = parameters.WriteTexture(
            handles.spectrumA[0], RHI::ResourceState::UnorderedAccess);
        handles.spectrumB[0] = parameters.WriteTexture(
            handles.spectrumB[0], RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass("SpectralOcean.Evolution",
            std::move(parameters), callbacks.evolution, options);
    }
    const auto addFftPass = [&](const char* name,
        const RenderGraph::ParameterExecuteCallback& execute)
    {
        auto parameters = graph.CreatePassParameters();
        for (TextureHandle& texture : handles.spectrumA)
        {
            texture = parameters.WriteTexture(
                texture, RHI::ResourceState::UnorderedAccess);
        }
        for (TextureHandle& texture : handles.spectrumB)
        {
            texture = parameters.WriteTexture(
                texture, RHI::ResourceState::UnorderedAccess);
        }
        graph.AddParameterPass(name, std::move(parameters), execute, options);
    };
    addFftPass("SpectralOcean.Fft.Horizontal", callbacks.horizontalFft);
    addFftPass("SpectralOcean.Fft.Vertical", callbacks.verticalFft);
    {
        auto parameters = graph.CreatePassParameters();
        parameters.ReadTexture(handles.spectrumA[0],
            RHI::ResourceState::ShaderResource);
        parameters.ReadTexture(handles.spectrumB[0],
            RHI::ResourceState::ShaderResource);
        handles.displacement = parameters.WriteTexture(
            handles.displacement, RHI::ResourceState::UnorderedAccess);
        handles.gradientFoam = parameters.WriteTexture(
            handles.gradientFoam, RHI::ResourceState::UnorderedAccess);
        handles.slopeMoments = parameters.WriteTexture(
            handles.slopeMoments, RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass("SpectralOcean.OutputMaps",
            std::move(parameters), callbacks.outputMaps, options);
    }
    {
        auto parameters = graph.CreatePassParameters();
        parameters.ReadTexture(handles.displacement,
            RHI::ResourceState::ShaderResource);
        parameters.ReadTexture(handles.gradientFoam,
            RHI::ResourceState::ShaderResource);
        parameters.ReadTexture(
            handles.foamHistory[handles.foamReadIndex],
            RHI::ResourceState::ShaderResource);
        handles.foamHistory[handles.foamWriteIndex] =
            parameters.WriteTexture(
                handles.foamHistory[handles.foamWriteIndex],
                RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass("SpectralOcean.Foam",
            std::move(parameters), callbacks.foam, options);
    }
    {
        auto parameters = graph.CreatePassParameters();
        handles.displacement = parameters.WriteTexture(
            handles.displacement, RHI::ResourceState::UnorderedAccess);
        handles.gradientFoam = parameters.WriteTexture(
            handles.gradientFoam, RHI::ResourceState::UnorderedAccess);
        handles.slopeMoments = parameters.WriteTexture(
            handles.slopeMoments, RHI::ResourceState::UnorderedAccess);
        handles.foamHistory[handles.foamWriteIndex] =
            parameters.WriteTexture(
                handles.foamHistory[handles.foamWriteIndex],
                RHI::ResourceState::UnorderedAccess);
        graph.AddParameterPass("SpectralOcean.Mips",
            std::move(parameters), callbacks.mips, options);
    }
}

void SpectralOceanSimulation::ExecuteInitialSpectrum(
    RHI::ICommandContext& commandContext, const std::uint32_t frameIndex)
{
    Core::Check(IsGpuReady() && frameIndex < m_gpu->frames.size(),
        "Spectral ocean initial spectrum uses an invalid frame.");
    RHI::ScopedDebugLabel label(commandContext,
        "SpectralOcean.InitialSpectrum");
    commandContext.BindComputePipeline(*m_gpu->initialPipeline);
    commandContext.BindDescriptorSet(*m_gpu->frames[frameIndex].initialSet);
    const std::uint32_t groups = (m_gpu->resolution + 7u) / 8u;
    commandContext.Dispatch(groups, groups, CascadeCount);
    m_recordedSpectrumVersion = m_pendingSpectrumVersion;
}

void SpectralOceanSimulation::ExecuteEvolution(
    RHI::ICommandContext& commandContext, const std::uint32_t frameIndex) const
{
    Core::Check(IsGpuReady() && frameIndex < m_gpu->frames.size(),
        "Spectral ocean evolution uses an invalid frame.");
    RHI::ScopedDebugLabel label(commandContext, "SpectralOcean.Evolution");
    commandContext.BindComputePipeline(*m_gpu->evolutionPipeline);
    commandContext.BindDescriptorSet(*m_gpu->frames[frameIndex].evolutionSet);
    const std::uint32_t groups = (m_gpu->resolution + 7u) / 8u;
    commandContext.Dispatch(groups, groups, CascadeCount);
}

void SpectralOceanSimulation::ExecuteHorizontalFft(
    RHI::ICommandContext& commandContext) const
{
    RHI::ScopedDebugLabel label(commandContext, "SpectralOcean.Fft.Horizontal");
    m_gpu->fft.ExecuteInverseHorizontalArray(commandContext);
}

void SpectralOceanSimulation::ExecuteVerticalFft(
    RHI::ICommandContext& commandContext) const
{
    RHI::ScopedDebugLabel label(commandContext, "SpectralOcean.Fft.Vertical");
    m_gpu->fft.ExecuteInverseVerticalArray(commandContext);
}

void SpectralOceanSimulation::ExecuteOutputMaps(
    RHI::ICommandContext& commandContext) const
{
    Core::Check(IsGpuReady(), "Spectral ocean map build is not ready.");
    RHI::ScopedDebugLabel label(commandContext, "SpectralOcean.OutputMaps");
    commandContext.BindComputePipeline(*m_gpu->buildPipeline);
    commandContext.BindDescriptorSet(*m_gpu->buildSet);
    const std::uint32_t groups = (m_gpu->resolution + 7u) / 8u;
    commandContext.Dispatch(groups, groups, CascadeCount);
}

void SpectralOceanSimulation::ExecuteFoam(
    RHI::ICommandContext& commandContext,
    const std::uint32_t frameIndex) const
{
    Core::Check(IsGpuReady() && frameIndex < m_gpu->frames.size(),
        "Spectral ocean foam update uses an invalid frame.");
    RHI::ScopedDebugLabel label(commandContext, "SpectralOcean.Foam");
    commandContext.BindComputePipeline(*m_gpu->foamPipeline);
    commandContext.BindDescriptorSet(
        *m_gpu->frames[frameIndex].foamSets[m_foamWriteIndex]);
    const std::uint32_t groups = (m_gpu->resolution + 7u) / 8u;
    commandContext.Dispatch(groups, groups, CascadeCount);
}

void SpectralOceanSimulation::ExecuteMips(
    RHI::ICommandContext& commandContext) const
{
    Core::Check(IsGpuReady(), "Spectral ocean mip build is not ready.");
    RHI::ScopedDebugLabel label(commandContext, "SpectralOcean.Mips");
    commandContext.BindComputePipeline(*m_gpu->mipPipeline);
    const auto& mipSets = m_gpu->mipSets[m_foamWriteIndex];
    for (std::uint32_t index = 0u; index < mipSets.size(); ++index)
    {
        const std::uint32_t resolution = std::max(
            m_gpu->resolution >> (index + 1u), 1u);
        commandContext.BindDescriptorSet(*mipSets[index]);
        commandContext.Dispatch((resolution + 7u) / 8u,
            (resolution + 7u) / 8u, CascadeCount);
        commandContext.GlobalBarrier({RHI::ResourceState::UnorderedAccess,
            RHI::ResourceState::UnorderedAccess});
    }
}

bool SpectralOceanSimulation::IsInitialized() const noexcept
{
    return m_gpu != nullptr && m_gpu->resolution != 0u
        && m_gpu->initialSpectrum.texture != nullptr
        && m_gpu->spectrumA[0].texture != nullptr
        && m_gpu->spectrumA[1].texture != nullptr
        && m_gpu->spectrumB[0].texture != nullptr
        && m_gpu->spectrumB[1].texture != nullptr
        && m_gpu->displacement.texture != nullptr
        && m_gpu->gradient.texture != nullptr
        && m_gpu->moments.texture != nullptr
        && m_gpu->foamHistory[0].texture != nullptr
        && m_gpu->foamHistory[1].texture != nullptr;
}

bool SpectralOceanSimulation::IsGpuReady() const noexcept
{
    return IsInitialized() && m_gpu->pipelinesReady
        && m_gpu->fft.IsArrayGpuReady() && !m_gpu->frames.empty();
}

std::uint32_t SpectralOceanSimulation::Resolution() const noexcept
{
    return m_gpu != nullptr ? m_gpu->resolution : 0u;
}

std::uint32_t SpectralOceanSimulation::MipLevelCount() const noexcept
{
    return m_gpu != nullptr ? m_gpu->mipLevelCount : 0u;
}

OceanSimulationQuality SpectralOceanSimulation::ActiveQuality() const noexcept
{
    return m_gpu != nullptr ? m_gpu->quality : OceanSimulationQuality::Normal;
}

std::uint64_t SpectralOceanSimulation::ResourceGeneration() const noexcept
{
    return m_resourceGeneration;
}

std::uint64_t SpectralOceanSimulation::HistoryVersion() const noexcept
{
    return m_historyVersion;
}

bool SpectralOceanSimulation::IsFoamHistoryResetPending() const noexcept
{
    return m_foamResetPending;
}

std::size_t SpectralOceanSimulation::RetiredResourceSetCount() const noexcept
{
    return m_retiredStates.size();
}

const OceanSpectrumWorkingResource& SpectralOceanSimulation::SpectrumA(
    const std::uint32_t pingIndex) const
{
    Core::Check(m_gpu != nullptr && pingIndex < WorkingPingCount,
        "Spectral ocean spectrum A ping index is out of range.");
    return m_gpu->spectrumA[pingIndex];
}

const OceanSpectrumWorkingResource& SpectralOceanSimulation::SpectrumB(
    const std::uint32_t pingIndex) const
{
    Core::Check(m_gpu != nullptr && pingIndex < WorkingPingCount,
        "Spectral ocean spectrum B ping index is out of range.");
    return m_gpu->spectrumB[pingIndex];
}

const OceanSpectrumWorkingResource& SpectralOceanSimulation::InitialSpectrum() const
{
    Core::Check(m_gpu != nullptr,
        "Spectral ocean initial spectrum is not initialized.");
    return m_gpu->initialSpectrum;
}

const OceanPublishedMapResource& SpectralOceanSimulation::DisplacementMap() const
{
    Core::Check(m_gpu != nullptr, "Spectral ocean maps are not initialized.");
    return m_gpu->displacement;
}

const OceanPublishedMapResource& SpectralOceanSimulation::GradientMap() const
{
    Core::Check(m_gpu != nullptr, "Spectral ocean maps are not initialized.");
    return m_gpu->gradient;
}

const OceanPublishedMapResource& SpectralOceanSimulation::SlopeMomentMap() const
{
    Core::Check(m_gpu != nullptr, "Spectral ocean maps are not initialized.");
    return m_gpu->moments;
}

const OceanPublishedMapResource& SpectralOceanSimulation::FoamMap() const
{
    Core::Check(m_gpu != nullptr, "Spectral ocean foam is not initialized.");
    return m_gpu->foamHistory[m_foamWriteIndex];
}

const OceanPublishedMapResource& SpectralOceanSimulation::FoamHistoryMap(
    const std::uint32_t index) const
{
    Core::Check(m_gpu != nullptr && index < 2u,
        "Spectral ocean foam history index is out of range.");
    return m_gpu->foamHistory[index];
}

OceanPublishedMapMetadata SpectralOceanSimulation::PublishedMetadata() const noexcept
{
    OceanPublishedMapMetadata metadata{};
    if (m_gpu == nullptr)
    {
        return metadata;
    }
    metadata.quality = m_gpu->quality;
    metadata.resolution = m_gpu->resolution;
    metadata.cascadeCount = CascadeCount;
    metadata.mipLevelCount = m_gpu->mipLevelCount;
    metadata.resourceGeneration = m_gpu->generation;
    metadata.displacementFormat = PublishedMapFormat;
    metadata.gradientFormat = PublishedMapFormat;
    metadata.momentFormat = PublishedMapFormat;
    metadata.foamFormat = PublishedMapFormat;
    return metadata;
}

void SpectralOceanSimulation::ResetFoamHistory() noexcept
{
    InvalidateFoamHistory();
}

void SpectralOceanSimulation::InvalidateFoamHistory() noexcept
{
    if (!m_foamResetPending)
    {
        ++m_historyVersion;
    }
    m_foamResetPending = true;
    m_hasPreviousFoamTime = false;
    m_previousFoamTimeSeconds = 0.0;
    m_foamDeltaSeconds = 0.0f;
}

void SpectralOceanSimulation::RefreshResourceStatistics(
    const bool initialSpectrumScheduled) noexcept
{
    if (m_gpu == nullptr)
    {
        m_statistics = {};
        return;
    }
    const std::uint64_t basePixels =
        static_cast<std::uint64_t>(m_gpu->resolution)
        * m_gpu->resolution * CascadeCount;
    const std::uint64_t mipPixels = MipPixelCount(
        m_gpu->resolution, m_gpu->mipLevelCount) * CascadeCount;
    constexpr std::uint64_t WorkingBytesPerPixel = 16u;
    constexpr std::uint64_t PublishedBytesPerPixel = 8u;
    constexpr std::uint64_t WorkingTextureCount = 5u;
    constexpr std::uint64_t PublishedTextureCount = 5u;
    const std::uint64_t allocatedBytes =
        basePixels * WorkingBytesPerPixel * WorkingTextureCount
        + mipPixels * PublishedBytesPerPixel * PublishedTextureCount;
    m_statistics.allocatedMegabytes = static_cast<float>(allocatedBytes)
        / (1024.0f * 1024.0f);
    m_statistics.cascadeResolution = m_gpu->resolution;
    m_statistics.publishedVersion = m_gpu->generation;
    const std::uint32_t fftStages = std::bit_width(m_gpu->resolution) - 1u;
    m_statistics.dispatchCount = 1u + 2u * fftStages + 1u + 1u
        + (m_gpu->mipLevelCount - 1u)
        + (initialSpectrumScheduled ? 1u : 0u);
}

const OceanStatistics& SpectralOceanSimulation::GetStatistics() const noexcept
{
    return m_statistics;
}

void SpectralOceanSimulation::UpdateGpuTimings(
    const float spectrumMilliseconds,
    const float horizontalFftMilliseconds,
    const float verticalFftMilliseconds,
    const float mapMilliseconds,
    const float foamMilliseconds,
    const float mipMilliseconds,
    const bool available) noexcept
{
    constexpr float FilterAlpha = 0.25f;
    const auto filter = [available](const float previous, const float current)
    {
        return available ? previous + FilterAlpha * (current - previous) : 0.0f;
    };
    m_statistics.spectrumMilliseconds = filter(
        m_statistics.spectrumMilliseconds, spectrumMilliseconds);
    m_statistics.fftMilliseconds = filter(
        m_statistics.fftMilliseconds,
        horizontalFftMilliseconds + verticalFftMilliseconds);
    m_statistics.mapMilliseconds = filter(
        m_statistics.mapMilliseconds, mapMilliseconds);
    m_statistics.foamMilliseconds = filter(
        m_statistics.foamMilliseconds, foamMilliseconds);
    m_statistics.mipMilliseconds = filter(
        m_statistics.mipMilliseconds, mipMilliseconds);
    m_statistics.gpuTotalMilliseconds = filter(
        m_statistics.gpuTotalMilliseconds,
        spectrumMilliseconds + horizontalFftMilliseconds
            + verticalFftMilliseconds + mapMilliseconds
            + foamMilliseconds + mipMilliseconds);
    m_statistics.gpuTimersAvailable = available;
}
} // namespace Prism::Renderer
