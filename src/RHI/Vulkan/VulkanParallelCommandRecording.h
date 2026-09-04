#pragma once

#include "RHI/ICommandContext.h"
#include "RHI/Vulkan/VulkanLoader.h"

#include <memory>

namespace Prism::RHI::Vulkan
{
class VulkanContext;

class VulkanParallelCommandRecording final
    : public IParallelCommandRecording
{
public:
    VulkanParallelCommandRecording(
        VulkanContext& context,
        CommandQueueType queue);
    ~VulkanParallelCommandRecording() override;

    VulkanParallelCommandRecording(
        const VulkanParallelCommandRecording&) = delete;
    VulkanParallelCommandRecording& operator=(
        const VulkanParallelCommandRecording&) = delete;

    ICommandContext& GetCommandContext() override;
    bool Close() override;

    [[nodiscard]] CommandQueueType GetQueue() const;
    [[nodiscard]] VkCommandBuffer GetCommandBuffer() const;
    VkCommandPool ReleaseCommandPool();

private:
    VulkanContext* m_context = nullptr;
    CommandQueueType m_queue =
        CommandQueueType::Graphics;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer =
        VK_NULL_HANDLE;
    std::unique_ptr<ICommandContext>
        m_commandContext;
    bool m_closed = false;
};
} // namespace Prism::RHI::Vulkan
