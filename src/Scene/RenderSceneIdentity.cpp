#include "Scene/RenderSceneIdentity.h"

#include <functional>

namespace Prism::Scene
{
std::size_t RenderObjectIdHash::operator()(
    const RenderObjectId& id) const noexcept
{
    std::size_t result = std::hash<std::uint64_t>{}(id.high);
    result ^= std::hash<std::uint64_t>{}(id.low)
        + 0x9e3779b97f4a7c15ull
        + (result << 6u)
        + (result >> 2u);
    result ^= static_cast<std::size_t>(id.domain)
        + 0x9e3779b97f4a7c15ull
        + (result << 6u)
        + (result >> 2u);
    return result;
}
} // namespace Prism::Scene
