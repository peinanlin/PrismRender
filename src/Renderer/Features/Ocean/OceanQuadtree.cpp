#include "Renderer/Features/Ocean/OceanQuadtree.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

namespace Prism::Renderer
{
OceanPatchGeometry OceanQuadtree::BuildPatchGeometry(
    const std::uint32_t cellCount, const OceanPatchEdge edgeMask)
{
    const std::uint32_t cells = std::clamp(cellCount, 1u, 256u);
    OceanPatchGeometry geometry{cells, edgeMask};
    geometry.vertices.reserve(
        static_cast<std::size_t>(cells + 1u) * (cells + 1u));
    for (std::uint32_t z = 0u; z <= cells; ++z)
        for (std::uint32_t x = 0u; x <= cells; ++x)
            geometry.vertices.push_back(
                {static_cast<float>(x) / cells, static_cast<float>(z) / cells});
    const auto index = [cells](std::uint32_t x, std::uint32_t z)
    { return z * (cells + 1u) + x; };
    const auto remap = [cells, edgeMask](std::uint32_t x, std::uint32_t z)
    {
        // A coarse patch adjacent to a one-level-finer neighbor collapses
        // every other boundary vertex.  This finite variant keeps the
        // shared edge on the same sample positions without adding a CPU-side
        // stitch buffer or relying on primitive restart.
        const auto has = [edgeMask](const OceanPatchEdge edge)
        {
            return (static_cast<std::uint8_t>(edgeMask) &
                       static_cast<std::uint8_t>(edge)) != 0u;
        };
        if (has(OceanPatchEdge::North) && z == 0u && (x & 1u) != 0u)
            --x;
        if (has(OceanPatchEdge::South) && z == cells && (x & 1u) != 0u)
            --x;
        if (has(OceanPatchEdge::West) && x == 0u && (z & 1u) != 0u)
            --z;
        if (has(OceanPatchEdge::East) && x == cells && (z & 1u) != 0u)
            --z;
        return std::pair{x, z};
    };
    geometry.indices.reserve(static_cast<std::size_t>(cells) * cells * 6u);
    for (std::uint32_t z = 0u; z < cells; ++z)
        for (std::uint32_t x = 0u; x < cells; ++x)
        {
            const auto [ax, az] = remap(x, z);
            const auto [bx, bz] = remap(x + 1u, z);
            const auto [cx, cz] = remap(x, z + 1u);
            const auto [dx, dz] = remap(x + 1u, z + 1u);
            const std::uint32_t a = index(ax, az), b = index(bx, bz);
            const std::uint32_t c = index(cx, cz), d = index(dx, dz);
            const auto appendTriangle = [&geometry](const std::uint32_t first,
                                            const std::uint32_t second,
                                            const std::uint32_t third,
                                            const std::uint32_t fallbackFirst,
                                            const std::uint32_t fallbackSecond,
                                            const std::uint32_t fallbackThird)
            {
                if (first != second && first != third && second != third)
                    geometry.indices.insert(
                        geometry.indices.end(), {first, second, third});
                else if (fallbackFirst != fallbackSecond &&
                         fallbackFirst != fallbackThird &&
                         fallbackSecond != fallbackThird)
                    geometry.indices.insert(geometry.indices.end(),
                        {fallbackFirst, fallbackSecond, fallbackThird});
                else
                    geometry.indices.insert(
                        geometry.indices.end(), {0u, 1u, 2u});
            };
            appendTriangle(a, c, b, a, b, d);
            appendTriangle(b, c, d, a, c, d);
        }
    return geometry;
}

OceanQuadtreeSelection OceanQuadtree::Select(
    const OceanQuadtreeSettings& settings,
    const DirectX::XMFLOAT3 cameraPosition,
    const float cameraFarPlane)
{
    return Select(settings, cameraPosition, cameraFarPlane, {});
}

OceanQuadtreeSelection OceanQuadtree::Select(
    const OceanQuadtreeSettings& settings,
    const DirectX::XMFLOAT3 cameraPosition,
    const float cameraFarPlane,
    const OceanQuadtreeFrustum& frustum)
{
    OceanQuadtreeSelection selection{};
    const float farPlane = std::min(std::max(cameraFarPlane, 1.0f),
        std::max(settings.maximumViewDistanceMeters, 1.0f));
    const float rootHalf = std::max(settings.minimumPatchSizeMeters,
        farPlane + settings.conservativeDisplacementMeters);
    // Follow the camera on a stable grid. The root covers the complete view
    // radius in every horizontal direction; snapping avoids rebuilding
    // an unrelated tree for sub-patch camera jitter.
    const float rootSnap = std::max(rootHalf * 0.25f,
        settings.minimumPatchSizeMeters);
    const DirectX::XMFLOAT2 rootCenter{
        std::floor(cameraPosition.x / rootSnap) * rootSnap,
        std::floor(cameraPosition.z / rootSnap) * rootSnap};
    const std::size_t maximumNodeCount =
        std::max<std::size_t>(settings.maximumNodeCount, 1u);
    std::vector<OceanQuadtreeNode> pending{
        {rootCenter, rootHalf, 0u, 0.0f, OceanPatchEdge::None}};
    while (!pending.empty())
    {
        const OceanQuadtreeNode node = pending.back();
        pending.pop_back();
        const float dx = node.center.x - cameraPosition.x;
        const float dz = node.center.y - cameraPosition.z;
        const float dy = settings.meanSeaLevelMeters - cameraPosition.y;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance - node.halfExtent > farPlane)
            continue;
        if (frustum.enabled)
        {
            bool outside = false;
            const float radius = node.halfExtent * 1.41421356237f +
                                 settings.conservativeDisplacementMeters;
            for (const DirectX::XMFLOAT4& plane : frustum.planes)
            {
                const float distanceToPlane = plane.x * node.center.x +
                                              plane.y * 0.0f +
                                              plane.z * node.center.y + plane.w;
                if (distanceToPlane < -radius)
                {
                    outside = true;
                    break;
                }
            }
            if (outside)
                continue;
        }
        const float focalLengthPixels =
            0.5f * std::max(settings.viewportHeightPixels, 1u)
            / std::tan(std::clamp(settings.verticalFieldOfViewRadians,
                           0.05f, 3.0f)
                       * 0.5f);
        // maximumScreenEdgePixels applies to one grid cell, not to an entire
        // patch. A 64-cell node may therefore be much wider than ten pixels.
        const float targetEdge = std::max(settings.minimumPatchSizeMeters,
            settings.maximumScreenEdgePixels * std::max(distance, 1.0f)
                * std::max(settings.cellsPerPatch, 1u)
                / std::max(focalLengthPixels, 1.0f));
        const bool canSplitWithinBudget =
            pending.size() + selection.nodes.size() + 4u <= maximumNodeCount;
        if (node.lod < settings.maximumLod &&
            node.halfExtent * 2.0f > targetEdge && canSplitWithinBudget)
        {
            ++selection.refinementIterations;
            const float childHalf = node.halfExtent * 0.5f;
            for (int z = -1; z <= 1; z += 2)
                for (int x = -1; x <= 1; x += 2)
                    pending.push_back({{node.center.x + x * childHalf,
                                           node.center.y + z * childHalf},
                        childHalf,
                        node.lod + 1u,
                        0.0f,
                        OceanPatchEdge::None});
        }
        else
        {
            OceanQuadtreeNode leaf = node;
            leaf.morph =
                std::clamp((node.halfExtent * 2.0f - targetEdge) /
                               std::max(node.halfExtent * 2.0f, 1.0e-3f),
                    0.0f,
                    1.0f);
            selection.nodes.push_back(leaf);
        }
    }
    selection.nodes = Balance(
        std::move(selection.nodes), maximumNodeCount);
    // Mark the finite edge variants after balancing.  A node gets an edge bit
    // when the adjacent leaf is coarser, so the patch builder can collapse
    // every other boundary sample and avoid T-junctions.
    for (std::size_t i = 0u; i < selection.nodes.size(); ++i)
    {
        auto& node = selection.nodes[i];
        const float left = node.center.x - node.halfExtent;
        const float right = node.center.x + node.halfExtent;
        const float top = node.center.y - node.halfExtent;
        const float bottom = node.center.y + node.halfExtent;
        for (std::size_t j = 0u; j < selection.nodes.size(); ++j)
        {
            if (i == j || selection.nodes[j].lod >= node.lod)
                continue;
            const auto& other = selection.nodes[j];
            const float otherLeft = other.center.x - other.halfExtent;
            const float otherRight = other.center.x + other.halfExtent;
            const float otherTop = other.center.y - other.halfExtent;
            const float otherBottom = other.center.y + other.halfExtent;
            constexpr float Epsilon = 1.0e-3f;
            const bool verticalOverlap =
                otherBottom > top + Epsilon && otherTop < bottom - Epsilon;
            const bool horizontalOverlap =
                otherRight > left + Epsilon && otherLeft < right - Epsilon;
            if (verticalOverlap && std::abs(left - otherRight) <= Epsilon)
                node.edgeMask = node.edgeMask | OceanPatchEdge::West;
            if (verticalOverlap && std::abs(right - otherLeft) <= Epsilon)
                node.edgeMask = node.edgeMask | OceanPatchEdge::East;
            if (horizontalOverlap && std::abs(top - otherBottom) <= Epsilon)
                node.edgeMask = node.edgeMask | OceanPatchEdge::North;
            if (horizontalOverlap && std::abs(bottom - otherTop) <= Epsilon)
                node.edgeMask = node.edgeMask | OceanPatchEdge::South;
        }
    }
    return selection;
}

std::vector<OceanQuadtreeNode> OceanQuadtree::Balance(
    std::vector<OceanQuadtreeNode> nodes,
    const std::size_t maximumNodeCount)
{
    // Refine the coarse leaf itself. Merely changing its LOD label would leave
    // its physical extent unchanged and produce a false "balanced" result.
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (std::size_t i = 0u; i < nodes.size() && !changed; ++i)
            for (std::size_t j = i + 1u; j < nodes.size(); ++j)
            {
                const float dx =
                    std::abs(nodes[i].center.x - nodes[j].center.x);
                const float dz =
                    std::abs(nodes[i].center.y - nodes[j].center.y);
                const float xGap =
                    dx - (nodes[i].halfExtent + nodes[j].halfExtent);
                const float zGap =
                    dz - (nodes[i].halfExtent + nodes[j].halfExtent);
                constexpr float Epsilon = 1.0e-3f;
                const bool edgeAdjacent =
                    xGap <= Epsilon && zGap <= Epsilon &&
                    (std::abs(xGap) <= Epsilon || std::abs(zGap) <= Epsilon);
                if (!edgeAdjacent)
                    continue;
                const auto delta = std::abs(static_cast<int>(nodes[i].lod) -
                                            static_cast<int>(nodes[j].lod));
                if (delta <= 1u)
                    continue;
                if (nodes.size() + 3u > maximumNodeCount)
                    return nodes;
                const std::size_t coarseIndex =
                    nodes[i].lod < nodes[j].lod ? i : j;
                const OceanQuadtreeNode coarse = nodes[coarseIndex];
                nodes.erase(nodes.begin()
                    + static_cast<std::ptrdiff_t>(coarseIndex));
                const float childHalf = coarse.halfExtent * 0.5f;
                for (int z = -1; z <= 1; z += 2)
                    for (int x = -1; x <= 1; x += 2)
                        nodes.push_back({{coarse.center.x + x * childHalf,
                                             coarse.center.y + z * childHalf},
                            childHalf, coarse.lod + 1u, coarse.morph,
                            OceanPatchEdge::None});
                changed = true;
                break;
            }
    }
    return nodes;
}

std::vector<OceanQuadtreeNode> OceanQuadtree::GroupForInstances(
    const OceanQuadtreeSelection& selection)
{
    auto grouped = selection.nodes;
    std::ranges::sort(grouped,
        [](const OceanQuadtreeNode& a, const OceanQuadtreeNode& b)
        {
            const auto aMask = static_cast<std::uint8_t>(a.edgeMask);
            const auto bMask = static_cast<std::uint8_t>(b.edgeMask);
            return aMask != bMask ? aMask < bMask : a.lod < b.lod;
        });
    return grouped;
}

std::vector<OceanPatchInstanceData> OceanQuadtree::BuildInstanceData(
    const OceanQuadtreeSelection& selection)
{
    std::vector<OceanPatchInstanceData> data;
    data.reserve(selection.nodes.size());
    for (const OceanQuadtreeNode& node : GroupForInstances(selection))
    {
        data.push_back({{node.center.x,
                            node.center.y,
                            node.halfExtent,
                            static_cast<float>(node.lod)},
            {node.morph,
                static_cast<float>(static_cast<std::uint8_t>(node.edgeMask)),
                static_cast<float>(node.lod),
                0.0f}});
    }
    return data;
}
} // namespace Prism::Renderer
