#include "RHI/TransientResources.h"

#include "RHI/IGraphicsDevice.h"

#include <atomic>
#include <unordered_map>
#include <unordered_set>

namespace Prism::RHI
{
namespace
{
bool AreAliasCompatible(
    const TextureDescription& left,
    const TextureDescription& right)
{
    return left.dimension == right.dimension
        && left.width == right.width
        && left.height == right.height
        && left.arrayLayers == right.arrayLayers
        && left.mipLevels == right.mipLevels
        && left.sampleCount == right.sampleCount
        && left.format == right.format
        && left.usage == right.usage
        && left.memoryAccess == right.memoryAccess;
}

bool AreAliasCompatible(
    const BufferDescription& left,
    const BufferDescription& right)
{
    return left.size == right.size
        && left.stride == right.stride
        && left.usage == right.usage
        && left.memoryAccess == right.memoryAccess;
}
} // namespace

bool ValidateTransientTextureRequests(
    const std::vector<TransientTextureRequest>& requests,
    std::string* outError)
{
    if (requests.empty())
    {
        if (outError != nullptr)
        {
            *outError =
                "A transient texture pool requires at least one texture.";
        }
        return false;
    }

    std::unordered_set<std::string> names;
    std::unordered_map<std::size_t, TextureDescription>
        allocationDescriptions;
    for (const TransientTextureRequest& request : requests)
    {
        if (request.name.empty())
        {
            if (outError != nullptr)
            {
                *outError =
                    "Transient texture names must not be empty.";
            }
            return false;
        }
        if (!names.emplace(request.name).second)
        {
            if (outError != nullptr)
            {
                *outError =
                    "Transient texture names must be unique.";
            }
            return false;
        }

        std::string validationError;
        if (!ValidateTextureDescription(
                request.description,
                &validationError))
        {
            if (outError != nullptr)
            {
                *outError = validationError;
            }
            return false;
        }
        if (request.description.memoryAccess
            != MemoryAccess::GpuOnly)
        {
            if (outError != nullptr)
            {
                *outError =
                    "Transient textures must use GPU-only memory.";
            }
            return false;
        }

        const auto [found, inserted] =
            allocationDescriptions.try_emplace(
                request.allocationIndex,
                request.description);
        if (!inserted
            && !AreAliasCompatible(
                found->second,
                request.description))
        {
            if (outError != nullptr)
            {
                *outError =
                    "Textures sharing a transient allocation must "
                    "have identical descriptions.";
            }
            return false;
        }
    }
    return true;
}

bool ValidateTransientBufferRequests(
    const std::vector<TransientBufferRequest>& requests,
    std::string* outError)
{
    if (requests.empty())
    {
        if (outError != nullptr)
        {
            *outError =
                "A transient buffer pool requires at least one buffer.";
        }
        return false;
    }

    std::unordered_set<std::string> names;
    std::unordered_map<std::size_t, BufferDescription>
        allocationDescriptions;
    for (const TransientBufferRequest& request : requests)
    {
        if (request.name.empty())
        {
            if (outError != nullptr)
            {
                *outError =
                    "Transient buffer names must not be empty.";
            }
            return false;
        }
        if (!names.emplace(request.name).second)
        {
            if (outError != nullptr)
            {
                *outError =
                    "Transient buffer names must be unique.";
            }
            return false;
        }

        std::string validationError;
        if (!ValidateBufferDescription(
                request.description,
                &request.description,
                &validationError))
        {
            if (outError != nullptr)
            {
                *outError = validationError;
            }
            return false;
        }
        if (request.description.memoryAccess
            != MemoryAccess::GpuOnly)
        {
            if (outError != nullptr)
            {
                *outError =
                    "Transient buffers must use GPU-only memory.";
            }
            return false;
        }

        const auto [found, inserted] =
            allocationDescriptions.try_emplace(
                request.allocationIndex,
                request.description);
        if (!inserted
            && !AreAliasCompatible(
                found->second,
                request.description))
        {
            if (outError != nullptr)
            {
                *outError =
                    "Buffers sharing a transient allocation must "
                    "have identical descriptions.";
            }
            return false;
        }
    }
    return true;
}

bool RunTransientBufferPoolValidation(
    IGraphicsDevice& graphicsDevice,
    std::string* outError)
{
    BufferDescription description{};
    description.size = 4096;
    description.stride = 16;
    description.usage =
        BufferUsage::Storage
        | BufferUsage::CopyDestination;
    description.memoryAccess = MemoryAccess::GpuOnly;
    const std::vector<TransientBufferRequest> requests{
        {"ValidationA", description, 0},
        {"ValidationB", description, 0}};
    const std::shared_ptr<ITransientBufferPool> pool =
        graphicsDevice.CreateTransientBufferPool(
            requests);
    if (pool == nullptr)
    {
        if (outError != nullptr)
        {
            *outError =
                "The graphics device returned no transient buffer pool.";
        }
        return false;
    }
    const std::shared_ptr<IBuffer> first =
        pool->GetBuffer("ValidationA");
    const std::shared_ptr<IBuffer> second =
        pool->GetBuffer("ValidationB");
    const TransientBufferAllocationInfo* firstInfo =
        first != nullptr
        ? first->GetTransientAllocationInfo()
        : nullptr;
    const TransientBufferAllocationInfo* secondInfo =
        second != nullptr
        ? second->GetTransientAllocationInfo()
        : nullptr;
    const TransientBufferPoolStatistics& statistics =
        pool->GetStatistics();
    const bool valid =
        firstInfo != nullptr
        && secondInfo != nullptr
        && firstInfo->poolId == statistics.poolId
        && secondInfo->poolId == statistics.poolId
        && firstInfo->allocationIndex
            == secondInfo->allocationIndex
        && statistics.bufferCount == 2
        && statistics.allocationCount == 1
        && statistics.logicalBytes
            > statistics.physicalBytes
        && statistics.aliasedBytes
            == statistics.logicalBytes
                - statistics.physicalBytes;
    if (!valid && outError != nullptr)
    {
        *outError =
            "Transient buffer pool metadata or native alias accounting is invalid.";
    }
    return valid;
}

std::uint64_t AllocateTransientPoolId()
{
    static std::atomic_uint64_t nextId{1};
    return nextId.fetch_add(1, std::memory_order_relaxed);
}
} // namespace Prism::RHI
