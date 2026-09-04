#pragma once

#include "RHI/TransientResources.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Prism::RHI::Vulkan
{
class VulkanContext;

class VulkanTransientTexturePool final
    : public ITransientTexturePool
{
public:
    VulkanTransientTexturePool(
        VulkanContext& context,
        const std::vector<TransientTextureRequest>&
            requests);

    GraphicsApi GetGraphicsApi() const override;
    std::shared_ptr<ITexture> GetTexture(
        std::string_view name) const override;
    const TransientTexturePoolStatistics&
        GetStatistics() const override;

private:
    std::unordered_map<std::string, std::shared_ptr<ITexture>>
        m_textures;
    TransientTexturePoolStatistics m_statistics;
};

class VulkanTransientBufferPool final
    : public ITransientBufferPool
{
public:
    VulkanTransientBufferPool(
        VulkanContext& context,
        const std::vector<TransientBufferRequest>&
            requests);

    GraphicsApi GetGraphicsApi() const override;
    std::shared_ptr<IBuffer> GetBuffer(
        std::string_view name) const override;
    const TransientBufferPoolStatistics&
        GetStatistics() const override;

private:
    std::unordered_map<std::string, std::shared_ptr<IBuffer>>
        m_buffers;
    TransientBufferPoolStatistics m_statistics;
};
} // namespace Prism::RHI::Vulkan
