#pragma once

#include <cstdint>
#include <string>

namespace Prism::RHI
{
struct GraphicsAdapterInfo
{
    std::string name;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint64_t dedicatedVideoMemoryBytes = 0;
    std::uint64_t sharedSystemMemoryBytes = 0;
    std::uint64_t driverVersionRaw = 0;
    std::string driverVersion;
    std::string apiVersion;
};
} // namespace Prism::RHI
