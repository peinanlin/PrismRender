#pragma once

#include "RHI/TransientResources.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Prism::RHI
{
class D3D12Context;
}

namespace Prism::RHI::D3D12
{
class D3D12TransientTexturePool final
    : public ITransientTexturePool
{
public:
    D3D12TransientTexturePool(
        D3D12Context& context,
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

class D3D12TransientBufferPool final
    : public ITransientBufferPool
{
public:
    D3D12TransientBufferPool(
        D3D12Context& context,
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
} // namespace Prism::RHI::D3D12
