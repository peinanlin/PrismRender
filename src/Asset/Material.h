#pragma once

#include <cstdint>
#include <memory>

#include <DirectXMath.h>

namespace Prism::Asset
{
class Texture;
}

namespace Prism::Asset
{
class Material
{
public:
    struct Parameters
    {
        DirectX::XMFLOAT4 albedoColor{1.0f, 1.0f, 1.0f, 1.0f};
        DirectX::XMFLOAT3 specularColor{1.0f, 1.0f, 1.0f};
        DirectX::XMFLOAT3 emissiveColor{0.0f, 0.0f, 0.0f};
        float metallic = 0.0f;
        float roughness = 0.5f;
        float shininess = 32.0f;
        float occlusionStrength = 1.0f;
        float normalScale = 1.0f;
        float emissiveStrength = 1.0f;
        float alphaCutoff = 0.5f;
        float alphaMode = 0.0f;
        bool doubleSided = false;
        bool useAlbedoTexture = true;
        bool useMetallicRoughnessTexture = false;
        bool useNormalTexture = false;
        bool useOcclusionTexture = false;
        bool useEmissiveTexture = false;
    };

    void Initialize(
        Parameters parameters,
        std::shared_ptr<Texture> albedoTexture,
        std::shared_ptr<Texture> metallicRoughnessTexture,
        std::shared_ptr<Texture> normalTexture,
        std::shared_ptr<Texture> occlusionTexture,
        std::shared_ptr<Texture> emissiveTexture);

    const Parameters& GetParameters() const;
    const std::shared_ptr<Texture>& GetAlbedoTexture() const;
    const std::shared_ptr<Texture>& GetMetallicRoughnessTexture() const;
    const std::shared_ptr<Texture>& GetNormalTexture() const;
    const std::shared_ptr<Texture>& GetOcclusionTexture() const;
    const std::shared_ptr<Texture>& GetEmissiveTexture() const;
private:
    Parameters m_parameters{};
    std::shared_ptr<Texture> m_albedoTexture;
    std::shared_ptr<Texture> m_metallicRoughnessTexture;
    std::shared_ptr<Texture> m_normalTexture;
    std::shared_ptr<Texture> m_occlusionTexture;
    std::shared_ptr<Texture> m_emissiveTexture;
};
} // namespace Prism::Asset
