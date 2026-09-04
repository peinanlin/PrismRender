#pragma once

#include "RHI/GraphicsResources.h"

#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Prism::RHI
{
enum class RayTracingTier : std::uint8_t
{
    Unsupported,
    Tier1_0,
    Tier1_1
};

enum class AccelerationStructureType : std::uint8_t
{
    BottomLevel,
    TopLevel
};

enum class RayTracingGeometryFlags : std::uint32_t
{
    None = 0,
    Opaque = 1u << 0u,
    NoDuplicateAnyHitInvocation = 1u << 1u
};

enum class AccelerationStructureBuildFlags : std::uint32_t
{
    None = 0,
    AllowUpdate = 1u << 0u,
    AllowCompaction = 1u << 1u,
    PreferFastTrace = 1u << 2u,
    PreferFastBuild = 1u << 3u,
    MinimizeMemory = 1u << 4u
};

constexpr RayTracingGeometryFlags operator|(
    const RayTracingGeometryFlags lhs,
    const RayTracingGeometryFlags rhs)
{
    return static_cast<RayTracingGeometryFlags>(
        static_cast<std::uint32_t>(lhs)
        | static_cast<std::uint32_t>(rhs));
}

constexpr AccelerationStructureBuildFlags operator|(
    const AccelerationStructureBuildFlags lhs,
    const AccelerationStructureBuildFlags rhs)
{
    return static_cast<AccelerationStructureBuildFlags>(
        static_cast<std::uint32_t>(lhs)
        | static_cast<std::uint32_t>(rhs));
}

constexpr bool HasAnyFlag(
    const RayTracingGeometryFlags value,
    const RayTracingGeometryFlags flags)
{
    return (static_cast<std::uint32_t>(value)
            & static_cast<std::uint32_t>(flags))
        != 0;
}

constexpr bool HasAnyFlag(
    const AccelerationStructureBuildFlags value,
    const AccelerationStructureBuildFlags flags)
{
    return (static_cast<std::uint32_t>(value)
            & static_cast<std::uint32_t>(flags))
        != 0;
}

struct RayTracingTrianglesDescription
{
    std::uint32_t vertexCount = 0;
    std::uint32_t vertexStride = 0;
    std::uint32_t indexCount = 0;
    IndexFormat indexFormat = IndexFormat::UInt32;
    RayTracingGeometryFlags flags =
        RayTracingGeometryFlags::Opaque;
};

struct AccelerationStructureBuildDescription
{
    AccelerationStructureType type =
        AccelerationStructureType::BottomLevel;
    std::vector<RayTracingTrianglesDescription> geometries;
    std::uint32_t instanceCount = 0;
    AccelerationStructureBuildFlags flags =
        AccelerationStructureBuildFlags::PreferFastTrace;
};

struct AccelerationStructureBuildSizes
{
    std::uint64_t accelerationStructureBytes = 0;
    std::uint64_t buildScratchBytes = 0;
    std::uint64_t updateScratchBytes = 0;
};

class IRayTracingAccelerationStructure;

struct RayTracingGeometryBuildInput
{
    RayTracingTrianglesDescription description;
    std::shared_ptr<IBuffer> vertexBuffer;
    std::uint64_t vertexBufferOffset = 0;
    std::shared_ptr<IBuffer> indexBuffer;
    std::uint64_t indexBufferOffset = 0;
};

enum class RayTracingInstanceFlags : std::uint32_t
{
    None = 0,
    TriangleCullDisable = 1u << 0u,
    TriangleFrontCounterClockwise = 1u << 1u,
    ForceOpaque = 1u << 2u,
    ForceNonOpaque = 1u << 3u
};

constexpr RayTracingInstanceFlags operator|(
    const RayTracingInstanceFlags lhs,
    const RayTracingInstanceFlags rhs)
{
    return static_cast<RayTracingInstanceFlags>(
        static_cast<std::uint32_t>(lhs)
        | static_cast<std::uint32_t>(rhs));
}

constexpr bool HasAnyFlag(
    const RayTracingInstanceFlags value,
    const RayTracingInstanceFlags flags)
{
    return (static_cast<std::uint32_t>(value)
            & static_cast<std::uint32_t>(flags))
        != 0;
}

struct RayTracingInstanceDescription
{
    std::array<float, 12> transform = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    std::uint32_t instanceId = 0;
    std::uint8_t mask = 0xff;
    std::uint32_t hitGroupOffset = 0;
    RayTracingInstanceFlags flags =
        RayTracingInstanceFlags::None;
    std::shared_ptr<IRayTracingAccelerationStructure>
        bottomLevel;
};

struct AccelerationStructureBuildRequest
{
    AccelerationStructureBuildDescription description;
    std::vector<RayTracingGeometryBuildInput>
        geometries;
    std::vector<RayTracingInstanceDescription>
        instances;
};

class IRayTracingAccelerationStructure
    : public IGraphicsResource
{
public:
    virtual const AccelerationStructureBuildDescription&
        GetDescription() const = 0;
    virtual const AccelerationStructureBuildSizes&
        GetBuildSizes() const = 0;
    virtual std::uint64_t GetDeviceAddress() const = 0;
    // Opaque backend handle bits are used only by the matching RHI
    // descriptor implementation. Engine and renderer code must not
    // interpret this value.
    virtual std::uint64_t GetNativeHandleBits() const = 0;
};

bool ValidateAccelerationStructureBuildDescription(
    const AccelerationStructureBuildDescription& description,
    std::string* errorMessage = nullptr);
bool ValidateAccelerationStructureBuildRequest(
    const AccelerationStructureBuildRequest& request,
    GraphicsApi graphicsApi,
    std::string* errorMessage = nullptr);

constexpr const char* ToString(const RayTracingTier tier)
{
    switch (tier)
    {
    case RayTracingTier::Unsupported: return "unsupported";
    case RayTracingTier::Tier1_0: return "1.0";
    case RayTracingTier::Tier1_1: return "1.1";
    }
    return "unknown";
}
} // namespace Prism::RHI
