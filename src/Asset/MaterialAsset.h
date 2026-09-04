#pragma once

#include <string>

#include <DirectXMath.h>

#include "Asset/AssetHandle.h"

namespace Prism::Asset
{
class MaterialAsset
{
public:
    void SetName(std::string name);
    void SetAlbedoColor(const DirectX::XMFLOAT4& albedoColor);
    void SetSpecularColor(const DirectX::XMFLOAT3& specularColor);
    void SetEmissiveColor(const DirectX::XMFLOAT3& emissiveColor);
    void SetMetallic(float metallic);
    void SetRoughness(float roughness);
    void SetShininess(float shininess);
    void SetNormalScale(float normalScale);
    void SetEmissiveStrength(float emissiveStrength);
    void SetAlphaCutoff(float alphaCutoff);
    void SetAlphaMode(float alphaMode);
    void SetDoubleSided(bool doubleSided);
    void SetUseAlbedoTexture(bool useAlbedoTexture);
    void SetUseMetallicRoughnessTexture(bool useMetallicRoughnessTexture);
    void SetUseNormalTexture(bool useNormalTexture);
    void SetUseOcclusionTexture(bool useOcclusionTexture);
    void SetUseEmissiveTexture(bool useEmissiveTexture);
    void SetOcclusionStrength(float occlusionStrength);
    void SetBaseColorTexture(TextureHandle baseColorTexture);
    void SetMetallicRoughnessTexture(TextureHandle metallicRoughnessTexture);
    void SetNormalTexture(TextureHandle normalTexture);
    void SetOcclusionTexture(TextureHandle occlusionTexture);
    void SetEmissiveTexture(TextureHandle emissiveTexture);

    const std::string& GetName() const;
    const DirectX::XMFLOAT4& GetAlbedoColor() const;
    const DirectX::XMFLOAT3& GetSpecularColor() const;
    const DirectX::XMFLOAT3& GetEmissiveColor() const;
    float GetMetallic() const;
    float GetRoughness() const;
    float GetShininess() const;
    float GetNormalScale() const;
    float GetEmissiveStrength() const;
    float GetAlphaCutoff() const;
    float GetAlphaMode() const;
    bool GetDoubleSided() const;
    bool GetUseAlbedoTexture() const;
    bool GetUseMetallicRoughnessTexture() const;
    bool GetUseNormalTexture() const;
    bool GetUseOcclusionTexture() const;
    bool GetUseEmissiveTexture() const;
    float GetOcclusionStrength() const;
    TextureHandle GetBaseColorTexture() const;
    TextureHandle GetMetallicRoughnessTexture() const;
    TextureHandle GetNormalTexture() const;
    TextureHandle GetOcclusionTexture() const;
    TextureHandle GetEmissiveTexture() const;

private:
    std::string m_name;
    DirectX::XMFLOAT4 m_albedoColor{1.0f, 1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT3 m_specularColor{1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT3 m_emissiveColor{0.0f, 0.0f, 0.0f};
    float m_metallic = 0.0f;
    float m_roughness = 0.5f;
    float m_shininess = 32.0f;
    float m_normalScale = 1.0f;
    float m_emissiveStrength = 1.0f;
    float m_alphaCutoff = 0.5f;
    float m_alphaMode = 0.0f;
    bool m_doubleSided = false;
    bool m_useAlbedoTexture = true;
    bool m_useMetallicRoughnessTexture = false;
    bool m_useNormalTexture = false;
    bool m_useOcclusionTexture = false;
    bool m_useEmissiveTexture = false;
    float m_occlusionStrength = 1.0f;
    TextureHandle m_baseColorTexture;
    TextureHandle m_metallicRoughnessTexture;
    TextureHandle m_normalTexture;
    TextureHandle m_occlusionTexture;
    TextureHandle m_emissiveTexture;
};
} // namespace Prism::Asset
