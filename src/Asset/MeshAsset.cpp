#include "Asset/MeshAsset.h"

#include "Asset/Mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace Prism::Asset
{
std::shared_ptr<MeshAsset> MeshAsset::CreateCube()
{
    constexpr std::array<MeshVertex, 24> vertices = {
        MeshVertex{{-1.0f, -1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        MeshVertex{{1.0f, -1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        MeshVertex{{1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        MeshVertex{{-1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},

        MeshVertex{{1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}},
        MeshVertex{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}},
        MeshVertex{{-1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}},
        MeshVertex{{1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}},

        MeshVertex{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},
        MeshVertex{{-1.0f, -1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},
        MeshVertex{{-1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},
        MeshVertex{{-1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},

        MeshVertex{{1.0f, -1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, -1.0f, 1.0f}},
        MeshVertex{{1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, -1.0f, 1.0f}},
        MeshVertex{{1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, -1.0f, 1.0f}},
        MeshVertex{{1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, -1.0f, 1.0f}},

        MeshVertex{{-1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, -1.0f}},
        MeshVertex{{1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, -1.0f}},
        MeshVertex{{1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, -1.0f}},
        MeshVertex{{-1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, -1.0f}},

        MeshVertex{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        MeshVertex{{1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        MeshVertex{{1.0f, -1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        MeshVertex{{-1.0f, -1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
    };

    constexpr std::array<std::uint16_t, 36> indices = {
        0, 1, 2, 0, 2, 3,
        4, 5, 6, 4, 6, 7,
        8, 9, 10, 8, 10, 11,
        12, 13, 14, 12, 14, 15,
        16, 17, 18, 16, 18, 19,
        20, 21, 22, 20, 22, 23};

    std::shared_ptr<MeshAsset> meshAsset = std::make_shared<MeshAsset>();
    meshAsset->SetName("BuiltinCube");
    meshAsset->m_vertices.assign(vertices.begin(), vertices.end());
    meshAsset->m_indices.assign(indices.begin(), indices.end());
    meshAsset->m_bounds = {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, std::sqrt(3.0f)};
    return meshAsset;
}

std::shared_ptr<MeshAsset> MeshAsset::CreateUvSphere(const std::uint32_t latitudeSegments, const std::uint32_t longitudeSegments)
{
    const std::uint32_t clampedLatitudeSegments = std::max(4u, latitudeSegments);
    const std::uint32_t clampedLongitudeSegments = std::max(8u, longitudeSegments);
    const std::uint32_t vertexRowSize = clampedLongitudeSegments + 1u;
    const std::uint32_t vertexCount = (clampedLatitudeSegments + 1u) * vertexRowSize;
    if (vertexCount > static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max()))
    {
        return CreateCube();
    }

    constexpr float Pi = 3.14159265358979323846f;
    std::vector<MeshVertex> vertices;
    vertices.reserve(vertexCount);
    for (std::uint32_t latitude = 0; latitude <= clampedLatitudeSegments; ++latitude)
    {
        const float v = static_cast<float>(latitude) / static_cast<float>(clampedLatitudeSegments);
        const float theta = v * Pi;
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);

        for (std::uint32_t longitude = 0; longitude <= clampedLongitudeSegments; ++longitude)
        {
            const float u = static_cast<float>(longitude) / static_cast<float>(clampedLongitudeSegments);
            const float phi = u * Pi * 2.0f;
            const float sinPhi = std::sin(phi);
            const float cosPhi = std::cos(phi);

            const DirectX::XMFLOAT3 normal{sinTheta * sinPhi, cosTheta, sinTheta * cosPhi};
            MeshVertex vertex{};
            vertex.position = normal;
            vertex.normal = normal;
            vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
            vertex.texCoord = {u, v};
            vertex.tangent = {cosPhi, 0.0f, -sinPhi, 1.0f};
            vertices.push_back(vertex);
        }
    }

    std::vector<std::uint16_t> indices;
    indices.reserve(static_cast<std::size_t>(clampedLatitudeSegments) * clampedLongitudeSegments * 12u);
    for (std::uint32_t latitude = 0; latitude < clampedLatitudeSegments; ++latitude)
    {
        for (std::uint32_t longitude = 0; longitude < clampedLongitudeSegments; ++longitude)
        {
            const std::uint16_t first = static_cast<std::uint16_t>(latitude * vertexRowSize + longitude);
            const std::uint16_t second = static_cast<std::uint16_t>(first + vertexRowSize);
            const std::uint16_t firstNext = static_cast<std::uint16_t>(first + 1u);
            const std::uint16_t secondNext = static_cast<std::uint16_t>(second + 1u);

            indices.push_back(first);
            indices.push_back(second);
            indices.push_back(firstNext);
            indices.push_back(firstNext);
            indices.push_back(second);
            indices.push_back(secondNext);

            indices.push_back(firstNext);
            indices.push_back(second);
            indices.push_back(first);
            indices.push_back(secondNext);
            indices.push_back(second);
            indices.push_back(firstNext);
        }
    }

    std::shared_ptr<MeshAsset> meshAsset = std::make_shared<MeshAsset>();
    meshAsset->SetName("BuiltinUvSphere");
    meshAsset->SetGeometry(std::move(vertices), std::move(indices), {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f});
    return meshAsset;
}

void MeshAsset::SetName(std::string name)
{
    m_name = std::move(name);
}

void MeshAsset::SetGeometry(std::vector<MeshVertex> vertices, std::vector<std::uint16_t> indices, const MeshBounds& bounds)
{
    m_vertices = std::move(vertices);
    m_indices = std::move(indices);
    m_bounds = bounds;
}

const std::string& MeshAsset::GetName() const
{
    return m_name;
}

const std::vector<MeshVertex>& MeshAsset::GetVertices() const
{
    return m_vertices;
}

const std::vector<std::uint16_t>& MeshAsset::GetIndices() const
{
    return m_indices;
}

const MeshBounds& MeshAsset::GetBounds() const
{
    return m_bounds;
}
} // namespace Prism::Asset
