#pragma once

#include "RHI/GraphicsApi.h"
#include "RHI/RayTracing.h"

#include <compare>
#include <cstdint>
#include <string>

namespace Prism::RHI
{
struct DeviceLimits
{
    std::uint32_t maxTextureDimension2D = 0;
    std::uint32_t maxTextureArrayLayers = 0;
    std::uint32_t maxColorAttachments = 0;
    std::uint32_t maxSamplerAnisotropy = 1;
    std::uint64_t minConstantBufferOffsetAlignment = 1;
    std::uint64_t minStorageBufferOffsetAlignment = 1;
    std::uint32_t shaderVisibleResourceDescriptorCapacity = 0;
    std::uint32_t shaderVisibleSamplerDescriptorCapacity = 0;
    std::uint32_t maxRayRecursionDepth = 0;
    std::uint32_t rayTracingShaderGroupHandleSize = 0;
    std::uint32_t rayTracingShaderGroupBaseAlignment = 0;
    std::uint64_t accelerationStructureScratchAlignment = 0;
    std::uint32_t maxTessellationControlPoints = 0;
};

struct DeviceFeatures
{
    bool graphicsQueue = true;
    bool computeQueue = false;
    bool dedicatedComputeQueue = false;
    bool copyQueue = false;
    bool dedicatedCopyQueue = false;
    bool timelineSynchronization = false;
    bool dynamicRendering = false;
    bool descriptorIndexing = false;
    bool bufferDeviceAddress = false;
    bool drawIndirectCount = false;
    bool samplerAnisotropy = false;
    bool nativeParallelCommandRecording = false;
    bool rayTracingAccelerationStructure = false;
    bool rayTracingPipeline = false;
    bool rayQuery = false;
    bool tessellationShader = false;
    bool patchListTopology = false;
};

struct GraphicsDeviceCapabilities
{
    GraphicsApi graphicsApi =
        GraphicsApi::Direct3D12;
    std::string adapterName;
    DeviceLimits limits;
    DeviceFeatures features;
    RayTracingTier rayTracingTier =
        RayTracingTier::Unsupported;
};

struct UploadTicket
{
    std::uint64_t value = 0;

    [[nodiscard]] bool IsValid() const
    {
        return value != 0;
    }

    auto operator<=>(const UploadTicket&) const = default;
};

struct DescriptorAllocatorStatistics
{
    std::uint32_t poolCount = 0;
    std::uint32_t setCapacity = 0;
    std::uint32_t allocatedSetCount = 0;
    std::uint32_t setHighWatermark = 0;
    std::uint32_t pendingReleaseCount = 0;
    std::uint32_t resourceDescriptorCapacity = 0;
    std::uint32_t allocatedResourceDescriptorCount = 0;
    std::uint32_t resourceDescriptorHighWatermark = 0;
    std::uint32_t samplerDescriptorCapacity = 0;
    std::uint32_t allocatedSamplerDescriptorCount = 0;
    std::uint32_t samplerDescriptorHighWatermark = 0;
};

struct UploadQueueStatistics
{
    std::uint64_t uploadedBytes = 0;
    std::uint64_t submittedBatchCount = 0;
    std::uint64_t synchronousFlushCount = 0;
    std::uint64_t maximumBatchBytes = 0;
    std::uint32_t pendingOperationCount = 0;
    std::uint64_t pendingBytes = 0;
    std::uint64_t pendingTicket = 0;
    std::uint64_t lastSubmittedTicket = 0;
    std::uint64_t completedTicket = 0;
    std::uint64_t outstandingBatchCount = 0;
    std::uint32_t stagingPageCount = 0;
    std::uint64_t stagingCapacityBytes = 0;
    std::uint64_t stagingHighWatermarkBytes = 0;
};

struct ResourceRetirementStatistics
{
    std::uint64_t totalRetiredObjectCount = 0;
    std::uint64_t totalReclaimedObjectCount = 0;
    std::uint32_t pendingObjectCount = 0;
    std::uint32_t pendingObjectHighWatermark = 0;
};
} // namespace Prism::RHI
