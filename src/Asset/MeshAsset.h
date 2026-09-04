#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Asset/Mesh.h"

namespace Prism::Asset
{
struct MeshBounds
{
    DirectX::XMFLOAT3 min{-1.0f, -1.0f, -1.0f};
    DirectX::XMFLOAT3 max{1.0f, 1.0f, 1.0f};
    float radius = 1.0f;
};

class MeshAsset
{
public:
    static std::shared_ptr<MeshAsset> CreateCube();
    static std::shared_ptr<MeshAsset> CreateUvSphere(std::uint32_t latitudeSegments = 16, std::uint32_t longitudeSegments = 32);

    void SetName(std::string name);
    void SetGeometry(std::vector<MeshVertex> vertices, std::vector<std::uint16_t> indices, const MeshBounds& bounds);

    const std::string& GetName() const;
    const std::vector<MeshVertex>& GetVertices() const;
    const std::vector<std::uint16_t>& GetIndices() const;
    const MeshBounds& GetBounds() const;

private:
    std::string m_name;
    std::vector<MeshVertex> m_vertices;
    std::vector<std::uint16_t> m_indices;
    MeshBounds m_bounds;
};
} // namespace Prism::Asset
