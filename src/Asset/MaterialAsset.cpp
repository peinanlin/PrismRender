#include "Asset/MaterialAsset.h"

#include <utility>

namespace Prism::Asset
{
void MaterialAsset::SetName(std::string name)
{
    m_name = std::move(name);
}

void MaterialAsset::SetAlbedoColor(const DirectX::XMFLOAT4& albedoColor)
{
    m_albedoColor = albedoColor;
}

void MaterialAsset::SetSpecularColor(const DirectX::XMFLOAT3& specularColor)
{
    m_specularColor = specularColor;
}

void MaterialAsset::SetEmissiveColor(const DirectX::XMFLOAT3& emissiveColor)
{
    m_emissiveColor = emissiveColor;
}

void MaterialAsset::SetMetallic(const float metallic)
{
    m_metallic = metallic;
}

void MaterialAsset::SetRoughness(const float roughness)
{
    m_roughness = roughness;
}

void MaterialAsset::SetShininess(const float shininess)
{
    m_shininess = shininess;
}

void MaterialAsset::SetNormalScale(const float normalScale)
{
    m_normalScale = normalScale;
}

void MaterialAsset::SetEmissiveStrength(const float emissiveStrength)
{
    m_emissiveStrength = emissiveStrength;
}

void MaterialAsset::SetAlphaCutoff(const float alphaCutoff)
{
    m_alphaCutoff = alphaCutoff;
}

void MaterialAsset::SetAlphaMode(const float alphaMode)
{
    m_alphaMode = alphaMode;
}

void MaterialAsset::SetDoubleSided(const bool doubleSided)
{
    m_doubleSided = doubleSided;
}

void MaterialAsset::SetUseAlbedoTexture(const bool useAlbedoTexture)
{
    m_useAlbedoTexture = useAlbedoTexture;
}

void MaterialAsset::SetUseMetallicRoughnessTexture(const bool useMetallicRoughnessTexture)
{
    m_useMetallicRoughnessTexture = useMetallicRoughnessTexture;
}

void MaterialAsset::SetUseNormalTexture(const bool useNormalTexture)
{
    m_useNormalTexture = useNormalTexture;
}

void MaterialAsset::SetUseOcclusionTexture(const bool useOcclusionTexture)
{
    m_useOcclusionTexture = useOcclusionTexture;
}

void MaterialAsset::SetUseEmissiveTexture(const bool useEmissiveTexture)
{
    m_useEmissiveTexture = useEmissiveTexture;
}

void MaterialAsset::SetOcclusionStrength(const float occlusionStrength)
{
    m_occlusionStrength = occlusionStrength;
}

void MaterialAsset::SetBaseColorTexture(const TextureHandle baseColorTexture)
{
    m_baseColorTexture = baseColorTexture;
}

void MaterialAsset::SetMetallicRoughnessTexture(const TextureHandle metallicRoughnessTexture)
{
    m_metallicRoughnessTexture = metallicRoughnessTexture;
}

void MaterialAsset::SetNormalTexture(const TextureHandle normalTexture)
{
    m_normalTexture = normalTexture;
}

void MaterialAsset::SetOcclusionTexture(const TextureHandle occlusionTexture)
{
    m_occlusionTexture = occlusionTexture;
}

void MaterialAsset::SetEmissiveTexture(const TextureHandle emissiveTexture)
{
    m_emissiveTexture = emissiveTexture;
}

const std::string& MaterialAsset::GetName() const
{
    return m_name;
}

const DirectX::XMFLOAT4& MaterialAsset::GetAlbedoColor() const
{
    return m_albedoColor;
}

const DirectX::XMFLOAT3& MaterialAsset::GetSpecularColor() const
{
    return m_specularColor;
}

const DirectX::XMFLOAT3& MaterialAsset::GetEmissiveColor() const
{
    return m_emissiveColor;
}

float MaterialAsset::GetMetallic() const
{
    return m_metallic;
}

float MaterialAsset::GetRoughness() const
{
    return m_roughness;
}

float MaterialAsset::GetShininess() const
{
    return m_shininess;
}

float MaterialAsset::GetNormalScale() const
{
    return m_normalScale;
}

float MaterialAsset::GetEmissiveStrength() const
{
    return m_emissiveStrength;
}

float MaterialAsset::GetAlphaCutoff() const
{
    return m_alphaCutoff;
}

float MaterialAsset::GetAlphaMode() const
{
    return m_alphaMode;
}

bool MaterialAsset::GetDoubleSided() const
{
    return m_doubleSided;
}

bool MaterialAsset::GetUseAlbedoTexture() const
{
    return m_useAlbedoTexture;
}

bool MaterialAsset::GetUseMetallicRoughnessTexture() const
{
    return m_useMetallicRoughnessTexture;
}

bool MaterialAsset::GetUseNormalTexture() const
{
    return m_useNormalTexture;
}

bool MaterialAsset::GetUseOcclusionTexture() const
{
    return m_useOcclusionTexture;
}

bool MaterialAsset::GetUseEmissiveTexture() const
{
    return m_useEmissiveTexture;
}

float MaterialAsset::GetOcclusionStrength() const
{
    return m_occlusionStrength;
}

TextureHandle MaterialAsset::GetBaseColorTexture() const
{
    return m_baseColorTexture;
}

TextureHandle MaterialAsset::GetMetallicRoughnessTexture() const
{
    return m_metallicRoughnessTexture;
}

TextureHandle MaterialAsset::GetNormalTexture() const
{
    return m_normalTexture;
}

TextureHandle MaterialAsset::GetOcclusionTexture() const
{
    return m_occlusionTexture;
}

TextureHandle MaterialAsset::GetEmissiveTexture() const
{
    return m_emissiveTexture;
}
} // namespace Prism::Asset
