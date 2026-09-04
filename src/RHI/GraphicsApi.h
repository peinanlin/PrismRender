#pragma once

#include <optional>
#include <string_view>

namespace Prism::RHI
{
enum class GraphicsApi
{
    Direct3D12,
    Direct3D11,
    Vulkan
};

std::string_view ToString(GraphicsApi api);
std::optional<GraphicsApi> TryParseGraphicsApi(std::string_view name);
} // namespace Prism::RHI
