#include "RHI/RayTracing.h"

#include <string_view>

namespace Prism::RHI
{
namespace
{
bool Fail(std::string* errorMessage, const std::string_view message)
{
    if (errorMessage != nullptr)
    {
        *errorMessage = message;
    }
    return false;
}
} // namespace

bool ValidateAccelerationStructureBuildDescription(
    const AccelerationStructureBuildDescription& description,
    std::string* errorMessage)
{
    if (description.type
        == AccelerationStructureType::BottomLevel)
    {
        if (description.geometries.empty())
        {
            return Fail(
                errorMessage,
                "A bottom-level acceleration structure requires at least one triangle geometry.");
        }
        if (description.instanceCount != 0)
        {
            return Fail(
                errorMessage,
                "A bottom-level acceleration structure cannot contain instances.");
        }
        for (const RayTracingTrianglesDescription& geometry :
             description.geometries)
        {
            if (geometry.vertexCount < 3
                || geometry.vertexStride < 12)
            {
                return Fail(
                    errorMessage,
                    "Ray-tracing triangle geometry requires at least three float3 vertices.");
            }
            if (geometry.indexCount != 0
                && (geometry.indexCount < 3
                    || geometry.indexCount % 3 != 0))
            {
                return Fail(
                    errorMessage,
                    "Indexed ray-tracing geometry requires a triangle-list index count.");
            }
            if (geometry.indexCount == 0
                && geometry.vertexCount % 3 != 0)
            {
                return Fail(
                    errorMessage,
                    "Non-indexed ray-tracing geometry requires a triangle-list vertex count.");
            }
        }
    }
    else
    {
        if (!description.geometries.empty())
        {
            return Fail(
                errorMessage,
                "A top-level acceleration structure contains instances, not triangle geometries.");
        }
        if (description.instanceCount == 0)
        {
            return Fail(
                errorMessage,
                "A top-level acceleration structure requires at least one instance.");
        }
    }

    const bool preferFastTrace = HasAnyFlag(
        description.flags,
        AccelerationStructureBuildFlags::PreferFastTrace);
    const bool preferFastBuild = HasAnyFlag(
        description.flags,
        AccelerationStructureBuildFlags::PreferFastBuild);
    if (preferFastTrace && preferFastBuild)
    {
        return Fail(
            errorMessage,
            "PreferFastTrace and PreferFastBuild are mutually exclusive.");
    }
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }
    return true;
}

bool ValidateAccelerationStructureBuildRequest(
    const AccelerationStructureBuildRequest& request,
    const GraphicsApi graphicsApi,
    std::string* errorMessage)
{
    if (!ValidateAccelerationStructureBuildDescription(
            request.description,
            errorMessage))
    {
        return false;
    }
    if (request.description.type
        == AccelerationStructureType::BottomLevel)
    {
        if (request.geometries.size()
            != request.description.geometries.size())
        {
            return Fail(
                errorMessage,
                "BLAS geometry buffers must match the geometry descriptions.");
        }
        if (!request.instances.empty())
        {
            return Fail(
                errorMessage,
                "A BLAS build request cannot contain instances.");
        }
        for (std::size_t index = 0;
             index < request.geometries.size();
             ++index)
        {
            const RayTracingGeometryBuildInput& input =
                request.geometries[index];
            const RayTracingTrianglesDescription& expected =
                request.description.geometries[index];
            if (input.description.vertexCount
                    != expected.vertexCount
                || input.description.vertexStride
                    != expected.vertexStride
                || input.description.indexCount
                    != expected.indexCount
                || input.description.indexFormat
                    != expected.indexFormat)
            {
                return Fail(
                    errorMessage,
                    "BLAS geometry metadata does not match its prebuild description.");
            }
            if (input.vertexBuffer == nullptr
                || input.vertexBuffer->GetGraphicsApi()
                    != graphicsApi)
            {
                return Fail(
                    errorMessage,
                    "BLAS vertex buffers must belong to the active graphics API.");
            }
            if (!HasAnyFlag(
                    input.vertexBuffer
                        ->GetDescription()
                        .usage,
                    BufferUsage::
                        AccelerationStructureBuildInput))
            {
                return Fail(
                    errorMessage,
                    "BLAS vertex buffers require AccelerationStructureBuildInput usage.");
            }
            if (expected.indexCount > 0
                && (input.indexBuffer == nullptr
                    || input.indexBuffer
                               ->GetGraphicsApi()
                           != graphicsApi
                    || !HasAnyFlag(
                        input.indexBuffer
                            ->GetDescription()
                            .usage,
                        BufferUsage::
                            AccelerationStructureBuildInput)))
            {
                return Fail(
                    errorMessage,
                    "Indexed BLAS geometry requires an API-compatible acceleration-structure input buffer.");
            }
        }
    }
    else
    {
        if (!request.geometries.empty()
            || request.instances.size()
                != request.description.instanceCount)
        {
            return Fail(
                errorMessage,
                "TLAS instances must match the instance count in the build description.");
        }
        for (const RayTracingInstanceDescription& instance :
             request.instances)
        {
            if (instance.bottomLevel == nullptr
                || instance.bottomLevel
                           ->GetGraphicsApi()
                       != graphicsApi
                || instance.bottomLevel
                           ->GetDescription()
                           .type
                       != AccelerationStructureType::
                           BottomLevel)
            {
                return Fail(
                    errorMessage,
                    "TLAS instances require an API-compatible BLAS.");
            }
            if (instance.instanceId > 0x00ffffffu
                || instance.hitGroupOffset
                    > 0x00ffffffu)
            {
                return Fail(
                    errorMessage,
                    "TLAS instance IDs and hit-group offsets are limited to 24 bits.");
            }
        }
    }
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }
    return true;
}

} // namespace Prism::RHI
