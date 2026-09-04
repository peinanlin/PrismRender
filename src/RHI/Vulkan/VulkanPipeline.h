#pragma once

#include "RHI/PipelineState.h"
#include "RHI/Vulkan/VulkanLoader.h"

namespace Prism::RHI::Vulkan
{
class VulkanContext;

class VulkanGraphicsPipeline final : public IGraphicsPipeline
{
public:
    VulkanGraphicsPipeline(VulkanContext& context, const GraphicsPipelineDescription& description);
    ~VulkanGraphicsPipeline() override;

    GraphicsApi GetGraphicsApi() const override;
    VkPipeline GetHandle() const;
    VkPipelineLayout GetLayout() const;

private:
    VulkanContext* m_context = nullptr;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

class VulkanComputePipeline final : public IComputePipeline
{
public:
    VulkanComputePipeline(VulkanContext& context, const ComputePipelineDescription& description);
    ~VulkanComputePipeline() override;

    GraphicsApi GetGraphicsApi() const override;
    VkPipeline GetHandle() const;
    VkPipelineLayout GetLayout() const;

private:
    VulkanContext* m_context = nullptr;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};
} // namespace Prism::RHI::Vulkan
