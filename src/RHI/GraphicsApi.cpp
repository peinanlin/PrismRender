#include "RHI/GraphicsApi.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace Prism::RHI
{
std::string_view ToString(const GraphicsApi api)
{
    switch (api)
    {
    case GraphicsApi::Direct3D12: return "Direct3D 12";
    case GraphicsApi::Direct3D11: return "Direct3D 11";
    case GraphicsApi::Vulkan: return "Vulkan";
    }
    return "Unknown";
}

std::optional<GraphicsApi> TryParseGraphicsApi(const std::string_view name)
{
    std::string normalized(name);
    std::ranges::transform(normalized, normalized.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });

    if (normalized == "d3d12" || normalized == "dx12" || normalized == "direct3d12")
    {
        return GraphicsApi::Direct3D12;
    }
    if (normalized == "d3d11" || normalized == "dx11" || normalized == "direct3d11")
    {
        return GraphicsApi::Direct3D11;
    }
    if (normalized == "vulkan" || normalized == "vk")
    {
        return GraphicsApi::Vulkan;
    }
    return std::nullopt;
}
} // namespace Prism::RHI
