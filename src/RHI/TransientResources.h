#pragma once

#include "RHI/GraphicsResources.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Prism::RHI
{
class IGraphicsDevice;

struct TransientTextureRequest
{
    std::string name;
    TextureDescription description;
    std::size_t allocationIndex = 0;
};

struct TransientTexturePoolStatistics
{
    std::uint64_t poolId = 0;
    std::size_t textureCount = 0;
    std::size_t allocationCount = 0;
    std::uint64_t logicalBytes = 0;
    std::uint64_t physicalBytes = 0;
    std::uint64_t aliasedBytes = 0;
};

struct TransientBufferRequest
{
    std::string name;
    BufferDescription description;
    std::size_t allocationIndex = 0;
};

struct TransientBufferPoolStatistics
{
    std::uint64_t poolId = 0;
    std::size_t bufferCount = 0;
    std::size_t allocationCount = 0;
    std::uint64_t logicalBytes = 0;
    std::uint64_t physicalBytes = 0;
    std::uint64_t aliasedBytes = 0;
};

class ITransientTexturePool : public IGraphicsResource
{
public:
    ~ITransientTexturePool() override = default;

    virtual std::shared_ptr<ITexture> GetTexture(
        std::string_view name) const = 0;
    virtual const TransientTexturePoolStatistics&
        GetStatistics() const = 0;
};

class ITransientBufferPool : public IGraphicsResource
{
public:
    ~ITransientBufferPool() override = default;

    virtual std::shared_ptr<IBuffer> GetBuffer(
        std::string_view name) const = 0;
    virtual const TransientBufferPoolStatistics&
        GetStatistics() const = 0;
};

bool ValidateTransientTextureRequests(
    const std::vector<TransientTextureRequest>& requests,
    std::string* outError = nullptr);
bool ValidateTransientBufferRequests(
    const std::vector<TransientBufferRequest>& requests,
    std::string* outError = nullptr);
bool RunTransientBufferPoolValidation(
    IGraphicsDevice& graphicsDevice,
    std::string* outError = nullptr);
std::uint64_t AllocateTransientPoolId();
} // namespace Prism::RHI
