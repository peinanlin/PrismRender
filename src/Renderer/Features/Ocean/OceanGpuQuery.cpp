#include "Renderer/Features/Ocean/OceanGpuQuery.h"

#include "Asset/ShaderManager.h"
#include "Core/Assert.h"
#include "RHI/ICommandContext.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ShaderLayoutBuilder.h"
#include "Renderer/Pipeline/PipelineCache.h"

#include <array>

namespace Prism::Renderer
{
void OceanGpuQuery::InitializeGpu(
    RHI::IGraphicsDevice& device,
    Asset::ShaderManager& shaderManager,
    PipelineCache& pipelineCache,
    const std::filesystem::path& shaderDirectory,
    const RHI::ShaderBinaryFormat shaderFormat)
{
    const RHI::ShaderBinary shader = shaderManager.LoadShader(
        shaderDirectory / "OceanDisplacementQuery.slang",
        "SampleOceanDisplacementCS",
        RHI::ShaderStage::Compute,
        shaderFormat);
    const std::array stages{RHI::ShaderLayoutStage{
        &shader.reflection, RHI::ShaderStage::Compute}};
    m_device = &device;
    m_layout = device.CreateDescriptorSetLayout(
        RHI::BuildDescriptorSetLayout(stages, {}));
    RHI::ComputePipelineDescription description{};
    description.computeShader = shader;
    description.descriptorSetLayout = m_layout;
    m_pipeline = pipelineCache.GetOrCreateCompute(
        device, "Feature.OceanGpuQuery", description);
}

std::shared_ptr<RHI::IDescriptorSet> OceanGpuQuery::Bind(
    const OceanGpuQueryBinding& binding)
{
    Core::Check(IsReady(), "Ocean GPU query must be initialized before binding.");
    Core::Check(binding.constants != nullptr && binding.points != nullptr
            && binding.results != nullptr
            && binding.spectralDisplacement != nullptr
            && binding.localDisplacement != nullptr,
        "Ocean GPU query bindings are incomplete.");
    const std::shared_ptr<RHI::IDescriptorSet> set =
        m_device->CreateDescriptorSet(m_layout);
    set->WriteBuffer(0u, binding.constants);
    set->WriteBuffer(16u, binding.points);
    set->WriteTextureView(17u, binding.spectralDisplacement);
    set->WriteTextureView(18u, binding.localDisplacement);
    set->WriteBuffer(32u, binding.results);
    return set;
}

void OceanGpuQuery::Execute(
    RHI::ICommandContext& commandContext,
    const RHI::IDescriptorSet& descriptorSet,
    const std::uint32_t queryCount) const
{
    Core::Check(IsReady() && queryCount > 0u,
        "Ocean GPU query dispatch requires a ready non-empty batch.");
    commandContext.BindComputePipeline(*m_pipeline);
    commandContext.BindDescriptorSet(descriptorSet);
    commandContext.Dispatch((queryCount + 63u) / 64u, 1u, 1u);
}
} // namespace Prism::Renderer
