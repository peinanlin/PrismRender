#pragma once

#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <vector>

namespace Prism::Renderer
{
enum class OceanPatchEdge : std::uint8_t
{
    None = 0u,
    North = 1u << 0u,
    East = 1u << 1u,
    South = 1u << 2u,
    West = 1u << 3u
};
constexpr OceanPatchEdge operator|(OceanPatchEdge lhs, OceanPatchEdge rhs)
{
    return static_cast<OceanPatchEdge>(
        static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}
struct OceanPatchGeometry
{
    std::uint32_t cellCount = 0u;
    OceanPatchEdge edgeMask = OceanPatchEdge::None;
    std::vector<DirectX::XMFLOAT2> vertices;
    std::vector<std::uint32_t> indices;
};
struct OceanQuadtreeSettings
{
    float minimumPatchSizeMeters = 16.0f;
    float maximumScreenEdgePixels = 96.0f;
    float maximumViewDistanceMeters = 4000.0f;
    float conservativeDisplacementMeters = 32.0f;
    std::uint32_t maximumLod = 8u;
    // Standalone selector fixtures measure one edge segment by default;
    // runtime adaptive rendering supplies the configured patch cell count.
    std::uint32_t cellsPerPatch = 1u;
    std::uint32_t viewportHeightPixels = 1080u;
    float verticalFieldOfViewRadians = 1.0471975512f;
    float meanSeaLevelMeters = 0.0f;
    // A CPU selector must remain bounded even when an editor camera uses an
    // effectively infinite far plane or a very small screen-edge target.
    std::uint32_t maximumNodeCount = 4096u;
};
struct OceanQuadtreeNode
{
    DirectX::XMFLOAT2 center{};
    float halfExtent = 0.0f;
    std::uint32_t lod = 0u;
    float morph = 0.0f;
    OceanPatchEdge edgeMask = OceanPatchEdge::None;
};
struct OceanQuadtreeFrustum
{
    // Plane equations use Ax + By + Cz + D >= 0 for the visible half-space.
    std::array<DirectX::XMFLOAT4, 6> planes{};
    bool enabled = false;
};
struct OceanQuadtreeSelection
{
    std::vector<OceanQuadtreeNode> nodes;
    std::uint32_t refinementIterations = 0u;
};
// Compact per-instance payload consumed by the adaptive ocean vertex path.
// Keeping this format renderer-owned lets D3D12/Vulkan upload the same data
// without baking API-specific descriptors into the quadtree selector.
struct OceanPatchInstanceData
{
    DirectX::XMFLOAT4 centerHalfExtent{}; // x/z center, half extent in y
    DirectX::XMFLOAT4 morphEdgeLod{};     // morph, edge mask, lod, reserved
};
class OceanQuadtree
{
public:
    [[nodiscard]] static OceanPatchGeometry BuildPatchGeometry(
        std::uint32_t cellCount, OceanPatchEdge edgeMask);
    [[nodiscard]] static OceanQuadtreeSelection Select(
        const OceanQuadtreeSettings& settings,
        DirectX::XMFLOAT3 cameraPosition,
        float cameraFarPlane);
    [[nodiscard]] static OceanQuadtreeSelection Select(
        const OceanQuadtreeSettings& settings,
        DirectX::XMFLOAT3 cameraPosition,
        float cameraFarPlane,
        const OceanQuadtreeFrustum& frustum);
    [[nodiscard]] static std::vector<OceanQuadtreeNode> Balance(
        std::vector<OceanQuadtreeNode> nodes,
        std::size_t maximumNodeCount = static_cast<std::size_t>(-1));
    [[nodiscard]] static std::vector<OceanQuadtreeNode> GroupForInstances(
        const OceanQuadtreeSelection& selection);
    [[nodiscard]] static std::vector<OceanPatchInstanceData> BuildInstanceData(
        const OceanQuadtreeSelection& selection);
};
} // namespace Prism::Renderer
