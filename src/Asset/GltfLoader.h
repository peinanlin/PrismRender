#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <DirectXMath.h>

namespace Prism::Asset
{
class MaterialAsset;
class MeshAsset;
class TextureAsset;

class GltfLoader
{
public:
    struct MeshInstanceRecord
    {
        std::string name;
        int nodeIndex = -1;
        int meshIndex = -1;
        std::uint32_t primitiveIndex = 0;
        int materialIndex = -1;
        std::shared_ptr<MeshAsset> mesh;
        std::shared_ptr<MaterialAsset> material;
        std::shared_ptr<TextureAsset> baseColorTexture;
        std::shared_ptr<TextureAsset> metallicRoughnessTexture;
        std::shared_ptr<TextureAsset> normalTexture;
        std::shared_ptr<TextureAsset> occlusionTexture;
        std::shared_ptr<TextureAsset> emissiveTexture;
        DirectX::XMFLOAT4X4 worldMatrix{};
    };

    struct SceneData
    {
        std::vector<MeshInstanceRecord> instances;
    };

    bool LoadScene(const std::string& path, SceneData& outScene, std::string* outErrorMessage = nullptr) const;
};
} // namespace Prism::Asset
