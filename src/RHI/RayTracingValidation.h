#pragma once

#include "RHI/RayTracing.h"

#include <filesystem>
#include <string>

namespace Prism::RHI
{
class IGraphicsDevice;

struct RayTracingValidationResult
{
    bool supported = false;
    bool passed = false;
    RayTracingTier tier =
        RayTracingTier::Unsupported;
    AccelerationStructureBuildSizes
        bottomLevelSizes;
    AccelerationStructureBuildSizes
        topLevelSizes;
    bool bottomLevelBuilt = false;
    bool topLevelBuilt = false;
    std::uint64_t bottomLevelDeviceAddress = 0;
    std::uint64_t topLevelDeviceAddress = 0;
    std::string message;
};

RayTracingValidationResult
RunRayTracingValidation(
    IGraphicsDevice& device);

bool WriteRayTracingValidationReport(
    const std::filesystem::path& outputPath,
    IGraphicsDevice& device,
    std::string* errorMessage = nullptr);
} // namespace Prism::RHI
