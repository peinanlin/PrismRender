#include "Renderer/Features/Ocean/OceanFft.h"

#include "Asset/ShaderManager.h"
#include "Core/Assert.h"
#include "RHI/ICommandContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"
#include "Renderer/Pipeline/PipelineCache.h"

#include <array>
#include <utility>

namespace Prism::Renderer
{
void OceanFft::InitializeGpu(RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager, PipelineCache& pipelineCache,
    const std::filesystem::path& shaderPath,
    const RHI::ShaderBinaryFormat shaderFormat)
{
    Core::Check(m_size != 0u,
        "Ocean FFT must be configured before GPU initialization.");
    const RHI::ShaderBinary& shader = shaderManager.LoadShader(
        shaderPath, "TextureButterflyCS", RHI::ShaderStage::Compute,
        shaderFormat);
    const RHI::ShaderBinary& arrayShader = shaderManager.LoadShader(
        shaderPath, "TextureArrayButterflyCS", RHI::ShaderStage::Compute,
        shaderFormat);
    const RHI::ShaderBinary& arrayRadixShader = shaderManager.LoadShader(
        shaderPath, "TextureArrayRadix2CS", RHI::ShaderStage::Compute,
        shaderFormat);
    const std::array stages{RHI::ShaderLayoutStage{
        &shader.reflection, RHI::ShaderStage::Compute}};
    m_device = &device;
    m_textureLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(stages, {}));
    const std::array arrayStages{RHI::ShaderLayoutStage{
        &arrayShader.reflection, RHI::ShaderStage::Compute}};
    m_arrayTextureLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(arrayStages, {}));
    const std::array arrayRadixStages{RHI::ShaderLayoutStage{
        &arrayRadixShader.reflection, RHI::ShaderStage::Compute}};
    m_arrayRadixLayout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(arrayRadixStages, {}));
    RHI::ComputePipelineDescription description{};
    description.computeShader = shader;
    description.descriptorSetLayout = m_textureLayout;
    m_texturePipeline = pipelineCache.GetOrCreateCompute(
        device, "Feature.OceanFft.TextureButterfly", description);
    description.computeShader = arrayShader;
    description.descriptorSetLayout = m_arrayTextureLayout;
    m_arrayTexturePipeline = pipelineCache.GetOrCreateCompute(
        device, "Feature.OceanFft.TextureArrayButterfly", description);
    // The production transform keeps an entire row/column in group-shared
    // memory.  The stage-by-stage pipeline remains available for numerical
    // fixtures, while the ocean uses two dispatches instead of 2*log2(N).
    description.computeShader = arrayRadixShader;
    description.descriptorSetLayout = m_arrayRadixLayout;
    m_arrayRadixPipeline = pipelineCache.GetOrCreateCompute(
        device, "Feature.OceanFft.TextureArrayRadix2", description);
}

void OceanFft::BindTextureArrayWorkingSet(
    const std::array<std::shared_ptr<RHI::ITextureView>, 2>& spectrumA,
    const std::array<std::shared_ptr<RHI::ITextureView>, 2>& spectrumB,
    const std::uint32_t arrayLayerCount)
{
    Core::Check(m_device != nullptr && m_arrayTextureLayout != nullptr,
        "Ocean FFT array pipeline must be initialized before binding textures.");
    Core::Check(arrayLayerCount > 0u,
        "Ocean FFT array working set requires at least one layer.");
    const std::vector<OceanFftPass> passes = BuildPasses(
        2u, true, 0u, false, false);
    m_arrayLayerCount = arrayLayerCount;
    m_arrayStageConstants.clear();
    m_arrayStageSets.clear();
    m_arrayStageConstants.reserve(passes.size());
    m_arrayStageSets.reserve(passes.size());
    for (const OceanFftPass& pass : passes)
    {
        GpuConstants constants{};
        constants.resolution = m_size;
        constants.stage = pass.stage;
        constants.direction = pass.direction;
        constants.inverse = pass.inverse ? 1u : 0u;
        RHI::BufferDescription bufferDescription{};
        bufferDescription.size = sizeof(GpuConstants);
        bufferDescription.stride = sizeof(GpuConstants);
        bufferDescription.usage = RHI::BufferUsage::Constant;
        bufferDescription.memoryAccess = RHI::MemoryAccess::CpuToGpu;
        std::shared_ptr<RHI::IBuffer> constantBuffer =
            m_device->CreateBuffer(bufferDescription, &constants);
        std::shared_ptr<RHI::IDescriptorSet> set =
            m_device->CreateDescriptorSet(m_arrayRadixLayout);
        set->WriteBuffer(0u, constantBuffer);
        set->WriteTextureView(40u, spectrumA[pass.sourceIndex]);
        set->WriteTextureView(41u, spectrumB[pass.sourceIndex]);
        set->WriteTextureView(42u, spectrumA[pass.destinationIndex]);
        set->WriteTextureView(43u, spectrumB[pass.destinationIndex]);
        m_arrayStageConstants.push_back(std::move(constantBuffer));
        m_arrayStageSets.push_back(std::move(set));
    }
    Core::Check(!passes.empty() && passes.back().destinationIndex == 0u,
        "Ocean FFT array plan must publish ping zero after two dimensions.");
    for (std::uint32_t direction = 0u; direction < 2u; ++direction)
    {
        GpuConstants constants{};
        constants.resolution = m_size;
        constants.direction = direction;
        constants.inverse = 1u;
        RHI::BufferDescription bufferDescription{};
        bufferDescription.size = sizeof(GpuConstants);
        bufferDescription.stride = sizeof(GpuConstants);
        bufferDescription.usage = RHI::BufferUsage::Constant;
        bufferDescription.memoryAccess = RHI::MemoryAccess::CpuToGpu;
        m_arrayRadixConstants[direction] = m_device->CreateBuffer(
            bufferDescription, &constants);
        m_arrayRadixSets[direction] =
            m_device->CreateDescriptorSet(m_arrayTextureLayout);
        const std::uint32_t sourceIndex = direction;
        const std::uint32_t destinationIndex = 1u - direction;
        m_arrayRadixSets[direction]->WriteBuffer(
            0u, m_arrayRadixConstants[direction]);
        m_arrayRadixSets[direction]->WriteTextureView(
            40u, spectrumA[sourceIndex]);
        m_arrayRadixSets[direction]->WriteTextureView(
            41u, spectrumB[sourceIndex]);
        m_arrayRadixSets[direction]->WriteTextureView(
            42u, spectrumA[destinationIndex]);
        m_arrayRadixSets[direction]->WriteTextureView(
            43u, spectrumB[destinationIndex]);
    }
}

void OceanFft::BindTextureWorkingSet(
    const std::array<std::shared_ptr<RHI::ITextureView>, 2>& spectrumA,
    const std::array<std::shared_ptr<RHI::ITextureView>, 2>& spectrumB)
{
    Core::Check(m_device != nullptr && m_textureLayout != nullptr,
        "Ocean FFT GPU pipeline must be initialized before binding textures.");
    const std::vector<OceanFftPass> passes = BuildPasses(
        2u, true, 0u, false, false);
    m_stageConstants.clear();
    m_stageSets.clear();
    m_stageConstants.reserve(passes.size());
    m_stageSets.reserve(passes.size());
    for (const OceanFftPass& pass : passes)
    {
        GpuConstants constants{};
        constants.resolution = m_size;
        constants.stage = pass.stage;
        constants.direction = pass.direction;
        constants.inverse = pass.inverse ? 1u : 0u;
        constants.bitReverseInput = pass.bitReverseInput ? 1u : 0u;
        constants.normalizeOutput = pass.normalizeOutput ? 1u : 0u;
        RHI::BufferDescription bufferDescription{};
        bufferDescription.size = sizeof(GpuConstants);
        bufferDescription.stride = sizeof(GpuConstants);
        bufferDescription.usage = RHI::BufferUsage::Constant;
        bufferDescription.memoryAccess = RHI::MemoryAccess::CpuToGpu;
        std::shared_ptr<RHI::IBuffer> constantBuffer =
            m_device->CreateBuffer(bufferDescription, &constants);
        std::shared_ptr<RHI::IDescriptorSet> set =
            m_device->CreateDescriptorSet(m_textureLayout);
        set->WriteBuffer(0, constantBuffer);
        set->WriteTextureView(32, spectrumA[pass.sourceIndex]);
        set->WriteTextureView(33, spectrumB[pass.sourceIndex]);
        set->WriteTextureView(34, spectrumA[pass.destinationIndex]);
        set->WriteTextureView(35, spectrumB[pass.destinationIndex]);
        m_stageConstants.push_back(std::move(constantBuffer));
        m_stageSets.push_back(std::move(set));
    }
    Core::Check(!passes.empty()
            && passes.back().destinationIndex == 0u,
        "Ocean FFT 2D inverse plan must publish ping zero.");
}

void OceanFft::ExecuteInverse2D(
    RHI::ICommandContext& commandContext) const
{
    Core::Check(IsGpuReady(),
        "Ocean FFT cannot execute before its GPU working set is ready.");
    const std::uint32_t groupCount = (m_size + 7u) / 8u;
    commandContext.BindComputePipeline(*m_texturePipeline);
    for (const std::shared_ptr<RHI::IDescriptorSet>& set : m_stageSets)
    {
        commandContext.BindDescriptorSet(*set);
        commandContext.Dispatch(groupCount, groupCount, 1u);
        commandContext.GlobalBarrier({
            RHI::ResourceState::UnorderedAccess,
            RHI::ResourceState::UnorderedAccess});
    }
}

namespace
{
void ExecuteArrayPassRange(RHI::ICommandContext& commandContext,
    const RHI::IComputePipeline& pipeline,
    const std::vector<std::shared_ptr<RHI::IDescriptorSet>>& sets,
    const std::uint32_t firstPass, const std::uint32_t passCount,
    const std::uint32_t groupCount, const std::uint32_t arrayLayerCount)
{
    commandContext.BindComputePipeline(pipeline);
    for (std::uint32_t pass = firstPass;
         pass < firstPass + passCount; ++pass)
    {
        commandContext.BindDescriptorSet(*sets[pass]);
        commandContext.Dispatch(groupCount, groupCount, arrayLayerCount);
        commandContext.GlobalBarrier({
            RHI::ResourceState::UnorderedAccess,
            RHI::ResourceState::UnorderedAccess});
    }
}
}

void OceanFft::ExecuteInverseHorizontalArray(
    RHI::ICommandContext& commandContext) const
{
    Core::Check(IsArrayGpuReady(),
        "Ocean FFT array working set is not ready.");
    commandContext.BindComputePipeline(*m_arrayRadixPipeline);
    commandContext.BindDescriptorSet(*m_arrayRadixSets[0]);
    commandContext.Dispatch(1u, m_size, m_arrayLayerCount);
    commandContext.GlobalBarrier({
        RHI::ResourceState::UnorderedAccess,
        RHI::ResourceState::UnorderedAccess});
}

void OceanFft::ExecuteInverseVerticalArray(
    RHI::ICommandContext& commandContext) const
{
    Core::Check(IsArrayGpuReady(),
        "Ocean FFT array working set is not ready.");
    commandContext.BindComputePipeline(*m_arrayRadixPipeline);
    commandContext.BindDescriptorSet(*m_arrayRadixSets[1]);
    commandContext.Dispatch(m_size, 1u, m_arrayLayerCount);
    commandContext.GlobalBarrier({
        RHI::ResourceState::UnorderedAccess,
        RHI::ResourceState::UnorderedAccess});
}

bool OceanFft::IsGpuReady() const noexcept
{
    return m_device != nullptr && m_texturePipeline != nullptr
        && !m_stageSets.empty()
        && m_stageSets.size()
            == static_cast<std::size_t>(m_stageCount) * 2u;
}

bool OceanFft::IsArrayGpuReady() const noexcept
{
    return m_device != nullptr && m_arrayTexturePipeline != nullptr
        && m_arrayRadixPipeline != nullptr
        && m_arrayLayerCount > 0u
        && m_arrayRadixSets[0] != nullptr
        && m_arrayRadixSets[1] != nullptr
        && m_arrayStageSets.size()
            == static_cast<std::size_t>(m_stageCount) * 2u;
}
} // namespace Prism::Renderer
