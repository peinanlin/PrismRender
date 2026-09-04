#include "Renderer/MaterialParameterResolver.h"

#include "Asset/Material.h"
#include "Scene/RenderObject.h"
#include "Scene/RenderSceneView.h"

#include <bit>

namespace Prism::Renderer
{
std::uint64_t ResolveMaterialOverrideSignature(
    const Scene::RenderObject& renderObject)
{
    if (!renderObject.hasMaterialOverride)
    {
        return 0;
    }

    const Scene::MaterialParameterOverride& material =
        renderObject.materialOverride;
    std::uint64_t hash = 14695981039346656037ull;
    const auto appendFloat = [&](const float value)
    {
        hash ^= std::bit_cast<std::uint32_t>(value);
        hash *= 1099511628211ull;
    };
    const auto appendBool = [&](const bool value)
    {
        hash ^= value ? 1ull : 0ull;
        hash *= 1099511628211ull;
    };
    appendFloat(material.albedoColor.x);
    appendFloat(material.albedoColor.y);
    appendFloat(material.albedoColor.z);
    appendFloat(material.albedoColor.w);
    appendFloat(material.emissiveColor.x);
    appendFloat(material.emissiveColor.y);
    appendFloat(material.emissiveColor.z);
    appendFloat(material.metallic);
    appendFloat(material.roughness);
    appendFloat(material.occlusionStrength);
    appendFloat(material.normalScale);
    appendFloat(material.emissiveStrength);
    appendFloat(material.alphaCutoff);
    appendFloat(material.alphaMode);
    appendBool(material.useAlbedoTexture);
    appendBool(material.useMetallicRoughnessTexture);
    appendBool(material.useNormalTexture);
    appendBool(material.useOcclusionTexture);
    appendBool(material.useEmissiveTexture);
    return hash != 0 ? hash : 1;
}

SharedMaterialConstants ResolveMaterialConstants(
    const Scene::RenderObject& renderObject)
{
    SharedMaterialConstants constants{};
    if (renderObject.material == nullptr)
    {
        return constants;
    }

    const Asset::Material::Parameters& base =
        renderObject.material->GetParameters();
    constants.albedoColor = base.albedoColor;
    constants.emissiveColor = base.emissiveColor;
    constants.metallic = base.metallic;
    constants.roughness = base.roughness;
    constants.occlusionStrength = base.occlusionStrength;
    constants.normalScale = base.normalScale;
    constants.emissiveStrength = base.emissiveStrength;
    constants.alphaCutoff = base.alphaCutoff;
    constants.alphaMode = base.alphaMode;
    constants.useAlbedoTexture = base.useAlbedoTexture ? 1.0f : 0.0f;
    constants.useMetallicRoughnessTexture =
        base.useMetallicRoughnessTexture ? 1.0f : 0.0f;
    constants.useNormalTexture = base.useNormalTexture ? 1.0f : 0.0f;
    constants.useOcclusionTexture = base.useOcclusionTexture ? 1.0f : 0.0f;
    constants.useEmissiveTexture = base.useEmissiveTexture ? 1.0f : 0.0f;

    if (renderObject.hasMaterialOverride)
    {
        const Scene::MaterialParameterOverride& value =
            renderObject.materialOverride;
        constants.albedoColor = value.albedoColor;
        constants.emissiveColor = value.emissiveColor;
        constants.metallic = value.metallic;
        constants.roughness = value.roughness;
        constants.occlusionStrength = value.occlusionStrength;
        constants.normalScale = value.normalScale;
        constants.emissiveStrength = value.emissiveStrength;
        constants.alphaCutoff = value.alphaCutoff;
        constants.alphaMode = value.alphaMode;
        constants.useAlbedoTexture = value.useAlbedoTexture ? 1.0f : 0.0f;
        constants.useMetallicRoughnessTexture =
            value.useMetallicRoughnessTexture ? 1.0f : 0.0f;
        constants.useNormalTexture = value.useNormalTexture ? 1.0f : 0.0f;
        constants.useOcclusionTexture = value.useOcclusionTexture ? 1.0f : 0.0f;
        constants.useEmissiveTexture = value.useEmissiveTexture ? 1.0f : 0.0f;
    }
    return constants;
}

MaterialRenderQueue ResolveMaterialRenderQueue(
    const Scene::RenderObject& renderObject)
{
    const SharedMaterialConstants material =
        ResolveMaterialConstants(renderObject);
    if (material.alphaMode >= 1.5f)
    {
        return MaterialRenderQueue::Transparent;
    }
    if (material.alphaMode >= 0.5f)
    {
        return MaterialRenderQueue::AlphaMask;
    }
    return MaterialRenderQueue::Opaque;
}

bool IsDoubleSidedMaterial(
    const Scene::RenderObject& renderObject)
{
    return renderObject.material != nullptr
        && renderObject.material
               ->GetParameters().doubleSided;
}

SharedMaterialConstants ResolveMaterialConstants(
    const Scene::RenderSceneView& scene,
    const std::size_t objectIndex)
{
    const Scene::RenderObject& object =
        scene.GetRenderObjects().at(objectIndex);
    SharedMaterialConstants constants{};
    if (const Scene::RenderSceneMaterialBinding* const material =
            scene.FindMaterialBinding(objectIndex))
    {
        const Asset::Material::Parameters& base = material->parameters;
        constants.albedoColor = base.albedoColor;
        constants.emissiveColor = base.emissiveColor;
        constants.metallic = base.metallic;
        constants.roughness = base.roughness;
        constants.occlusionStrength = base.occlusionStrength;
        constants.normalScale = base.normalScale;
        constants.emissiveStrength = base.emissiveStrength;
        constants.alphaCutoff = base.alphaCutoff;
        constants.alphaMode = base.alphaMode;
        constants.useAlbedoTexture = base.useAlbedoTexture ? 1.0f : 0.0f;
        constants.useMetallicRoughnessTexture =
            base.useMetallicRoughnessTexture ? 1.0f : 0.0f;
        constants.useNormalTexture = base.useNormalTexture ? 1.0f : 0.0f;
        constants.useOcclusionTexture = base.useOcclusionTexture ? 1.0f : 0.0f;
        constants.useEmissiveTexture = base.useEmissiveTexture ? 1.0f : 0.0f;
    }
    else
    {
        constants = ResolveMaterialConstants(object);
    }

    if (object.hasMaterialOverride)
    {
        const Scene::MaterialParameterOverride& value =
            object.materialOverride;
        constants.albedoColor = value.albedoColor;
        constants.emissiveColor = value.emissiveColor;
        constants.metallic = value.metallic;
        constants.roughness = value.roughness;
        constants.occlusionStrength = value.occlusionStrength;
        constants.normalScale = value.normalScale;
        constants.emissiveStrength = value.emissiveStrength;
        constants.alphaCutoff = value.alphaCutoff;
        constants.alphaMode = value.alphaMode;
        constants.useAlbedoTexture = value.useAlbedoTexture ? 1.0f : 0.0f;
        constants.useMetallicRoughnessTexture =
            value.useMetallicRoughnessTexture ? 1.0f : 0.0f;
        constants.useNormalTexture = value.useNormalTexture ? 1.0f : 0.0f;
        constants.useOcclusionTexture = value.useOcclusionTexture ? 1.0f : 0.0f;
        constants.useEmissiveTexture = value.useEmissiveTexture ? 1.0f : 0.0f;
    }
    return constants;
}

MaterialRenderQueue ResolveMaterialRenderQueue(
    const Scene::RenderSceneView& scene,
    const std::size_t objectIndex)
{
    const SharedMaterialConstants material =
        ResolveMaterialConstants(scene, objectIndex);
    if (material.alphaMode >= 1.5f)
    {
        return MaterialRenderQueue::Transparent;
    }
    if (material.alphaMode >= 0.5f)
    {
        return MaterialRenderQueue::AlphaMask;
    }
    return MaterialRenderQueue::Opaque;
}

bool IsDoubleSidedMaterial(
    const Scene::RenderSceneView& scene,
    const std::size_t objectIndex)
{
    if (const Scene::RenderSceneMaterialBinding* const material =
            scene.FindMaterialBinding(objectIndex))
    {
        return material->parameters.doubleSided;
    }
    return IsDoubleSidedMaterial(
        scene.GetRenderObjects().at(objectIndex));
}
} // namespace Prism::Renderer
