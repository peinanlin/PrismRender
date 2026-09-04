#include "RHI/Vulkan/VulkanPipeline.h"

#include "Core/Assert.h"
#include "RHI/Vulkan/VulkanContext.h"
#include "RHI/Vulkan/VulkanResources.h"
#include "RHI/Vulkan/VulkanTypeConversions.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Prism::RHI::Vulkan
{
namespace
{
void CheckVk(const VkResult result, const char* message)
{
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error(std::string(message) + " VkResult=" + std::to_string(static_cast<int>(result)));
    }
}

VkShaderModule CreateShaderModule(VulkanContext& context, const ShaderBinary& shader)
{
    Core::Check(shader.IsValid() && shader.Size() % sizeof(std::uint32_t) == 0,
                "Vulkan pipelines require valid SPIR-V words.");
    Core::Check(shader.format == ShaderBinaryFormat::SpirV,
                "Vulkan pipelines require SPIR-V shader binaries.");
    std::vector<std::uint32_t> code(shader.Size() / sizeof(std::uint32_t));
    std::memcpy(code.data(), shader.Data(), shader.Size());
    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = shader.Size();
    createInfo.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    CheckVk(vkCreateShaderModule(context.GetDevice(), &createInfo, nullptr, &module),
            "Failed to create Vulkan shader module.");
    return module;
}

VkFormat ToNativeVertexFormat(const VertexElementFormat format)
{
    switch (format)
    {
    case VertexElementFormat::Float: return VK_FORMAT_R32_SFLOAT;
    case VertexElementFormat::Float2: return VK_FORMAT_R32G32_SFLOAT;
    case VertexElementFormat::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexElementFormat::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VertexElementFormat::Uint: return VK_FORMAT_R32_UINT;
    }
    throw std::invalid_argument("Unsupported Vulkan vertex element format.");
}

VkPrimitiveTopology ToNativeTopology(const PrimitiveTopology topology)
{
    switch (topology)
    {
    case PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::PatchList: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    }
    throw std::invalid_argument("Unsupported Vulkan primitive topology.");
}

VkPipelineLayout CreatePipelineLayout(
    VulkanContext& context,
    const std::shared_ptr<IDescriptorSetLayout>& descriptorSetLayout)
{
    VkDescriptorSetLayout nativeLayout = VK_NULL_HANDLE;
    if (descriptorSetLayout != nullptr)
    {
        const auto layout = std::dynamic_pointer_cast<VulkanDescriptorSetLayout>(descriptorSetLayout);
        Core::Check(layout != nullptr, "Vulkan pipelines require a Vulkan descriptor-set layout.");
        nativeLayout = layout->GetHandle();
    }

    VkPipelineLayoutCreateInfo createInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (nativeLayout != VK_NULL_HANDLE)
    {
        createInfo.setLayoutCount = 1;
        createInfo.pSetLayouts = &nativeLayout;
    }
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    CheckVk(vkCreatePipelineLayout(context.GetDevice(), &createInfo, nullptr, &pipelineLayout),
            "Failed to create Vulkan pipeline layout.");
    return pipelineLayout;
}
} // namespace

VulkanGraphicsPipeline::VulkanGraphicsPipeline(
    VulkanContext& context,
    const GraphicsPipelineDescription& description)
    : m_context(&context)
{
    std::string validationError;
    Core::Check(ValidateGraphicsPipelineDescription(description, &validationError), validationError.c_str());
    Core::Check(description.vertexShader.format == ShaderBinaryFormat::SpirV,
                "Vulkan graphics pipelines require SPIR-V shaders.");

    m_layout = CreatePipelineLayout(context, description.descriptorSetLayout);
    const VkShaderModule vertexModule = CreateShaderModule(context, description.vertexShader);
    VkShaderModule pixelModule = VK_NULL_HANDLE;
    VkShaderModule hullModule = VK_NULL_HANDLE;
    VkShaderModule domainModule = VK_NULL_HANDLE;
    try
    {
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_VERTEX_BIT, vertexModule, description.vertexShader.emittedEntryPoint.c_str(), nullptr});
        if (description.pixelShader.IsValid())
        {
            pixelModule = CreateShaderModule(context, description.pixelShader);
            stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_FRAGMENT_BIT, pixelModule, description.pixelShader.emittedEntryPoint.c_str(), nullptr});
        }
        if (description.hullShader.IsValid())
        {
            hullModule = CreateShaderModule(context, description.hullShader);
            domainModule = CreateShaderModule(context, description.domainShader);
            stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, hullModule,
                description.hullShader.emittedEntryPoint.c_str(), nullptr});
            stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, domainModule,
                description.domainShader.emittedEntryPoint.c_str(), nullptr});
        }

        std::vector<VkVertexInputBindingDescription> bindings;
        bindings.reserve(description.vertexBindings.size());
        for (const VertexBufferBindingDescription& binding : description.vertexBindings)
        {
            bindings.push_back({
                binding.binding,
                binding.stride,
                binding.inputRate == VertexInputRate::PerInstance
                    ? VK_VERTEX_INPUT_RATE_INSTANCE
                    : VK_VERTEX_INPUT_RATE_VERTEX});
        }
        std::vector<VkVertexInputAttributeDescription> attributes;
        attributes.reserve(description.vertexAttributes.size());
        for (const VertexAttributeDescription& attribute : description.vertexAttributes)
        {
            attributes.push_back({
                attribute.location,
                attribute.binding,
                ToNativeVertexFormat(attribute.format),
                attribute.offset});
        }

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
        vertexInput.pVertexBindingDescriptions = bindings.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = ToNativeTopology(description.topology);
        VkPipelineTessellationStateCreateInfo tessellation{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
        tessellation.patchControlPoints = description.patchControlPointCount;

        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterizer.depthClampEnable = description.rasterizer.depthClipEnabled ? VK_FALSE : VK_TRUE;
        rasterizer.polygonMode = ToNativePolygonMode(description.rasterizer.fillMode);
        rasterizer.cullMode = ToNativeCullMode(description.rasterizer.cullMode);
        rasterizer.frontFace = ToNativeFrontFace(description.rasterizer.frontFace);
        rasterizer.depthBiasEnable = description.rasterizer.depthBias != 0
                                     || description.rasterizer.slopeScaledDepthBias != 0.0f;
        rasterizer.depthBiasConstantFactor = static_cast<float>(description.rasterizer.depthBias);
        rasterizer.depthBiasClamp = description.rasterizer.depthBiasClamp;
        rasterizer.depthBiasSlopeFactor = description.rasterizer.slopeScaledDepthBias;
        rasterizer.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = ToNativeSampleCount(description.sampleCount);

        VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable = description.depthStencil.depthTestEnabled ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = description.depthStencil.depthWriteEnabled ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = ToNativeCompareOperation(description.depthStencil.depthComparison);
        depthStencil.stencilTestEnable = description.depthStencil.stencilEnabled ? VK_TRUE : VK_FALSE;

        std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
        blendAttachments.reserve(description.blendAttachments.size());
        for (const BlendAttachmentDescription& blend : description.blendAttachments)
        {
            VkPipelineColorBlendAttachmentState native{};
            native.blendEnable = blend.blendEnabled ? VK_TRUE : VK_FALSE;
            native.srcColorBlendFactor = ToNativeBlendFactor(blend.sourceColor);
            native.dstColorBlendFactor = ToNativeBlendFactor(blend.destinationColor);
            native.colorBlendOp = ToNativeBlendOperation(blend.colorOperation);
            native.srcAlphaBlendFactor = ToNativeBlendFactor(blend.sourceAlpha);
            native.dstAlphaBlendFactor = ToNativeBlendFactor(blend.destinationAlpha);
            native.alphaBlendOp = ToNativeBlendOperation(blend.alphaOperation);
            native.colorWriteMask = blend.colorWriteMask;
            blendAttachments.push_back(native);
        }
        VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlend.attachmentCount = static_cast<std::uint32_t>(blendAttachments.size());
        colorBlend.pAttachments = blendAttachments.data();

        constexpr std::array<VkDynamicState, 2> DynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        };
        VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicState.dynamicStateCount = static_cast<std::uint32_t>(DynamicStates.size());
        dynamicState.pDynamicStates = DynamicStates.data();

        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        std::vector<VkFormat> colorFormats;
        colorFormats.reserve(description.colorFormats.size());
        for (const Format format : description.colorFormats)
        {
            colorFormats.push_back(ToNativeFormat(format));
        }
        VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        renderingInfo.colorAttachmentCount = static_cast<std::uint32_t>(colorFormats.size());
        renderingInfo.pColorAttachmentFormats = colorFormats.data();
        renderingInfo.depthAttachmentFormat = IsDepthFormat(description.depthFormat)
                                                  ? ToNativeFormat(description.depthFormat)
                                                  : VK_FORMAT_UNDEFINED;
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pTessellationState = description.hullShader.IsValid() ? &tessellation : nullptr;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = m_layout;
        pipelineInfo.renderPass = VK_NULL_HANDLE;
        CheckVk(vkCreateGraphicsPipelines(context.GetDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline),
                "Failed to create Vulkan graphics pipeline.");
    }
    catch (...)
    {
        if (pixelModule != VK_NULL_HANDLE) vkDestroyShaderModule(context.GetDevice(), pixelModule, nullptr);
        if (hullModule != VK_NULL_HANDLE) vkDestroyShaderModule(context.GetDevice(), hullModule, nullptr);
        if (domainModule != VK_NULL_HANDLE) vkDestroyShaderModule(context.GetDevice(), domainModule, nullptr);
        vkDestroyShaderModule(context.GetDevice(), vertexModule, nullptr);
        if (m_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(context.GetDevice(), m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
        throw;
    }
    if (pixelModule != VK_NULL_HANDLE) vkDestroyShaderModule(context.GetDevice(), pixelModule, nullptr);
    if (hullModule != VK_NULL_HANDLE) vkDestroyShaderModule(context.GetDevice(), hullModule, nullptr);
    if (domainModule != VK_NULL_HANDLE) vkDestroyShaderModule(context.GetDevice(), domainModule, nullptr);
    vkDestroyShaderModule(context.GetDevice(), vertexModule, nullptr);
}

VulkanGraphicsPipeline::~VulkanGraphicsPipeline()
{
    if (m_context == nullptr) return;
    const VkPipeline pipeline = std::exchange(
        m_pipeline,
        VK_NULL_HANDLE);
    const VkPipelineLayout layout = std::exchange(
        m_layout,
        VK_NULL_HANDLE);
    m_context->RetireGpuObject(
        [pipeline, layout](const VkDevice device)
        {
            if (pipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(
                    device,
                    pipeline,
                    nullptr);
            }
            if (layout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(
                    device,
                    layout,
                    nullptr);
            }
        });
}

GraphicsApi VulkanGraphicsPipeline::GetGraphicsApi() const { return GraphicsApi::Vulkan; }
VkPipeline VulkanGraphicsPipeline::GetHandle() const { return m_pipeline; }
VkPipelineLayout VulkanGraphicsPipeline::GetLayout() const { return m_layout; }

VulkanComputePipeline::VulkanComputePipeline(
    VulkanContext& context,
    const ComputePipelineDescription& description)
    : m_context(&context)
{
    std::string validationError;
    Core::Check(ValidateComputePipelineDescription(description, &validationError), validationError.c_str());
    Core::Check(description.computeShader.format == ShaderBinaryFormat::SpirV,
                "Vulkan compute pipelines require SPIR-V shaders.");
    m_layout = CreatePipelineLayout(context, description.descriptorSetLayout);
    const VkShaderModule shaderModule = CreateShaderModule(context, description.computeShader);
    try
    {
        VkComputePipelineCreateInfo createInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        createInfo.layout = m_layout;
        createInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_COMPUTE_BIT, shaderModule, description.computeShader.emittedEntryPoint.c_str(), nullptr};
        CheckVk(vkCreateComputePipelines(context.GetDevice(), VK_NULL_HANDLE, 1, &createInfo, nullptr, &m_pipeline),
                "Failed to create Vulkan compute pipeline.");
    }
    catch (...)
    {
        vkDestroyShaderModule(context.GetDevice(), shaderModule, nullptr);
        if (m_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(context.GetDevice(), m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
        throw;
    }
    vkDestroyShaderModule(context.GetDevice(), shaderModule, nullptr);
}

VulkanComputePipeline::~VulkanComputePipeline()
{
    if (m_context == nullptr) return;
    const VkPipeline pipeline = std::exchange(
        m_pipeline,
        VK_NULL_HANDLE);
    const VkPipelineLayout layout = std::exchange(
        m_layout,
        VK_NULL_HANDLE);
    m_context->RetireGpuObject(
        [pipeline, layout](const VkDevice device)
        {
            if (pipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(
                    device,
                    pipeline,
                    nullptr);
            }
            if (layout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(
                    device,
                    layout,
                    nullptr);
            }
        });
}

GraphicsApi VulkanComputePipeline::GetGraphicsApi() const { return GraphicsApi::Vulkan; }
VkPipeline VulkanComputePipeline::GetHandle() const { return m_pipeline; }
VkPipelineLayout VulkanComputePipeline::GetLayout() const { return m_layout; }
} // namespace Prism::RHI::Vulkan
