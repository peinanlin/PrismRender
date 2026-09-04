#include "Asset/ShaderManager.h"
#include "Platform/Window.h"
#include "RHI/ICommandContext.h"
#include "RHI/IFrameContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/IRenderBackend.h"
#include "RHI/RenderBackendFactory.h"
#include "RHI/ShaderLayoutBuilder.h"
#include "Renderer/Features/Ocean/OceanFft.h"
#include "Renderer/Features/Ocean/OceanGpuQuery.h"
#include "Renderer/Features/Ocean/OceanSpectrumMath.h"
#include "Renderer/Features/Ocean/SpectralOceanSimulation.h"
#include "Renderer/Features/Ocean/OceanFallbackResources.h"
#include "Renderer/Pipeline/PipelineCache.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
struct Float2
{
    float x = 0.0f;
    float y = 0.0f;
};

struct Float4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct Uint4
{
    std::uint32_t x = 0u;
    std::uint32_t y = 0u;
    std::uint32_t z = 0u;
    std::uint32_t w = 0u;
};

struct alignas(16) InitialSpectrumConstants
{
    std::uint32_t resolution = 0u;
    std::uint32_t randomSeed = 0u;
    std::uint32_t sampleCount = 0u;
    std::uint32_t padding = 0u;
    Float4 baseDirectionSpeedFetch;
    Float4 basePeakingAmplitudeCutoff;
    Float4 baseDependencyPadding;
    Float4 swellDirectionSpeedFetch;
    Float4 swellPeakingAmplitudeCutoff;
    Float4 swellDependencyPadding;
    Float4 cascadePatchLengths;
    Float4 cascadeLowerWavelengths;
    Float4 cascadeUpperWavelengths;
};

struct alignas(16) GpuConstants
{
    std::uint32_t resolution = 0;
    std::uint32_t stage = 0;
    std::uint32_t direction = 0;
    std::uint32_t inverse = 0;
    std::uint32_t bitReverseInput = 0;
    std::uint32_t normalizeOutput = 0;
    std::uint32_t padding0 = 0;
    std::uint32_t padding1 = 0;
};

struct Fixture
{
    std::uint32_t size = 0;
    std::string label;
    std::vector<Float2> expected;
    bool requireRealOutput = false;
    std::uint32_t outputIndex = 0u;
    std::array<std::shared_ptr<Prism::RHI::IBuffer>, 2> ping;
    std::shared_ptr<Prism::RHI::IBuffer> readback;
    std::vector<std::shared_ptr<Prism::RHI::IBuffer>> constants;
    std::vector<std::shared_ptr<Prism::RHI::IDescriptorSet>> sets;
    std::vector<Prism::Renderer::OceanFftPass> passes;
};

enum class FftFixtureKind
{
    ImpulseRoundTrip,
    ConstantRoundTrip,
    SinusoidRoundTrip,
    HermitianInverse,
    NormalizedInverse
};

struct SpectrumResourceFixture
{
    Prism::Renderer::SpectralOceanSimulation simulation;
    std::shared_ptr<Prism::RHI::IComputePipeline> writePipeline;
    std::shared_ptr<Prism::RHI::IComputePipeline> readPipeline;
    std::shared_ptr<Prism::RHI::IDescriptorSet> writeSet;
    std::shared_ptr<Prism::RHI::IDescriptorSet> readSet;
    std::shared_ptr<Prism::RHI::IBuffer> output;
    std::shared_ptr<Prism::RHI::IBuffer> readback;
};

void PrepareFixturePresent(Prism::RHI::IRenderBackend& backend)
{
    using namespace Prism::RHI;
    auto& frame = backend.GetFrameContext();
    RenderingInfo rendering{};
    rendering.width = frame.GetFrameWidth();
    rendering.height = frame.GetFrameHeight();
    RenderingAttachment color{};
    color.view = &frame.GetCurrentBackBufferView();
    color.loadOperation = LoadOperation::Clear;
    color.storeOperation = StoreOperation::Store;
    // Vulkan acquires a Present image; D3D12 BeginFrame transitions it already.
    color.stateBefore = backend.GetGraphicsDevice().GetGraphicsApi() == GraphicsApi::Vulkan
        ? ResourceState::Present : ResourceState::RenderTarget;
    color.stateAfter = ResourceState::RenderTarget;
    rendering.colorAttachments.push_back(color);
    backend.GetCommandContext().BeginRendering(rendering);
    backend.GetCommandContext().EndRendering();
}

void ValidateOceanFallbackResources(Prism::RHI::IRenderBackend& backend,
    Prism::Asset::ShaderManager& shaderManager)
{
    using namespace Prism::RHI;
    auto& device = backend.GetGraphicsDevice();
    Prism::Renderer::OceanFallbackResources fallback;
    fallback.Initialize(device);
    const auto zero = fallback.ZeroArray();
    const auto normal = fallback.UpNormalArray();
    fallback.Initialize(device);
    if (zero != fallback.ZeroArray() || normal != fallback.UpNormalArray())
        throw std::runtime_error("Ocean fallback initialization did not reuse its resources.");
    for (const auto& view : {zero, normal})
    {
        const auto& texture = view->GetTexture()->GetDescription();
        if (texture.width != 1u || texture.height != 1u || texture.arrayLayers != 4u
            || texture.mipLevels != 1u || view->GetDescription().arrayLayerCount != 4u
            || view->GetDescription().mipLevelCount != 1u)
            throw std::runtime_error("Ocean fallback exposes an uninitialized mip/layer range.");
    }
    const auto& shader = shaderManager.LoadShader(
        std::filesystem::path(PRISM_RENDER_SHADER_DIR) / "Ocean/OceanSpectrumResourceValidation.slang",
        "ReadSpectrumSlicesCS", ShaderStage::Compute, device.GetPreferredShaderBinaryFormat());
    const std::array stages{ShaderLayoutStage{&shader.reflection, ShaderStage::Compute}};
    auto layout = device.CreateDescriptorSetLayout(BuildDescriptorSetLayout(stages, {}));
    ComputePipelineDescription pipelineDescription{};
    pipelineDescription.computeShader = shader;
    pipelineDescription.descriptorSetLayout = layout;
    auto pipeline = device.CreateComputePipeline(pipelineDescription);
    std::array<std::shared_ptr<IBuffer>, 2> output;
    std::array<std::shared_ptr<IBuffer>, 2> readback;
    std::array<std::shared_ptr<IDescriptorSet>, 2> sets;
    for (std::size_t index = 0; index < sets.size(); ++index)
    {
        BufferDescription description{};
        description.size = sizeof(Float4) * 4u;
        description.stride = sizeof(Float4);
        description.usage = BufferUsage::Storage | BufferUsage::CopySource | BufferUsage::CopyDestination;
        description.memoryAccess = MemoryAccess::GpuOnly;
        output[index] = device.CreateBuffer(description);
        description.usage = BufferUsage::CopyDestination;
        description.memoryAccess = MemoryAccess::GpuToCpu;
        readback[index] = device.CreateBuffer(description);
        sets[index] = device.CreateDescriptorSet(layout);
        sets[index]->WriteTextureView(16u, index == 0u ? zero : normal);
        sets[index]->WriteBuffer(36u, output[index]);
    }
    auto& frame = backend.GetFrameContext();
    if (frame.BeginFrame() != FrameResult::Ready)
        throw std::runtime_error("Ocean fallback fixture failed to begin frame.");
    auto& command = backend.GetCommandContext();
    command.BindComputePipeline(*pipeline);
    for (std::size_t index = 0; index < sets.size(); ++index)
    {
        // No texture transition or clear here: the actual upload path must
        // initialize every layer and leave the whole sampled view readable.
        command.BindDescriptorSet(*sets[index]);
        command.Dispatch(1u, 1u, 4u);
        command.BufferBarrier({output[index].get(), ResourceState::UnorderedAccess, ResourceState::CopySource});
        command.CopyBuffer(*output[index], *readback[index], sizeof(Float4) * 4u);
    }
    PrepareFixturePresent(backend);
    if (frame.EndFrame() != FrameResult::Ready)
        throw std::runtime_error("Ocean fallback fixture failed to end frame.");
    frame.WaitForGpu();
    for (std::size_t index = 0; index < sets.size(); ++index)
    {
        std::array<Float4, 4> values{};
        readback[index]->Read(values.data(), sizeof(values));
        for (const auto& value : values)
            if (value.x != 0.0f || value.y != (index == 0u ? 0.0f : 1.0f)
                || value.z != 0.0f || value.w != 0.0f)
                throw std::runtime_error("Ocean fallback layer contains non-neutral or uninitialized values.");
    }
    std::cout << "Ocean fallback: all exposed mip/layer subresources initialized and sampled correctly.\n";
}

struct InitialSpectrumFixture
{
    static constexpr std::uint32_t Seed = 0x4f1bbcdcu;

    Prism::Renderer::SpectralOceanSimulation simulation;
    Prism::Renderer::OceanSettings settings;
    std::array<Uint4, 8> coordinates{};
    std::shared_ptr<Prism::RHI::IComputePipeline> generatePipeline;
    std::shared_ptr<Prism::RHI::IComputePipeline> samplePipeline;
    std::shared_ptr<Prism::RHI::IDescriptorSet> generateSet;
    std::shared_ptr<Prism::RHI::IDescriptorSet> sampleSet;
    std::shared_ptr<Prism::RHI::IBuffer> constants;
    std::shared_ptr<Prism::RHI::IBuffer> coordinateBuffer;
    std::shared_ptr<Prism::RHI::IBuffer> output;
    std::shared_ptr<Prism::RHI::IBuffer> readback;
};

struct SimulationValidationSample
{
    Float4 spectrum;
    Float4 displacement;
    Float4 gradient;
    Float4 moments;
    Float4 foam;
};

struct FullSimulationFixture
{
    static constexpr std::size_t SampleCount = 40u;

    Prism::Renderer::SpectralOceanSimulation simulation;
    Prism::Renderer::OceanSettings settings;
    std::array<Uint4, SampleCount> coordinates{};
    std::shared_ptr<Prism::RHI::IComputePipeline> samplePipeline;
    std::shared_ptr<Prism::RHI::IDescriptorSet> sampleSet;
    std::shared_ptr<Prism::RHI::IBuffer> constants;
    std::shared_ptr<Prism::RHI::IBuffer> coordinateBuffer;
    std::shared_ptr<Prism::RHI::IBuffer> output;
    std::shared_ptr<Prism::RHI::IBuffer> readback;
    Prism::Renderer::OceanGpuQuery gpuQuery;
    std::shared_ptr<Prism::RHI::IBuffer> queryConstants;
    std::shared_ptr<Prism::RHI::IBuffer> spectralOnlyConstants;
    std::shared_ptr<Prism::RHI::IBuffer> queryPoints;
    std::shared_ptr<Prism::RHI::IBuffer> queryResults;
    std::shared_ptr<Prism::RHI::IBuffer> queryReadback;
    std::shared_ptr<Prism::RHI::IBuffer> spectralOnlyResults;
    std::shared_ptr<Prism::RHI::IBuffer> spectralOnlyReadback;
    std::shared_ptr<Prism::RHI::ITexture> localTexture;
    std::shared_ptr<Prism::RHI::ITextureView> localTextureView;
    std::shared_ptr<Prism::RHI::IDescriptorSet> querySet;
    std::shared_ptr<Prism::RHI::IDescriptorSet> spectralOnlyQuerySet;
};

[[nodiscard]] Prism::RHI::GraphicsApi ParseApi(const int argc, char** argv)
{
    if (argc != 2 && !(argc == 3 && std::string(argv[2]) == "--fallback-only"))
    {
        throw std::runtime_error(
            "Usage: PrismOceanFftGpuTests <d3d12|vulkan> [--fallback-only]");
    }
    const std::string api(argv[1]);
    if (api == "d3d12")
    {
        return Prism::RHI::GraphicsApi::Direct3D12;
    }
    if (api == "vulkan")
    {
        return Prism::RHI::GraphicsApi::Vulkan;
    }
    throw std::runtime_error("Unknown graphics API: " + api);
}

[[nodiscard]] Fixture BuildFixture(Prism::RHI::IGraphicsDevice& device,
    const std::shared_ptr<Prism::RHI::IDescriptorSetLayout>& layout,
    const std::uint32_t size, const FftFixtureKind kind)
{
    Prism::Renderer::OceanFft fft;
    if (!fft.Configure(size))
    {
        throw std::runtime_error("Unsupported FFT size in GPU fixture.");
    }
    Fixture fixture{};
    fixture.size = size;
    std::vector<Float2> input(size);
    bool inverseOnly = false;
    switch (kind)
    {
    case FftFixtureKind::ImpulseRoundTrip:
        fixture.label = "impulse-round-trip";
        input[3u].x = 1.0f;
        break;
    case FftFixtureKind::ConstantRoundTrip:
        fixture.label = "constant-round-trip";
        std::ranges::fill(input, Float2{0.75f, -0.25f});
        break;
    case FftFixtureKind::SinusoidRoundTrip:
        fixture.label = "sinusoid-round-trip";
        for (std::uint32_t index = 0u; index < size; ++index)
        {
            const float phase = 2.0f * 3.14159265358979323846f
                * 5.0f * static_cast<float>(index)
                / static_cast<float>(size);
            input[index] = {std::cos(phase), std::sin(phase)};
        }
        break;
    case FftFixtureKind::HermitianInverse:
        fixture.label = "hermitian-inverse";
        input[0u] = {0.5f, 0.0f};
        input[3u] = {0.25f, 0.4f};
        input[size - 3u] = {0.25f, -0.4f};
        inverseOnly = true;
        fixture.requireRealOutput = true;
        break;
    case FftFixtureKind::NormalizedInverse:
        fixture.label = "normalized-inverse";
        input[0u] = {static_cast<float>(size), 0.0f};
        inverseOnly = true;
        break;
    }
    fixture.expected = input;
    if (inverseOnly)
    {
        std::vector<std::complex<float>> cpuValues(size);
        for (std::uint32_t index = 0u; index < size; ++index)
        {
            cpuValues[index] = {input[index].x, input[index].y};
        }
        if (!fft.Transform(cpuValues, true))
        {
            throw std::runtime_error("CPU FFT fixture reference failed.");
        }
        for (std::uint32_t index = 0u; index < size; ++index)
        {
            fixture.expected[index] = {
                cpuValues[index].real(), cpuValues[index].imag()};
        }
    }

    Prism::RHI::BufferDescription workingDescription{};
    workingDescription.size = input.size() * sizeof(Float2);
    workingDescription.stride = sizeof(Float2);
    workingDescription.usage = Prism::RHI::BufferUsage::Storage
        | Prism::RHI::BufferUsage::CopySource
        | Prism::RHI::BufferUsage::CopyDestination;
    workingDescription.memoryAccess = Prism::RHI::MemoryAccess::GpuOnly;
    fixture.ping[0] = device.CreateBuffer(
        workingDescription, input.data());
    fixture.ping[1] = device.CreateBuffer(workingDescription);
    fixture.ping[0]->SetDebugName("OceanFftGpuTest.Ping0");
    fixture.ping[1]->SetDebugName("OceanFftGpuTest.Ping1");

    Prism::RHI::BufferDescription readbackDescription{};
    readbackDescription.size = workingDescription.size;
    readbackDescription.stride = sizeof(Float2);
    readbackDescription.usage = Prism::RHI::BufferUsage::CopyDestination;
    readbackDescription.memoryAccess = Prism::RHI::MemoryAccess::GpuToCpu;
    fixture.readback = device.CreateBuffer(readbackDescription);
    fixture.readback->SetDebugName("OceanFftGpuTest.Readback");

    if (inverseOnly)
    {
        fixture.passes = fft.BuildPasses(
            1u, true, 0u, true, true);
    }
    else
    {
        fixture.passes = fft.BuildPasses(
            1u, false, 0u, true, false);
        const std::uint32_t inverseSource =
            fixture.passes.back().destinationIndex;
        std::vector<Prism::Renderer::OceanFftPass> inversePasses =
            fft.BuildPasses(1u, true, inverseSource, true, true);
        fixture.passes.insert(fixture.passes.end(),
            inversePasses.begin(), inversePasses.end());
    }
    fixture.constants.reserve(fixture.passes.size());
    fixture.sets.reserve(fixture.passes.size());

    for (const Prism::Renderer::OceanFftPass& pass : fixture.passes)
    {
        const GpuConstants constants{
            size,
            pass.stage,
            pass.direction,
            pass.inverse ? 1u : 0u,
            pass.bitReverseInput ? 1u : 0u,
            pass.normalizeOutput ? 1u : 0u,
            0u,
            0u};
        Prism::RHI::BufferDescription constantDescription{};
        constantDescription.size = sizeof(GpuConstants);
        constantDescription.stride = sizeof(GpuConstants);
        constantDescription.usage = Prism::RHI::BufferUsage::Constant;
        constantDescription.memoryAccess = Prism::RHI::MemoryAccess::CpuToGpu;
        std::shared_ptr<Prism::RHI::IBuffer> constantBuffer =
            device.CreateBuffer(constantDescription, &constants);
        std::shared_ptr<Prism::RHI::IDescriptorSet> set =
            device.CreateDescriptorSet(layout);
        set->WriteBuffer(0, constantBuffer);
        set->WriteBuffer(36, fixture.ping[pass.sourceIndex]);
        set->WriteBuffer(37, fixture.ping[pass.destinationIndex]);
        fixture.constants.push_back(std::move(constantBuffer));
        fixture.sets.push_back(std::move(set));
    }
    fixture.outputIndex = fixture.passes.back().destinationIndex;
    return fixture;
}

[[nodiscard]] bool HasBinding(
    const Prism::RHI::DescriptorSetLayoutDescription& layout,
    const std::uint32_t binding,
    const Prism::RHI::DescriptorType type)
{
    return std::ranges::any_of(layout.bindings,
        [binding, type](const Prism::RHI::DescriptorBindingDescription& item)
        {
            return item.binding == binding && item.type == type;
        });
}

[[nodiscard]] SpectrumResourceFixture BuildSpectrumResourceFixture(
    Prism::RHI::IGraphicsDevice& device,
    Prism::Asset::ShaderManager& shaderManager,
    Prism::Renderer::PipelineCache& pipelineCache)
{
    SpectrumResourceFixture fixture{};
    fixture.simulation.InitializeWorkingResources(
        device, Prism::Renderer::OceanSimulationQuality::Normal);
    if (!fixture.simulation.IsInitialized()
        || fixture.simulation.Resolution() != 128u)
    {
        throw std::runtime_error(
            "Normal spectral-ocean resources did not initialize at 128.");
    }
    const Prism::Renderer::OceanSpectrumPrecisionPolicy precision =
        fixture.simulation.PrecisionPolicy();
    if (precision.workingFormat != Prism::RHI::Format::Rgba32Float
        || precision.bitsPerChannel != 32u
        || precision.allowHalfPrecisionOptimization)
    {
        throw std::runtime_error(
            "Spectrum working precision policy is not the FP32 correctness path.");
    }

    for (std::uint32_t ping = 0u;
         ping < Prism::Renderer::SpectralOceanSimulation::WorkingPingCount;
         ++ping)
    {
        for (const Prism::Renderer::OceanSpectrumWorkingResource* resource :
             {&fixture.simulation.SpectrumA(ping),
              &fixture.simulation.SpectrumB(ping)})
        {
            const Prism::RHI::TextureDescription& texture =
                resource->texture->GetDescription();
            if (texture.width != 128u || texture.height != 128u
                || texture.arrayLayers
                    != Prism::Renderer::SpectralOceanSimulation::CascadeCount
                || texture.format != Prism::RHI::Format::Rgba32Float)
            {
                throw std::runtime_error(
                    "Spectrum array description is incompatible with Normal quality.");
            }
            for (std::uint32_t cascade = 0u;
                 cascade
                    < Prism::Renderer::SpectralOceanSimulation::CascadeCount;
                 ++cascade)
            {
                const auto validateSlice = [cascade](
                    const std::shared_ptr<Prism::RHI::ITextureView>& view,
                    const Prism::RHI::TextureViewType type)
                {
                    const Prism::RHI::TextureViewDescription& description =
                        view->GetDescription();
                    return description.type == type
                        && description.baseArrayLayer == cascade
                        && description.arrayLayerCount == 1u;
                };
                if (!validateSlice(resource->sampledSlices[cascade],
                        Prism::RHI::TextureViewType::Sampled)
                    || !validateSlice(resource->storageSlices[cascade],
                        Prism::RHI::TextureViewType::Storage))
                {
                    throw std::runtime_error(
                        "Spectrum per-cascade view selects the wrong subresource.");
                }
            }
        }
    }

    const std::filesystem::path shaderPath =
        std::filesystem::path(PRISM_RENDER_SHADER_DIR)
        / "Ocean/OceanSpectrumResourceValidation.slang";
    const Prism::RHI::ShaderBinaryFormat format =
        device.GetPreferredShaderBinaryFormat();
    const Prism::RHI::ShaderBinary& writeShader = shaderManager.LoadShader(
        shaderPath, "WriteSpectrumSlicesCS",
        Prism::RHI::ShaderStage::Compute, format);
    const Prism::RHI::ShaderBinary& readShader = shaderManager.LoadShader(
        shaderPath, "ReadSpectrumSlicesCS",
        Prism::RHI::ShaderStage::Compute, format);

    const std::array writeStages{Prism::RHI::ShaderLayoutStage{
        &writeShader.reflection, Prism::RHI::ShaderStage::Compute}};
    const std::array readStages{Prism::RHI::ShaderLayoutStage{
        &readShader.reflection, Prism::RHI::ShaderStage::Compute}};
    const std::shared_ptr<Prism::RHI::IDescriptorSetLayout> writeLayout =
        device.CreateDescriptorSetLayout(
            Prism::RHI::BuildDescriptorSetLayout(writeStages, {}));
    const std::shared_ptr<Prism::RHI::IDescriptorSetLayout> readLayout =
        device.CreateDescriptorSetLayout(
            Prism::RHI::BuildDescriptorSetLayout(readStages, {}));
    if (!HasBinding(writeLayout->GetDescription(), 32u,
            Prism::RHI::DescriptorType::StorageTexture)
        || !HasBinding(readLayout->GetDescription(), 16u,
            Prism::RHI::DescriptorType::SampledTexture)
        || !HasBinding(readLayout->GetDescription(), 36u,
            Prism::RHI::DescriptorType::StorageBuffer))
    {
        throw std::runtime_error(
            "Spectrum validation shader reflection lost an expected binding.");
    }

    Prism::RHI::ComputePipelineDescription writeDescription{};
    writeDescription.computeShader = writeShader;
    writeDescription.descriptorSetLayout = writeLayout;
    fixture.writePipeline = pipelineCache.GetOrCreateCompute(
        device, "Test.OceanSpectrumResources.Write", writeDescription);
    Prism::RHI::ComputePipelineDescription readDescription{};
    readDescription.computeShader = readShader;
    readDescription.descriptorSetLayout = readLayout;
    fixture.readPipeline = pipelineCache.GetOrCreateCompute(
        device, "Test.OceanSpectrumResources.Read", readDescription);

    Prism::RHI::BufferDescription outputDescription{};
    outputDescription.size = sizeof(Float4)
        * Prism::Renderer::SpectralOceanSimulation::CascadeCount;
    outputDescription.stride = sizeof(Float4);
    outputDescription.usage = Prism::RHI::BufferUsage::Storage
        | Prism::RHI::BufferUsage::CopySource
        | Prism::RHI::BufferUsage::CopyDestination;
    outputDescription.memoryAccess = Prism::RHI::MemoryAccess::GpuOnly;
    fixture.output = device.CreateBuffer(outputDescription);
    Prism::RHI::BufferDescription readbackDescription{};
    readbackDescription.size = outputDescription.size;
    readbackDescription.stride = sizeof(Float4);
    readbackDescription.usage = Prism::RHI::BufferUsage::CopyDestination;
    readbackDescription.memoryAccess = Prism::RHI::MemoryAccess::GpuToCpu;
    fixture.readback = device.CreateBuffer(readbackDescription);

    fixture.writeSet = device.CreateDescriptorSet(writeLayout);
    fixture.writeSet->WriteTextureView(
        32u, fixture.simulation.SpectrumA(0u).storageArray);
    fixture.readSet = device.CreateDescriptorSet(readLayout);
    fixture.readSet->WriteTextureView(
        16u, fixture.simulation.SpectrumA(0u).sampledArray);
    fixture.readSet->WriteBuffer(36u, fixture.output);
    return fixture;
}

[[nodiscard]] InitialSpectrumFixture BuildInitialSpectrumFixture(
    Prism::RHI::IGraphicsDevice& device,
    Prism::Asset::ShaderManager& shaderManager,
    Prism::Renderer::PipelineCache& pipelineCache,
    const Prism::Renderer::OceanSimulationQuality quality)
{
    InitialSpectrumFixture fixture{};
    fixture.settings = Prism::Renderer::OceanSettings::WaveWorksReference();
    fixture.settings.quality = quality;
    fixture.simulation.InitializeWorkingResources(
        device, fixture.settings.quality);
    if (!fixture.simulation.ConfigureSpectrum(
            fixture.settings, InitialSpectrumFixture::Seed))
    {
        throw std::runtime_error(
            "Initial-spectrum fixture did not request its first cache build.");
    }
    const std::uint32_t center = fixture.simulation.Resolution() / 2u;
    fixture.coordinates = {{
        {center + 1u, center, 0u, 0u},
        {center + 2u, center + 1u, 0u, 0u},
        {center + 2u, center + 3u, 1u, 0u},
        {center - 1u, center + 2u, 1u, 0u},
        {center + 5u, center + 3u, 2u, 0u},
        {center - 5u, center - 3u, 2u, 0u},
        {center + 7u, center + 9u, 3u, 0u},
        {center - 7u, center + 4u, 3u, 0u}}};

    const auto& cascades = fixture.simulation.SpectrumGenerator().GetCascades();
    const InitialSpectrumConstants constants{
        fixture.simulation.Resolution(),
        InitialSpectrumFixture::Seed,
        static_cast<std::uint32_t>(fixture.coordinates.size()),
        0u,
        {fixture.settings.baseWind.direction.x,
            fixture.settings.baseWind.direction.y,
            Prism::Renderer::BeaufortToMetersPerSecond(
                fixture.settings.baseWind.speed),
            fixture.settings.baseWind.fetchKilometers * 1000.0f},
        {fixture.settings.baseWind.spectrumPeaking,
            fixture.settings.baseWind.amplitudeMultiplier,
            fixture.settings.baseWind.smallWavesCutoffLength,
            fixture.settings.baseWind.smallWavesCutoffPower},
        {fixture.settings.baseWind.dependency, 0.0f, 0.0f, 0.0f},
        {fixture.settings.swell.direction.x,
            fixture.settings.swell.direction.y,
            fixture.settings.swell.speed,
            fixture.settings.swell.fetchKilometers * 1000.0f},
        {fixture.settings.swell.spectrumPeaking,
            fixture.settings.swell.amplitudeMultiplier,
            fixture.settings.swell.smallWavesCutoffLength,
            fixture.settings.swell.smallWavesCutoffPower},
        {fixture.settings.swell.dependency, 0.0f, 0.0f, 0.0f},
        {cascades[0].patchLengthMeters, cascades[1].patchLengthMeters,
            cascades[2].patchLengthMeters, cascades[3].patchLengthMeters},
        {cascades[0].wavelengthMinMeters, cascades[1].wavelengthMinMeters,
            cascades[2].wavelengthMinMeters, cascades[3].wavelengthMinMeters},
        {cascades[0].wavelengthMaxMeters, cascades[1].wavelengthMaxMeters,
            cascades[2].wavelengthMaxMeters, cascades[3].wavelengthMaxMeters}};

    Prism::RHI::BufferDescription constantDescription{};
    constantDescription.size = sizeof(constants);
    constantDescription.stride = sizeof(constants);
    constantDescription.usage = Prism::RHI::BufferUsage::Constant;
    constantDescription.memoryAccess = Prism::RHI::MemoryAccess::CpuToGpu;
    fixture.constants = device.CreateBuffer(constantDescription, &constants);

    Prism::RHI::BufferDescription coordinateDescription{};
    coordinateDescription.size = sizeof(fixture.coordinates);
    coordinateDescription.stride = sizeof(Uint4);
    coordinateDescription.usage = Prism::RHI::BufferUsage::Storage
        | Prism::RHI::BufferUsage::CopyDestination;
    coordinateDescription.memoryAccess = Prism::RHI::MemoryAccess::GpuOnly;
    fixture.coordinateBuffer = device.CreateBuffer(
        coordinateDescription, fixture.coordinates.data());
    Prism::RHI::BufferDescription outputDescription{};
    outputDescription.size = sizeof(Float4) * fixture.coordinates.size();
    outputDescription.stride = sizeof(Float4);
    outputDescription.usage = Prism::RHI::BufferUsage::Storage
        | Prism::RHI::BufferUsage::CopySource
        | Prism::RHI::BufferUsage::CopyDestination;
    outputDescription.memoryAccess = Prism::RHI::MemoryAccess::GpuOnly;
    fixture.output = device.CreateBuffer(outputDescription);
    Prism::RHI::BufferDescription readbackDescription{};
    readbackDescription.size = outputDescription.size;
    readbackDescription.stride = sizeof(Float4);
    readbackDescription.usage = Prism::RHI::BufferUsage::CopyDestination;
    readbackDescription.memoryAccess = Prism::RHI::MemoryAccess::GpuToCpu;
    fixture.readback = device.CreateBuffer(readbackDescription);

    const std::filesystem::path shaderPath =
        std::filesystem::path(PRISM_RENDER_SHADER_DIR)
        / "Ocean/OceanInitialSpectrum.slang";
    const Prism::RHI::ShaderBinaryFormat format =
        device.GetPreferredShaderBinaryFormat();
    const Prism::RHI::ShaderBinary& generateShader = shaderManager.LoadShader(
        shaderPath, "GenerateInitialSpectrumCS",
        Prism::RHI::ShaderStage::Compute, format);
    const Prism::RHI::ShaderBinary& sampleShader = shaderManager.LoadShader(
        shaderPath, "SampleInitialSpectrumCS",
        Prism::RHI::ShaderStage::Compute, format);
    const std::array generateStages{Prism::RHI::ShaderLayoutStage{
        &generateShader.reflection, Prism::RHI::ShaderStage::Compute}};
    const std::array sampleStages{Prism::RHI::ShaderLayoutStage{
        &sampleShader.reflection, Prism::RHI::ShaderStage::Compute}};
    const auto generateLayout = device.CreateDescriptorSetLayout(
        Prism::RHI::BuildDescriptorSetLayout(generateStages, {}));
    const auto sampleLayout = device.CreateDescriptorSetLayout(
        Prism::RHI::BuildDescriptorSetLayout(sampleStages, {}));
    if (!HasBinding(generateLayout->GetDescription(), 0u,
            Prism::RHI::DescriptorType::ConstantBuffer)
        || !HasBinding(generateLayout->GetDescription(), 32u,
            Prism::RHI::DescriptorType::StorageTexture)
        || !HasBinding(sampleLayout->GetDescription(), 16u,
            Prism::RHI::DescriptorType::SampledTexture)
        || !HasBinding(sampleLayout->GetDescription(), 17u,
            Prism::RHI::DescriptorType::ReadOnlyStorageBuffer)
        || !HasBinding(sampleLayout->GetDescription(), 36u,
            Prism::RHI::DescriptorType::StorageBuffer))
    {
        throw std::runtime_error(
            "Initial-spectrum Slang reflection lost an expected binding.");
    }
    Prism::RHI::ComputePipelineDescription generateDescription{};
    generateDescription.computeShader = generateShader;
    generateDescription.descriptorSetLayout = generateLayout;
    fixture.generatePipeline = pipelineCache.GetOrCreateCompute(
        device, "Test.OceanInitialSpectrum.Generate", generateDescription);
    Prism::RHI::ComputePipelineDescription sampleDescription{};
    sampleDescription.computeShader = sampleShader;
    sampleDescription.descriptorSetLayout = sampleLayout;
    fixture.samplePipeline = pipelineCache.GetOrCreateCompute(
        device, "Test.OceanInitialSpectrum.Sample", sampleDescription);
    fixture.generateSet = device.CreateDescriptorSet(generateLayout);
    fixture.generateSet->WriteBuffer(0u, fixture.constants);
    fixture.generateSet->WriteTextureView(32u,
        fixture.simulation.InitialSpectrum().storageArray);
    fixture.sampleSet = device.CreateDescriptorSet(sampleLayout);
    fixture.sampleSet->WriteBuffer(0u, fixture.constants);
    fixture.sampleSet->WriteTextureView(16u,
        fixture.simulation.InitialSpectrum().sampledArray);
    fixture.sampleSet->WriteBuffer(17u, fixture.coordinateBuffer);
    fixture.sampleSet->WriteBuffer(36u, fixture.output);
    return fixture;
}

[[nodiscard]] FullSimulationFixture BuildFullSimulationFixture(
    Prism::RHI::IGraphicsDevice& device,
    Prism::Asset::ShaderManager& shaderManager,
    Prism::Renderer::PipelineCache& pipelineCache,
    const std::uint32_t activeCascadeMask,
    const Prism::Renderer::OceanSimulationQuality quality =
        Prism::Renderer::OceanSimulationQuality::Normal)
{
    FullSimulationFixture fixture{};
    fixture.settings = Prism::Renderer::OceanSettings::WaveWorksReference();
    fixture.settings.quality = quality;
    fixture.simulation.InitializeGpu(device, shaderManager, pipelineCache,
        std::filesystem::path(PRISM_RENDER_SHADER_DIR),
        device.GetPreferredShaderBinaryFormat(), fixture.settings.quality, 2u);
    fixture.simulation.SetActiveCascadeMask(activeCascadeMask);
    if (fixture.simulation.Update(0u, 12.5, fixture.settings))
    {
        throw std::runtime_error(
            "Initial spectral update unexpectedly switched quality.");
    }
    if (fixture.simulation.PendingSpectrumVersion() != 1u
        || fixture.simulation.ActiveSpectrumVersion() != 0u)
    {
        throw std::runtime_error(
            "Initial spectral settings were not staged before GPU commit.");
    }
    const Prism::Renderer::OceanPublishedMapMetadata metadata =
        fixture.simulation.PublishedMetadata();
    if (!fixture.simulation.IsGpuReady()
        || metadata.resolution
            != Prism::Renderer::OceanSettings::ResolutionForQuality(quality)
        || metadata.cascadeCount != 4u
        || metadata.mipLevelCount != static_cast<std::uint32_t>(
            std::bit_width(metadata.resolution))
        || metadata.displacementFormat != Prism::RHI::Format::Rgba16Float
        || metadata.gradientFormat != Prism::RHI::Format::Rgba16Float
        || metadata.momentFormat != Prism::RHI::Format::Rgba16Float
        || metadata.foamFormat != Prism::RHI::Format::Rgba16Float
        || !metadata.yUpXzHorizontal)
    {
        throw std::runtime_error(
            "Spectral published-map metadata is incomplete or incompatible.");
    }
    for (const Prism::Renderer::OceanPublishedMapResource* map : {
             &fixture.simulation.DisplacementMap(),
             &fixture.simulation.GradientMap(),
             &fixture.simulation.SlopeMomentMap(),
             &fixture.simulation.FoamMap()})
    {
        if (map->sampledMips.size() != metadata.mipLevelCount
            || map->storageMips.size() != metadata.mipLevelCount
            || map->texture->GetDescription().arrayLayers != 4u
            || map->texture->GetDescription().mipLevels
                != metadata.mipLevelCount)
        {
            throw std::runtime_error(
                "A spectral published map does not expose every mip and slice.");
        }
    }

    for (std::uint32_t cascade = 0u; cascade < 4u; ++cascade)
    {
        for (std::uint32_t mip = 0u; mip < metadata.mipLevelCount; ++mip)
        {
            const std::uint32_t mipResolution = std::max(
                metadata.resolution >> mip, 1u);
            const std::uint32_t index = cascade * metadata.mipLevelCount
                + mip;
            fixture.coordinates[index] = {
                std::min((17u + cascade * 7u) >> mip,
                    mipResolution - 1u),
                std::min((23u + cascade * 5u) >> mip,
                    mipResolution - 1u),
                cascade, mip};
        }
    }
    const std::array<std::uint32_t, 4> constants{
        static_cast<std::uint32_t>(fixture.coordinates.size()), 0u, 0u, 0u};
    Prism::RHI::BufferDescription constantDescription{};
    constantDescription.size = sizeof(constants);
    constantDescription.stride = sizeof(constants);
    constantDescription.usage = Prism::RHI::BufferUsage::Constant;
    constantDescription.memoryAccess = Prism::RHI::MemoryAccess::CpuToGpu;
    fixture.constants = device.CreateBuffer(constantDescription,
        constants.data());
    Prism::RHI::BufferDescription coordinateDescription{};
    coordinateDescription.size = sizeof(fixture.coordinates);
    coordinateDescription.stride = sizeof(Uint4);
    coordinateDescription.usage = Prism::RHI::BufferUsage::Storage
        | Prism::RHI::BufferUsage::CopyDestination;
    coordinateDescription.memoryAccess = Prism::RHI::MemoryAccess::GpuOnly;
    fixture.coordinateBuffer = device.CreateBuffer(coordinateDescription,
        fixture.coordinates.data());
    Prism::RHI::BufferDescription outputDescription{};
    outputDescription.size = sizeof(SimulationValidationSample)
        * fixture.coordinates.size();
    outputDescription.stride = sizeof(SimulationValidationSample);
    outputDescription.usage = Prism::RHI::BufferUsage::Storage
        | Prism::RHI::BufferUsage::CopySource
        | Prism::RHI::BufferUsage::CopyDestination;
    outputDescription.memoryAccess = Prism::RHI::MemoryAccess::GpuOnly;
    fixture.output = device.CreateBuffer(outputDescription);
    Prism::RHI::BufferDescription readbackDescription{};
    readbackDescription.size = outputDescription.size;
    readbackDescription.stride = outputDescription.stride;
    readbackDescription.usage = Prism::RHI::BufferUsage::CopyDestination;
    readbackDescription.memoryAccess = Prism::RHI::MemoryAccess::GpuToCpu;
    fixture.readback = device.CreateBuffer(readbackDescription);

    const Prism::RHI::ShaderBinary& shader = shaderManager.LoadShader(
        std::filesystem::path(PRISM_RENDER_SHADER_DIR)
            / "Ocean/OceanSimulationValidation.slang",
        "SampleOceanSimulationCS", Prism::RHI::ShaderStage::Compute,
        device.GetPreferredShaderBinaryFormat());
    const std::array stages{Prism::RHI::ShaderLayoutStage{
        &shader.reflection, Prism::RHI::ShaderStage::Compute}};
    const auto layout = device.CreateDescriptorSetLayout(
        Prism::RHI::BuildDescriptorSetLayout(stages, {}));
    Prism::RHI::ComputePipelineDescription description{};
    description.computeShader = shader;
    description.descriptorSetLayout = layout;
    fixture.samplePipeline = pipelineCache.GetOrCreateCompute(device,
        "Test.SpectralOcean.SamplePublishedMaps", description);
    fixture.sampleSet = device.CreateDescriptorSet(layout);
    fixture.sampleSet->WriteBuffer(0u, fixture.constants);
    fixture.sampleSet->WriteTextureView(16u,
        fixture.simulation.SpectrumA(0u).sampledArray);
    fixture.sampleSet->WriteTextureView(17u,
        fixture.simulation.DisplacementMap().sampledArray);
    fixture.sampleSet->WriteTextureView(18u,
        fixture.simulation.GradientMap().sampledArray);
    fixture.sampleSet->WriteTextureView(19u,
        fixture.simulation.SlopeMomentMap().sampledArray);
    fixture.sampleSet->WriteTextureView(20u,
        fixture.simulation.FoamMap().sampledArray);
    fixture.sampleSet->WriteBuffer(21u, fixture.coordinateBuffer);
    fixture.sampleSet->WriteBuffer(36u, fixture.output);
    fixture.gpuQuery.InitializeGpu(device, shaderManager, pipelineCache,
        std::filesystem::path(PRISM_RENDER_SHADER_DIR) / "Ocean",
        device.GetPreferredShaderBinaryFormat());
    const std::array<Float2, 2> queryPoints{{
        {0.0f, 0.0f},
        {1000.0f, 0.0f}}};
    const auto patchLengths =
        Prism::Renderer::OceanSettings::ReferenceCascadePatchLengths();
    Prism::Renderer::OceanGpuQueryConstants queryConstants{};
    queryConstants.queryCount = static_cast<std::uint32_t>(queryPoints.size());
    queryConstants.resolution = metadata.resolution;
    queryConstants.localEnabled = 1u;
    queryConstants.simulationTimeSeconds = 12.5f;
    queryConstants.domainSizeMeters = fixture.settings.local.domainSizeMeters;
    queryConstants.cascadePatchLengths = {
        patchLengths[0], patchLengths[1], patchLengths[2], patchLengths[3]};
    Prism::RHI::BufferDescription queryConstantDescription{};
    queryConstantDescription.size = sizeof(queryConstants);
    queryConstantDescription.stride = sizeof(queryConstants);
    queryConstantDescription.usage = Prism::RHI::BufferUsage::Constant;
    queryConstantDescription.memoryAccess = Prism::RHI::MemoryAccess::CpuToGpu;
    fixture.queryConstants = device.CreateBuffer(queryConstantDescription,
        &queryConstants);
    Prism::Renderer::OceanGpuQueryConstants spectralOnlyConstants =
        queryConstants;
    spectralOnlyConstants.localEnabled = 0u;
    fixture.spectralOnlyConstants = device.CreateBuffer(
        queryConstantDescription, &spectralOnlyConstants);
    Prism::RHI::BufferDescription queryPointDescription{};
    queryPointDescription.size = sizeof(queryPoints);
    queryPointDescription.stride = sizeof(Float2);
    queryPointDescription.usage = Prism::RHI::BufferUsage::Storage;
    queryPointDescription.memoryAccess = Prism::RHI::MemoryAccess::GpuOnly;
    fixture.queryPoints = device.CreateBuffer(queryPointDescription,
        queryPoints.data());
    Prism::RHI::BufferDescription queryResultDescription{};
    queryResultDescription.size = sizeof(Float4) * queryPoints.size();
    queryResultDescription.stride = sizeof(Float4);
    queryResultDescription.usage = Prism::RHI::BufferUsage::Storage
        | Prism::RHI::BufferUsage::CopySource
        | Prism::RHI::BufferUsage::CopyDestination;
    queryResultDescription.memoryAccess = Prism::RHI::MemoryAccess::GpuOnly;
    fixture.queryResults = device.CreateBuffer(queryResultDescription);
    Prism::RHI::BufferDescription queryReadbackDescription =
        queryResultDescription;
    queryReadbackDescription.usage = Prism::RHI::BufferUsage::CopyDestination;
    queryReadbackDescription.memoryAccess = Prism::RHI::MemoryAccess::GpuToCpu;
    fixture.queryReadback = device.CreateBuffer(queryReadbackDescription);
    fixture.spectralOnlyResults = device.CreateBuffer(queryResultDescription);
    fixture.spectralOnlyReadback = device.CreateBuffer(queryReadbackDescription);

    // Vertical-only local offset keeps the Eulerian horizontal inversion
    // identical, so subtracting spectral-only isolates the exact local height.
    const std::array<std::uint8_t, 4> localTexel{0u, 128u, 0u, 0u};
    constexpr std::uint32_t LocalQueryWidth = 17u;
    constexpr std::uint32_t LocalQueryHeight = 23u;
    std::vector<std::uint8_t> localTexels(
        static_cast<std::size_t>(LocalQueryWidth) * LocalQueryHeight
            * localTexel.size());
    for (std::size_t index = 0; index < localTexels.size(); index += 4u)
    {
        std::ranges::copy(localTexel, localTexels.begin() + index);
    }
    Prism::RHI::TextureDescription localDescription{};
    localDescription.width = LocalQueryWidth;
    localDescription.height = LocalQueryHeight;
    localDescription.format = Prism::RHI::Format::Rgba8Unorm;
    localDescription.usage = Prism::RHI::TextureUsage::ShaderResource;
    Prism::RHI::TextureInitialData localInitialData{
        localTexels.data(),
        static_cast<std::size_t>(LocalQueryWidth) * localTexel.size(),
        localTexels.size()};
    fixture.localTexture = device.CreateTexture(localDescription,
        &localInitialData);
    Prism::RHI::TextureViewDescription localViewDescription{};
    localViewDescription.type = Prism::RHI::TextureViewType::Sampled;
    localViewDescription.format = Prism::RHI::Format::Rgba8Unorm;
    fixture.localTextureView = device.CreateTextureView(
        fixture.localTexture, localViewDescription);
    Prism::Renderer::OceanGpuQueryBinding queryBinding{
        fixture.queryConstants, fixture.queryPoints, fixture.queryResults,
        fixture.simulation.DisplacementMap().sampledArray,
        fixture.localTextureView};
    fixture.querySet = fixture.gpuQuery.Bind(queryBinding);
    queryBinding.constants = fixture.spectralOnlyConstants;
    queryBinding.results = fixture.spectralOnlyResults;
    fixture.spectralOnlyQuerySet = fixture.gpuQuery.Bind(queryBinding);
    return fixture;
}

void ExecuteFullSimulationFixture(Prism::RHI::ICommandContext& commandContext,
    FullSimulationFixture& fixture)
{
    const auto transitionWorking = [&commandContext](
        const Prism::Renderer::OceanSpectrumWorkingResource& resource)
    {
        commandContext.TextureViewBarrier(*resource.storageArray,
            Prism::RHI::ResourceState::Undefined,
            Prism::RHI::ResourceState::UnorderedAccess);
    };
    transitionWorking(fixture.simulation.InitialSpectrum());
    for (std::uint32_t ping = 0u; ping < 2u; ++ping)
    {
        transitionWorking(fixture.simulation.SpectrumA(ping));
        transitionWorking(fixture.simulation.SpectrumB(ping));
    }
    for (const Prism::Renderer::OceanPublishedMapResource* map : {
             &fixture.simulation.DisplacementMap(),
             &fixture.simulation.GradientMap(),
             &fixture.simulation.SlopeMomentMap()})
    {
        commandContext.TextureViewBarrier(*map->sampledArray,
            Prism::RHI::ResourceState::Undefined,
            Prism::RHI::ResourceState::UnorderedAccess);
    }
    commandContext.TextureViewBarrier(
        *fixture.simulation.FoamHistoryMap(
            fixture.simulation.FoamReadIndex()).sampledArray,
        Prism::RHI::ResourceState::Undefined,
        Prism::RHI::ResourceState::ShaderResource);
    commandContext.TextureViewBarrier(
        *fixture.simulation.FoamHistoryMap(
            fixture.simulation.FoamWriteIndex()).sampledArray,
        Prism::RHI::ResourceState::Undefined,
        Prism::RHI::ResourceState::UnorderedAccess);
    commandContext.BufferBarrier({fixture.output.get(),
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::UnorderedAccess});

    fixture.simulation.ExecuteInitialSpectrum(commandContext, 0u);
    commandContext.TextureViewBarrier(
        *fixture.simulation.InitialSpectrum().sampledArray,
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::ShaderResource);
    fixture.simulation.ExecuteEvolution(commandContext, 0u);
    commandContext.GlobalBarrier({Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::UnorderedAccess});
    fixture.simulation.ExecuteHorizontalFft(commandContext);
    fixture.simulation.ExecuteVerticalFft(commandContext);
    commandContext.TextureViewBarrier(
        *fixture.simulation.SpectrumA(0u).sampledArray,
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::ShaderResource);
    commandContext.TextureViewBarrier(
        *fixture.simulation.SpectrumB(0u).sampledArray,
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::ShaderResource);
    fixture.simulation.ExecuteOutputMaps(commandContext);
    commandContext.GlobalBarrier({Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::UnorderedAccess});
    // Match the production graph: foam samples the base displacement/gradient,
    // while the mip builder subsequently accesses them as storage images.
    for (const auto* map : {&fixture.simulation.DisplacementMap(), &fixture.simulation.GradientMap()})
        commandContext.TextureViewBarrier(*map->sampledMips.front(),
            Prism::RHI::ResourceState::UnorderedAccess,
            Prism::RHI::ResourceState::ShaderResource);
    fixture.simulation.ExecuteFoam(commandContext, 0u);
    for (const auto* map : {&fixture.simulation.DisplacementMap(), &fixture.simulation.GradientMap()})
        commandContext.TextureViewBarrier(*map->sampledMips.front(),
            Prism::RHI::ResourceState::ShaderResource,
            Prism::RHI::ResourceState::UnorderedAccess);
    commandContext.GlobalBarrier({Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::UnorderedAccess});
    fixture.simulation.ExecuteMips(commandContext);
    for (const Prism::Renderer::OceanPublishedMapResource* map : {
             &fixture.simulation.DisplacementMap(),
             &fixture.simulation.GradientMap(),
             &fixture.simulation.SlopeMomentMap(),
             &fixture.simulation.FoamMap()})
    {
        commandContext.TextureViewBarrier(*map->sampledArray,
            Prism::RHI::ResourceState::UnorderedAccess,
            Prism::RHI::ResourceState::ShaderResource);
    }
    // localTexture has initial data: upload completion already leaves it in
    // ShaderResource. Undefined here would discard data / lie about its state.
    commandContext.BindComputePipeline(*fixture.samplePipeline);
    commandContext.BindDescriptorSet(*fixture.sampleSet);
    commandContext.Dispatch(1u, 1u, 1u);
    commandContext.BufferBarrier({fixture.output.get(),
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::CopySource});
    commandContext.CopyBuffer(*fixture.output, *fixture.readback,
        sizeof(SimulationValidationSample) * fixture.coordinates.size());

    commandContext.BufferBarrier({fixture.queryResults.get(),
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::UnorderedAccess});
    commandContext.BufferBarrier({fixture.spectralOnlyResults.get(),
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::UnorderedAccess});
    fixture.gpuQuery.Execute(commandContext, *fixture.querySet, 2u);
    commandContext.BufferBarrier({fixture.queryResults.get(),
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::CopySource});
    commandContext.CopyBuffer(*fixture.queryResults, *fixture.queryReadback,
        sizeof(Float4) * 2u);

    fixture.gpuQuery.Execute(commandContext,
        *fixture.spectralOnlyQuerySet, 2u);
    commandContext.BufferBarrier({fixture.spectralOnlyResults.get(),
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::CopySource});
    commandContext.CopyBuffer(*fixture.spectralOnlyResults,
        *fixture.spectralOnlyReadback, sizeof(Float4) * 2u);
}

[[nodiscard]] std::array<SimulationValidationSample,
    FullSimulationFixture::SampleCount> ReadFullSimulationFixture(
    const FullSimulationFixture& fixture)
{
    std::array<SimulationValidationSample,
        FullSimulationFixture::SampleCount> samples{};
    fixture.readback->Read(samples.data(), sizeof(samples));
    return samples;
}

void ValidateOceanGpuQuery(const FullSimulationFixture& fixture)
{
    std::array<Float4, 2> localSamples{};
    std::array<Float4, 2> spectralOnlySamples{};
    fixture.queryReadback->Read(localSamples.data(), sizeof(localSamples));
    fixture.spectralOnlyReadback->Read(spectralOnlySamples.data(),
        sizeof(spectralOnlySamples));
    const auto finite = [](const Float4& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y)
            && std::isfinite(value.z) && std::isfinite(value.w);
    };
    for (const Float4& sample : localSamples)
    {
        if (!finite(sample) || std::abs(sample.w - 12.5f) > 1.0e-4f)
        {
            throw std::runtime_error(
                "GPU displacement query returned a non-finite sample or lost simulation time.");
        }
    }
    const float outsideDifference = std::max({
        std::abs(localSamples[1].x - spectralOnlySamples[1].x),
        std::abs(localSamples[1].y - spectralOnlySamples[1].y),
        std::abs(localSamples[1].z - spectralOnlySamples[1].z),
        std::abs(localSamples[1].w - spectralOnlySamples[1].w)});
    if (outsideDifference > 1.0e-3f)
    {
        throw std::runtime_error(
            "GPU displacement query incorrectly applied local displacement outside its domain.");
    }
    const Float4 localContribution{
        localSamples[0].x - spectralOnlySamples[0].x,
        localSamples[0].y - spectralOnlySamples[0].y,
        localSamples[0].z - spectralOnlySamples[0].z,
        0.0f};
    if (std::abs(localContribution.x) > 2.0e-3f
        || std::abs(localContribution.y - 128.0f / 255.0f) > 2.0e-3f
        || std::abs(localContribution.z) > 2.0e-3f)
    {
        throw std::runtime_error(
            "GPU displacement query did not combine the local-domain sample.");
    }
    std::cout << "GPU displacement query inside/outside domain passed.\n";
}

float MaximumChannelDifference(const Float4& left, const Float4& right)
{
    return std::max({std::abs(left.x - right.x),
        std::abs(left.y - right.y), std::abs(left.z - right.z),
        std::abs(left.w - right.w)});
}

void ValidateFullSimulationFixtures(const FullSimulationFixture& baseline,
    const FullSimulationFixture& replay,
    const FullSimulationFixture& masked)
{
    const auto baselineSamples = ReadFullSimulationFixture(baseline);
    const auto replaySamples = ReadFullSimulationFixture(replay);
    const auto maskedSamples = ReadFullSimulationFixture(masked);
    const auto finite = [](const Float4& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y)
            && std::isfinite(value.z) && std::isfinite(value.w);
    };
    float maximumHeight = 0.0f;
    float maximumNormalSlope = 0.0f;
    float maximumFolding = 0.0f;
    float maximumFoam = 0.0f;
    for (std::size_t index = 0; index < baselineSamples.size(); ++index)
    {
        const SimulationValidationSample& sample = baselineSamples[index];
        maximumHeight = std::max(maximumHeight,
            std::abs(sample.displacement.y));
        maximumNormalSlope = std::max(maximumNormalSlope,
            std::sqrt(sample.gradient.x * sample.gradient.x
                + sample.gradient.z * sample.gradient.z));
        maximumFolding = std::max(maximumFolding, sample.gradient.w);
        maximumFoam = std::max(maximumFoam, sample.foam.x);
        if (!finite(sample.spectrum) || !finite(sample.displacement)
            || !finite(sample.gradient) || !finite(sample.moments)
            || !finite(sample.foam)
            || sample.foam.x < -0.001f || sample.foam.x > 1.001f)
        {
            throw std::runtime_error(
                "Four-cascade simulation produced a non-finite map sample.");
        }
        const float normalLength = std::sqrt(sample.gradient.x
                * sample.gradient.x
            + sample.gradient.y * sample.gradient.y
            + sample.gradient.z * sample.gradient.z);
        if (std::abs(normalLength - 1.0f) > 0.01f
            || sample.moments.z < -0.001f || sample.moments.w < -0.001f)
        {
            throw std::runtime_error(
                "Derived normal or slope moments violate map invariants.");
        }
        if (MaximumChannelDifference(sample.spectrum,
                replaySamples[index].spectrum) > 1.0e-6f
            || MaximumChannelDifference(sample.displacement,
                replaySamples[index].displacement) > 1.0e-6f
            || MaximumChannelDifference(sample.gradient,
                replaySamples[index].gradient) > 1.0e-6f
            || MaximumChannelDifference(sample.moments,
                replaySamples[index].moments) > 1.0e-6f
            || MaximumChannelDifference(sample.foam,
                replaySamples[index].foam) > 1.0e-6f)
        {
            throw std::runtime_error(
                "Absolute-time four-cascade replay is not deterministic.");
        }
    }
    constexpr std::uint32_t mipCount = 8u;
    for (std::uint32_t cascade : {0u, 2u, 3u})
    {
        for (std::uint32_t mip = 0u; mip < mipCount; ++mip)
        {
            const std::uint32_t index = cascade * mipCount + mip;
            if (MaximumChannelDifference(baselineSamples[index].displacement,
                    maskedSamples[index].displacement) > 0.002f
                || MaximumChannelDifference(baselineSamples[index].gradient,
                    maskedSamples[index].gradient) > 0.002f
                || MaximumChannelDifference(baselineSamples[index].moments,
                    maskedSamples[index].moments) > 0.002f
                || MaximumChannelDifference(baselineSamples[index].foam,
                    maskedSamples[index].foam) > 0.002f)
            {
                throw std::runtime_error(
                    "Disabling one cascade altered an enabled cascade.");
            }
        }
    }
    for (std::uint32_t mip = 0u; mip < mipCount; ++mip)
    {
        const std::uint32_t index = mipCount + mip;
        const auto& sample = maskedSamples[index];
        if (std::abs(sample.displacement.x) > 0.001f
            || std::abs(sample.displacement.y) > 0.001f
            || std::abs(sample.displacement.z) > 0.001f
            || std::abs(sample.displacement.w - 1.0f) > 0.002f
            || std::abs(sample.gradient.x) > 0.001f
            || std::abs(sample.gradient.y - 1.0f) > 0.002f
            || std::abs(sample.gradient.z) > 0.001f
            || MaximumChannelDifference(sample.moments, {}) > 0.001f
            || MaximumChannelDifference(sample.foam, {}) > 0.001f)
        {
            throw std::runtime_error(
                "Disabled cascade did not publish a coherent flat map.");
        }
    }
    std::cout << "Four-cascade evolution, array IFFT, maps, mips, replay, and isolation passed"
              << " (sample max height " << maximumHeight
              << ", normal slope " << maximumNormalSlope
              << ", folding " << maximumFolding
              << ", foam " << maximumFoam << ").\n";
}

void ValidateQualitySwitching(Prism::RHI::IGraphicsDevice& device,
    Prism::Asset::ShaderManager& shaderManager,
    Prism::Renderer::PipelineCache& pipelineCache)
{
    Prism::Renderer::SpectralOceanSimulation simulation;
    simulation.InitializeGpu(device, shaderManager, pipelineCache,
        std::filesystem::path(PRISM_RENDER_SHADER_DIR),
        device.GetPreferredShaderBinaryFormat(),
        Prism::Renderer::OceanSimulationQuality::Normal, 2u);
    Prism::Renderer::OceanSettings settings =
        Prism::Renderer::OceanSettings::WaveWorksReference();
    settings.quality = Prism::Renderer::OceanSimulationQuality::Normal;
    (void)simulation.Update(0u, 1.0, settings);
    const auto normalStatistics = simulation.GetStatistics();
    if (!simulation.IsFoamHistoryResetPending()
        || normalStatistics.cascadeResolution != 128u
        || normalStatistics.dispatchCount != 25u
        || !(normalStatistics.allocatedMegabytes > 8.0f
            && normalStatistics.allocatedMegabytes < 9.0f))
    {
        throw std::runtime_error(
            "Normal spectral resource/dispatch statistics are invalid.");
    }
    const std::uint64_t initialGeneration = simulation.ResourceGeneration();
    const std::uint64_t initialHistory = simulation.HistoryVersion();
    settings.quality = Prism::Renderer::OceanSimulationQuality::High;
    if (!simulation.Update(1u, 1.0, settings)
        || simulation.Resolution() != 256u
        || simulation.MipLevelCount() != 9u
        || !simulation.IsFoamHistoryResetPending())
    {
        throw std::runtime_error("Normal-to-High quality switch failed.");
    }
    const auto highStatistics = simulation.GetStatistics();
    if (highStatistics.cascadeResolution != 256u
        || highStatistics.dispatchCount != 28u
        || !(highStatistics.allocatedMegabytes > 33.0f
            && highStatistics.allocatedMegabytes < 34.0f))
    {
        throw std::runtime_error(
            "High spectral resource/dispatch statistics are invalid.");
    }
    settings.quality = Prism::Renderer::OceanSimulationQuality::Extreme;
    if (!simulation.Update(0u, 1.0, settings)
        || simulation.Resolution() != 512u
        || simulation.MipLevelCount() != 10u
        || simulation.RetiredResourceSetCount() == 0u
        || !simulation.IsFoamHistoryResetPending())
    {
        throw std::runtime_error("High-to-Extreme quality switch failed.");
    }
    const auto extremeStatistics = simulation.GetStatistics();
    if (extremeStatistics.cascadeResolution != 512u
        || extremeStatistics.dispatchCount != 31u
        || !(extremeStatistics.allocatedMegabytes > 133.0f
            && extremeStatistics.allocatedMegabytes < 134.0f))
    {
        throw std::runtime_error(
            "Extreme spectral resource/dispatch statistics are invalid.");
    }
    (void)simulation.Update(1u, 1.0, settings);
    (void)simulation.Update(0u, 1.0, settings);
    if (simulation.RetiredResourceSetCount() != 0u)
    {
        throw std::runtime_error(
            "Frame-safe quality retirement did not release expired sets.");
    }
    settings.quality = Prism::Renderer::OceanSimulationQuality::Normal;
    if (!simulation.Update(1u, 1.0, settings)
        || simulation.Resolution() != 128u
        || simulation.ResourceGeneration() != initialGeneration + 3u
        || simulation.HistoryVersion() != initialHistory + 3u)
    {
        throw std::runtime_error(
            "Repeated quality switching lost generation/history reset metadata.");
    }
    const auto finalStatistics = simulation.GetStatistics();
    if (finalStatistics.cascadeResolution != 128u
        || finalStatistics.publishedVersion
            != simulation.ResourceGeneration()
        || finalStatistics.dispatchCount != 25u)
    {
        throw std::runtime_error(
            "Statistics describe a retired rather than active resource set.");
    }
    simulation.EndFrame(true);
    if (simulation.IsFoamHistoryResetPending())
    {
        throw std::runtime_error(
            "A completed coherent frame did not commit the foam history reset.");
    }
    const std::uint64_t spectrumResetVersion = simulation.HistoryVersion();
    settings.baseWind.speed += 0.25f;
    (void)simulation.Update(0u, 2.0, settings);
    if (!simulation.IsFoamHistoryResetPending()
        || simulation.HistoryVersion() != spectrumResetVersion + 1u)
    {
        throw std::runtime_error(
            "An incompatible spectrum change retained stale foam history.");
    }
    simulation.EndFrame(true);
    const std::uint64_t explicitResetVersion = simulation.HistoryVersion();
    simulation.ResetFoamHistory();
    if (!simulation.IsFoamHistoryResetPending()
        || simulation.HistoryVersion() != explicitResetVersion + 1u)
    {
        throw std::runtime_error(
            "Explicit simulation-history reset did not invalidate foam.");
    }
    // In-flight lifecycle stress: alternate quality, simulation pause
    // (repeated absolute time), local/history resets, and frame slots.  The
    // resource retirement queue must stay bounded and never expose a retired
    // generation to a later update.
    for (std::uint32_t iteration = 0u; iteration < 18u; ++iteration)
    {
        settings.quality = static_cast<Prism::Renderer::OceanSimulationQuality>(
            iteration % 3u);
        const double time = (iteration & 1u) != 0u ? 2.0 : 2.0 + iteration;
        (void)simulation.Update(iteration % 2u, time, settings);
        if ((iteration % 4u) == 0u)
            simulation.ResetFoamHistory();
        simulation.EndFrame(true);
        if (simulation.RetiredResourceSetCount() > 2u)
            throw std::runtime_error(
                "In-flight ocean lifecycle stress retained too many resource generations.");
    }
    std::cout << "Normal/High/Extreme frame-safe resource switching passed.\n";
}

void ValidateExtremeSimulation(const FullSimulationFixture& fixture)
{
    const auto samples = ReadFullSimulationFixture(fixture);
    float maximumHeight = 0.0f;
    float maximumSlope = 0.0f;
    for (const auto& sample : samples)
    {
        maximumHeight = std::max(maximumHeight,
            std::abs(sample.displacement.y));
        maximumSlope = std::max(maximumSlope,
            std::sqrt(sample.gradient.x * sample.gradient.x
                + sample.gradient.z * sample.gradient.z));
    }
    if (!(maximumHeight > 0.001f) || !(maximumSlope > 0.001f))
    {
        throw std::runtime_error(
            "Extreme 512 spectral simulation published a flat map.");
    }
    std::cout << "Extreme 512 array IFFT published non-flat maps"
              << " (sample max height " << maximumHeight
              << ", normal slope " << maximumSlope << ").\n";
}

void ExecuteInitialSpectrumFixture(
    Prism::RHI::ICommandContext& commandContext,
    InitialSpectrumFixture& fixture)
{
    const auto& initialSpectrum = fixture.simulation.InitialSpectrum();
    commandContext.TextureViewBarrier(*initialSpectrum.storageArray,
        Prism::RHI::ResourceState::Undefined,
        Prism::RHI::ResourceState::UnorderedAccess);
    commandContext.BufferBarrier({fixture.output.get(),
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::UnorderedAccess});
    commandContext.BindComputePipeline(*fixture.generatePipeline);
    commandContext.BindDescriptorSet(*fixture.generateSet);
    const std::uint32_t groups =
        (fixture.simulation.Resolution() + 7u) / 8u;
    commandContext.Dispatch(groups, groups,
        Prism::Renderer::SpectralOceanSimulation::CascadeCount);
    commandContext.TextureViewBarrier(*initialSpectrum.storageArray,
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::ShaderResource);
    commandContext.BindComputePipeline(*fixture.samplePipeline);
    commandContext.BindDescriptorSet(*fixture.sampleSet);
    commandContext.Dispatch(
        (static_cast<std::uint32_t>(fixture.coordinates.size()) + 63u) / 64u,
        1u, 1u);
    commandContext.BufferBarrier({fixture.output.get(),
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::CopySource});
    commandContext.CopyBuffer(*fixture.output, *fixture.readback,
        sizeof(Float4) * fixture.coordinates.size());
}

void ValidateInitialSpectrumFixture(const InitialSpectrumFixture& fixture)
{
    std::array<Float4, 8> gpuSamples{};
    fixture.readback->Read(gpuSamples.data(), sizeof(gpuSamples));
    const auto& generator = fixture.simulation.SpectrumGenerator();
    const auto& cascades = generator.GetCascades();
    for (std::size_t index = 0; index < fixture.coordinates.size(); ++index)
    {
        const Uint4 coordinate = fixture.coordinates[index];
        const float patchLength = cascades[coordinate.z].patchLengthMeters;
        const float centeredX = static_cast<float>(
            static_cast<std::int32_t>(coordinate.x)
            - static_cast<std::int32_t>(fixture.simulation.Resolution() / 2u));
        const float centeredY = static_cast<float>(
            static_cast<std::int32_t>(coordinate.y)
            - static_cast<std::int32_t>(fixture.simulation.Resolution() / 2u));
        const float deltaK = 2.0f * 3.14159265358979323846f / patchLength;
        const float waveNumber = deltaK
            * std::sqrt(centeredX * centeredX + centeredY * centeredY);
        const float wavelength = 2.0f * 3.14159265358979323846f
            / waveNumber;
        const auto weights = generator.BandWeights(wavelength);
        const auto cpu = Prism::Renderer::EvaluateInitialSpectrumComponents(
            fixture.settings, patchLength, weights[coordinate.z],
            fixture.simulation.Resolution(), coordinate.x, coordinate.y,
            coordinate.z, InitialSpectrumFixture::Seed);
        const std::array<float, 4> expected{
            cpu.baseH0.x, cpu.baseH0.y, cpu.swellH0.x, cpu.swellH0.y};
        const std::array<float, 4> actual{
            gpuSamples[index].x, gpuSamples[index].y,
            gpuSamples[index].z, gpuSamples[index].w};
        for (std::size_t channel = 0; channel < expected.size(); ++channel)
        {
            const float tolerance = std::max(
                Prism::Renderer::OceanInitialSpectrumAbsoluteTolerance,
                std::abs(expected[channel])
                    * Prism::Renderer::OceanInitialSpectrumRelativeTolerance);
            if (!std::isfinite(actual[channel])
                || std::abs(actual[channel] - expected[channel]) > tolerance)
            {
                throw std::runtime_error(
                    "GPU/CPU JONSWAP initial-spectrum mismatch at sample "
                    + std::to_string(index) + ", channel "
                    + std::to_string(channel) + ": expected "
                    + std::to_string(expected[channel]) + ", received "
                    + std::to_string(actual[channel]));
            }
        }
    }
    std::cout << "GPU/CPU deterministic JONSWAP initial-spectrum samples passed.\n";
}

void ExecuteSpectrumResourceFixture(
    Prism::RHI::ICommandContext& commandContext,
    SpectrumResourceFixture& fixture)
{
    const auto& resource = fixture.simulation.SpectrumA(0u);
    commandContext.TextureViewBarrier(*resource.storageArray,
        Prism::RHI::ResourceState::Undefined,
        Prism::RHI::ResourceState::UnorderedAccess);
    commandContext.BufferBarrier({fixture.output.get(),
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::UnorderedAccess});
    commandContext.BindComputePipeline(*fixture.writePipeline);
    commandContext.BindDescriptorSet(*fixture.writeSet);
    const std::uint32_t groupCount =
        (fixture.simulation.Resolution() + 7u) / 8u;
    commandContext.Dispatch(groupCount, groupCount,
        Prism::Renderer::SpectralOceanSimulation::CascadeCount);
    commandContext.TextureViewBarrier(*resource.storageArray,
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::ShaderResource);
    commandContext.BindComputePipeline(*fixture.readPipeline);
    commandContext.BindDescriptorSet(*fixture.readSet);
    commandContext.Dispatch(1u, 1u,
        Prism::Renderer::SpectralOceanSimulation::CascadeCount);
    commandContext.BufferBarrier({fixture.output.get(),
        Prism::RHI::ResourceState::UnorderedAccess,
        Prism::RHI::ResourceState::CopySource});
    commandContext.CopyBuffer(*fixture.output, *fixture.readback,
        sizeof(Float4)
            * Prism::Renderer::SpectralOceanSimulation::CascadeCount);
}

void ValidateSpectrumResourceFixture(
    const SpectrumResourceFixture& fixture)
{
    std::array<Float4,
        Prism::Renderer::SpectralOceanSimulation::CascadeCount> result{};
    fixture.readback->Read(result.data(), sizeof(result));
    for (std::uint32_t cascade = 0u; cascade < result.size(); ++cascade)
    {
        const Float4 expected{
            static_cast<float>(cascade + 1u),
            5.0f / 128.0f,
            7.0f / 128.0f,
            static_cast<float>(100u + cascade)};
        const Float4& actual = result[cascade];
        const float error = std::max({
            std::abs(actual.x - expected.x),
            std::abs(actual.y - expected.y),
            std::abs(actual.z - expected.z),
            std::abs(actual.w - expected.w)});
        if (!std::isfinite(error) || error > 1.0e-6f)
        {
            throw std::runtime_error(
                "Spectrum array UAV-write/SRV-read validation failed at cascade "
                + std::to_string(cascade));
        }
    }
    std::cout << "Four-slice RGBA32F spectrum resource validation passed.\n";
}

void ValidateFixture(const Fixture& fixture)
{
    std::vector<Float2> result(fixture.size);
    fixture.readback->Read(
        result.data(), result.size() * sizeof(Float2));
    float maximumError = 0.0f;
    for (std::uint32_t index = 0u; index < fixture.size; ++index)
    {
        if (!std::isfinite(result[index].x)
            || !std::isfinite(result[index].y))
        {
            throw std::runtime_error(
                "GPU FFT produced a non-finite value.");
        }
        maximumError = std::max(maximumError,
            std::abs(result[index].x - fixture.expected[index].x));
        maximumError = std::max(maximumError,
            std::abs(result[index].y - fixture.expected[index].y));
        if (fixture.requireRealOutput
            && std::abs(result[index].y)
                >= Prism::Renderer::OceanFft::Fp32RegressionTolerance)
        {
            throw std::runtime_error(
                "GPU Hermitian FFT produced a non-real spatial result.");
        }
    }
    if (maximumError
        >= Prism::Renderer::OceanFft::Fp32RegressionTolerance)
    {
        throw std::runtime_error(
            "GPU FFT fixture exceeded tolerance at size "
            + std::to_string(fixture.size) + " (" + fixture.label + ")"
            + ": " + std::to_string(maximumError));
    }
    std::cout << "GPU FFT " << fixture.size
              << " " << fixture.label << " max error: "
              << maximumError << '\n';
}
} // namespace

int main(int argc, char** argv)
{
    // Preserve the last completed fixture when a backend reports a device
    // failure instead of a C++ exception.
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    try
    {
        const Prism::RHI::GraphicsApi api = ParseApi(argc, argv);
        Prism::Platform::Window window("Ocean FFT GPU Tests", 64u, 64u);
        std::unique_ptr<Prism::RHI::IRenderBackend> backend =
            Prism::RHI::CreateRenderBackend(api);
        backend->Initialize(window);
        Prism::RHI::IGraphicsDevice& device =
            backend->GetGraphicsDevice();
        Prism::Asset::ShaderManager shaderManager;
        if (argc == 3)
        {
            ValidateOceanFallbackResources(*backend, shaderManager);
            return EXIT_SUCCESS;
        }
        Prism::Renderer::PipelineCache pipelineCache;
        const std::filesystem::path shaderPath =
            std::filesystem::path(PRISM_RENDER_SHADER_DIR)
            / "Ocean/OceanFft.slang";
        const Prism::RHI::ShaderBinary& shader = shaderManager.LoadShader(
            shaderPath,
            "BufferButterflyCS",
            Prism::RHI::ShaderStage::Compute,
            device.GetPreferredShaderBinaryFormat());
        const std::array stages{Prism::RHI::ShaderLayoutStage{
            &shader.reflection, Prism::RHI::ShaderStage::Compute}};
        const std::shared_ptr<Prism::RHI::IDescriptorSetLayout> layout =
            device.CreateDescriptorSetLayout(
                Prism::RHI::BuildDescriptorSetLayout(stages, {}));
        Prism::RHI::ComputePipelineDescription pipelineDescription{};
        pipelineDescription.computeShader = shader;
        pipelineDescription.descriptorSetLayout = layout;
        const std::shared_ptr<Prism::RHI::IComputePipeline> pipeline =
            pipelineCache.GetOrCreateCompute(device,
                "Test.OceanFft.BufferButterfly", pipelineDescription);
        SpectrumResourceFixture spectrumResources =
            BuildSpectrumResourceFixture(
                device, shaderManager, pipelineCache);
        InitialSpectrumFixture initialSpectrumNormal =
            BuildInitialSpectrumFixture(device, shaderManager, pipelineCache,
                Prism::Renderer::OceanSimulationQuality::Normal);
        InitialSpectrumFixture initialSpectrumHigh =
            BuildInitialSpectrumFixture(device, shaderManager, pipelineCache,
                Prism::Renderer::OceanSimulationQuality::High);
        InitialSpectrumFixture initialSpectrumExtreme =
            BuildInitialSpectrumFixture(device, shaderManager, pipelineCache,
                Prism::Renderer::OceanSimulationQuality::Extreme);
        FullSimulationFixture fullSimulation = BuildFullSimulationFixture(
            device, shaderManager, pipelineCache,
            Prism::Renderer::SpectralOceanSimulation::AllCascadesMask);
        FullSimulationFixture replaySimulation = BuildFullSimulationFixture(
            device, shaderManager, pipelineCache,
            Prism::Renderer::SpectralOceanSimulation::AllCascadesMask);
        FullSimulationFixture maskedSimulation = BuildFullSimulationFixture(
            device, shaderManager, pipelineCache,
            Prism::Renderer::SpectralOceanSimulation::AllCascadesMask
                & ~(1u << 1u));
        FullSimulationFixture extremeSimulation = BuildFullSimulationFixture(
            device, shaderManager, pipelineCache,
            Prism::Renderer::SpectralOceanSimulation::AllCascadesMask,
                Prism::Renderer::OceanSimulationQuality::Extreme);

        std::vector<Fixture> fixtures;
        for (const std::uint32_t size :
             Prism::Renderer::OceanFft::SupportedSizes)
        {
            for (const FftFixtureKind kind : {
                     FftFixtureKind::ImpulseRoundTrip,
                     FftFixtureKind::ConstantRoundTrip,
                     FftFixtureKind::SinusoidRoundTrip,
                     FftFixtureKind::HermitianInverse,
                     FftFixtureKind::NormalizedInverse})
            {
                fixtures.push_back(BuildFixture(device, layout, size, kind));
            }
        }

        Prism::RHI::IFrameContext& frame = backend->GetFrameContext();
        if (frame.BeginFrame() != Prism::RHI::FrameResult::Ready)
        {
            throw std::runtime_error("GPU FFT test could not begin a frame.");
        }
        Prism::RHI::ICommandContext& commandContext =
            backend->GetCommandContext();
        ExecuteSpectrumResourceFixture(commandContext, spectrumResources);
        ExecuteInitialSpectrumFixture(commandContext, initialSpectrumNormal);
        ExecuteInitialSpectrumFixture(commandContext, initialSpectrumHigh);
        ExecuteInitialSpectrumFixture(commandContext, initialSpectrumExtreme);
        ExecuteFullSimulationFixture(commandContext, fullSimulation);
        ExecuteFullSimulationFixture(commandContext, replaySimulation);
        ExecuteFullSimulationFixture(commandContext, maskedSimulation);
        ExecuteFullSimulationFixture(commandContext, extremeSimulation);
        commandContext.BindComputePipeline(*pipeline);
        for (Fixture& fixture : fixtures)
        {
            for (std::size_t passIndex = 0;
                 passIndex < fixture.passes.size(); ++passIndex)
            {
                commandContext.BindDescriptorSet(*fixture.sets[passIndex]);
                commandContext.Dispatch((fixture.size + 63u) / 64u, 1u, 1u);
                commandContext.GlobalBarrier({
                    Prism::RHI::ResourceState::UnorderedAccess,
                    Prism::RHI::ResourceState::UnorderedAccess});
            }
            commandContext.BufferBarrier({
                fixture.ping[fixture.outputIndex].get(),
                Prism::RHI::ResourceState::UnorderedAccess,
                Prism::RHI::ResourceState::CopySource});
            commandContext.CopyBuffer(*fixture.ping[fixture.outputIndex],
                *fixture.readback,
                static_cast<std::size_t>(fixture.size) * sizeof(Float2));
        }
        PrepareFixturePresent(*backend);
        if (frame.EndFrame() != Prism::RHI::FrameResult::Ready)
        {
            throw std::runtime_error("GPU FFT test could not end its frame.");
        }
        frame.WaitForGpu();
        for (const Fixture& fixture : fixtures)
        {
            ValidateFixture(fixture);
        }
        ValidateSpectrumResourceFixture(spectrumResources);
        ValidateInitialSpectrumFixture(initialSpectrumNormal);
        ValidateInitialSpectrumFixture(initialSpectrumHigh);
        ValidateInitialSpectrumFixture(initialSpectrumExtreme);
        ValidateFullSimulationFixtures(fullSimulation, replaySimulation,
            maskedSimulation);
        ValidateOceanGpuQuery(fullSimulation);
        ValidateExtremeSimulation(extremeSimulation);
        ValidateQualitySwitching(device, shaderManager, pipelineCache);
        ValidateOceanFallbackResources(*backend, shaderManager);
        std::cout << "Ocean FFT GPU tests passed on "
                  << backend->GetAdapterName() << ".\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Ocean FFT GPU tests failed: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
