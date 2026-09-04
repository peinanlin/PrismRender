#include "Asset/Material.h"

#include "Asset/Texture.h"
#include "Core/Assert.h"

namespace Prism::Asset
{
void Material::Initialize(
    Parameters parameters,
    std::shared_ptr<Texture> albedoTexture,
    std::shared_ptr<Texture> metallicRoughnessTexture,
    std::shared_ptr<Texture> normalTexture,
    std::shared_ptr<Texture> occlusionTexture,
    std::shared_ptr<Texture> emissiveTexture)
{
    Core::Check(
        albedoTexture != nullptr && metallicRoughnessTexture != nullptr && normalTexture != nullptr
            && occlusionTexture != nullptr && emissiveTexture != nullptr,
        "RHI material initialization requires all five fallback-resolved textures.");
    m_parameters = std::move(parameters);
    m_albedoTexture = std::move(albedoTexture);
    m_metallicRoughnessTexture = std::move(metallicRoughnessTexture);
    m_normalTexture = std::move(normalTexture);
    m_occlusionTexture = std::move(occlusionTexture);
    m_emissiveTexture = std::move(emissiveTexture);
}

const Material::Parameters& Material::GetParameters() const
{
    return m_parameters;
}

const std::shared_ptr<Texture>& Material::GetAlbedoTexture() const
{
    return m_albedoTexture;
}

const std::shared_ptr<Texture>& Material::GetMetallicRoughnessTexture() const
{
    return m_metallicRoughnessTexture;
}

const std::shared_ptr<Texture>& Material::GetNormalTexture() const
{
    return m_normalTexture;
}

const std::shared_ptr<Texture>& Material::GetOcclusionTexture() const
{
    return m_occlusionTexture;
}

const std::shared_ptr<Texture>& Material::GetEmissiveTexture() const
{
    return m_emissiveTexture;
}

} // namespace Prism::Asset
