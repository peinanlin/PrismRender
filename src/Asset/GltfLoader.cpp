#include "Asset/GltfLoader.h"

#include "Asset/MaterialAsset.h"
#include "Asset/MeshAsset.h"
#include "Asset/TextureAsset.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#if defined(PRISM_RENDER_HAS_TINYGLTF)
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include <tiny_gltf.h>
#endif

using namespace DirectX;

namespace Prism::Asset
{
#if defined(PRISM_RENDER_HAS_TINYGLTF)
namespace
{
constexpr float AlphaModeOpaque = 0.0f;
constexpr float AlphaModeMask = 1.0f;
constexpr float AlphaModeBlend = 2.0f;

template<typename T>
const T* GetAccessorData(const tinygltf::Model& model, const tinygltf::Accessor& accessor)
{
    if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
    {
        return nullptr;
    }

    const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
    if (bufferView.buffer < 0 || bufferView.buffer >= static_cast<int>(model.buffers.size()))
    {
        return nullptr;
    }

    const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];
    const std::size_t byteOffset = static_cast<std::size_t>(bufferView.byteOffset + accessor.byteOffset);
    if (byteOffset >= buffer.data.size())
    {
        return nullptr;
    }

    return reinterpret_cast<const T*>(buffer.data.data() + byteOffset);
}

std::shared_ptr<TextureAsset> BuildTextureAssetFromImage(
    const tinygltf::Model& model,
    const int textureIndex,
    const std::string& fallbackName)
{
    if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
    {
        return nullptr;
    }

    const tinygltf::Texture& texture = model.textures[textureIndex];
    if (texture.source < 0 || texture.source >= static_cast<int>(model.images.size()))
    {
        return nullptr;
    }

    const tinygltf::Image& image = model.images[texture.source];
    if (image.width <= 0 || image.height <= 0 || image.image.empty())
    {
        return nullptr;
    }

    std::vector<std::uint8_t> rgbaPixels(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4u, 255u);
    if (image.component == 4)
    {
        std::copy(image.image.begin(), image.image.end(), rgbaPixels.begin());
    }
    else if (image.component == 3)
    {
        for (int pixelIndex = 0; pixelIndex < image.width * image.height; ++pixelIndex)
        {
            rgbaPixels[static_cast<std::size_t>(pixelIndex) * 4u + 0u] = image.image[static_cast<std::size_t>(pixelIndex) * 3u + 0u];
            rgbaPixels[static_cast<std::size_t>(pixelIndex) * 4u + 1u] = image.image[static_cast<std::size_t>(pixelIndex) * 3u + 1u];
            rgbaPixels[static_cast<std::size_t>(pixelIndex) * 4u + 2u] = image.image[static_cast<std::size_t>(pixelIndex) * 3u + 2u];
            rgbaPixels[static_cast<std::size_t>(pixelIndex) * 4u + 3u] = 255u;
        }
    }
    else if (image.component == 1)
    {
        for (int pixelIndex = 0; pixelIndex < image.width * image.height; ++pixelIndex)
        {
            const std::uint8_t value = image.image[static_cast<std::size_t>(pixelIndex)];
            rgbaPixels[static_cast<std::size_t>(pixelIndex) * 4u + 0u] = value;
            rgbaPixels[static_cast<std::size_t>(pixelIndex) * 4u + 1u] = value;
            rgbaPixels[static_cast<std::size_t>(pixelIndex) * 4u + 2u] = value;
            rgbaPixels[static_cast<std::size_t>(pixelIndex) * 4u + 3u] = 255u;
        }
    }
    else
    {
        return nullptr;
    }

    auto textureAsset = std::make_shared<TextureAsset>();
    textureAsset->SetName(image.name.empty() ? fallbackName : image.name);
    textureAsset->SetSourcePath(image.uri);
    textureAsset->SetImageData(static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height), std::move(rgbaPixels));
    return textureAsset;
}

XMMATRIX ReadNodeTransform(const tinygltf::Node& node)
{
    if (node.matrix.size() == 16)
    {
        XMFLOAT4X4 matrix{};
        matrix._11 = static_cast<float>(node.matrix[0]);
        matrix._12 = static_cast<float>(node.matrix[1]);
        matrix._13 = static_cast<float>(node.matrix[2]);
        matrix._14 = static_cast<float>(node.matrix[3]);
        matrix._21 = static_cast<float>(node.matrix[4]);
        matrix._22 = static_cast<float>(node.matrix[5]);
        matrix._23 = static_cast<float>(node.matrix[6]);
        matrix._24 = static_cast<float>(node.matrix[7]);
        matrix._31 = static_cast<float>(node.matrix[8]);
        matrix._32 = static_cast<float>(node.matrix[9]);
        matrix._33 = static_cast<float>(node.matrix[10]);
        matrix._34 = static_cast<float>(node.matrix[11]);
        matrix._41 = static_cast<float>(node.matrix[12]);
        matrix._42 = static_cast<float>(node.matrix[13]);
        matrix._43 = static_cast<float>(node.matrix[14]);
        matrix._44 = static_cast<float>(node.matrix[15]);
        return XMLoadFloat4x4(&matrix);
    }

    const XMVECTOR scale = node.scale.size() == 3
                               ? XMVectorSet(static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]), static_cast<float>(node.scale[2]), 0.0f)
                               : XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
    const XMVECTOR rotation = node.rotation.size() == 4
                                  ? XMVectorSet(static_cast<float>(node.rotation[0]),
                                                static_cast<float>(node.rotation[1]),
                                                static_cast<float>(node.rotation[2]),
                                                static_cast<float>(node.rotation[3]))
                                  : XMQuaternionIdentity();
    const XMVECTOR translation = node.translation.size() == 3
                                     ? XMVectorSet(static_cast<float>(node.translation[0]),
                                                   static_cast<float>(node.translation[1]),
                                                   static_cast<float>(node.translation[2]),
                                                   1.0f)
                                     : XMVectorZero();
    return XMMatrixAffineTransformation(scale, XMVectorZero(), rotation, translation);
}

bool ReadIndices(const tinygltf::Model& model, const tinygltf::Accessor& accessor, std::vector<std::uint16_t>& outIndices)
{
    outIndices.resize(accessor.count);

    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
    {
        const std::uint16_t* values = GetAccessorData<std::uint16_t>(model, accessor);
        if (values == nullptr)
        {
            return false;
        }
        std::copy(values, values + accessor.count, outIndices.begin());
        return true;
    }

    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
    {
        const std::uint32_t* values = GetAccessorData<std::uint32_t>(model, accessor);
        if (values == nullptr)
        {
            return false;
        }

        for (std::size_t index = 0; index < accessor.count; ++index)
        {
            if (values[index] > std::numeric_limits<std::uint16_t>::max())
            {
                return false;
            }
            outIndices[index] = static_cast<std::uint16_t>(values[index]);
        }
        return true;
    }

    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
    {
        const std::uint8_t* values = GetAccessorData<std::uint8_t>(model, accessor);
        if (values == nullptr)
        {
            return false;
        }

        std::transform(values, values + accessor.count, outIndices.begin(), [](const std::uint8_t value) { return static_cast<std::uint16_t>(value); });
        return true;
    }

    return false;
}

std::shared_ptr<MaterialAsset> BuildMaterialAsset(const tinygltf::Model& model, const int materialIndex)
{
    auto material = std::make_shared<MaterialAsset>();
    material->SetSpecularColor({0.85f, 0.85f, 0.85f});
    material->SetEmissiveColor({0.0f, 0.0f, 0.0f});
    material->SetMetallic(0.0f);
    material->SetRoughness(0.5f);
    material->SetShininess(64.0f);
    material->SetNormalScale(1.0f);
    material->SetEmissiveStrength(1.0f);
    material->SetAlphaCutoff(0.5f);
    material->SetAlphaMode(AlphaModeOpaque);
    material->SetDoubleSided(false);
    material->SetUseAlbedoTexture(false);
    material->SetUseMetallicRoughnessTexture(false);
    material->SetUseNormalTexture(false);
    material->SetUseOcclusionTexture(false);
    material->SetUseEmissiveTexture(false);
    material->SetOcclusionStrength(1.0f);

    if (materialIndex >= 0 && materialIndex < static_cast<int>(model.materials.size()))
    {
        const tinygltf::Material& source = model.materials[materialIndex];
        material->SetName(source.name.empty() ? ("Material_" + std::to_string(materialIndex)) : source.name);
        if (source.pbrMetallicRoughness.baseColorFactor.size() >= 4)
        {
            material->SetAlbedoColor({
                static_cast<float>(source.pbrMetallicRoughness.baseColorFactor[0]),
                static_cast<float>(source.pbrMetallicRoughness.baseColorFactor[1]),
                static_cast<float>(source.pbrMetallicRoughness.baseColorFactor[2]),
                static_cast<float>(source.pbrMetallicRoughness.baseColorFactor[3])});
        }
        material->SetMetallic(static_cast<float>(source.pbrMetallicRoughness.metallicFactor));
        material->SetRoughness(static_cast<float>(source.pbrMetallicRoughness.roughnessFactor));
        material->SetUseAlbedoTexture(source.pbrMetallicRoughness.baseColorTexture.index >= 0);
        material->SetUseMetallicRoughnessTexture(source.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0);
        material->SetUseNormalTexture(source.normalTexture.index >= 0);
        material->SetUseOcclusionTexture(source.occlusionTexture.index >= 0);
        material->SetUseEmissiveTexture(source.emissiveTexture.index >= 0);
        material->SetNormalScale(static_cast<float>(source.normalTexture.scale));
        material->SetOcclusionStrength(static_cast<float>(source.occlusionTexture.strength));
        material->SetAlphaCutoff(static_cast<float>(source.alphaCutoff));
        material->SetDoubleSided(source.doubleSided);
        if (source.alphaMode == "MASK")
        {
            material->SetAlphaMode(AlphaModeMask);
        }
        else if (source.alphaMode == "BLEND")
        {
            material->SetAlphaMode(AlphaModeBlend);
        }
        const auto emissiveStrengthIt = source.extensions.find("KHR_materials_emissive_strength");
        if (emissiveStrengthIt != source.extensions.end())
        {
            const auto strengthIt = emissiveStrengthIt->second.Get("emissiveStrength");
            if (strengthIt.IsNumber())
            {
                material->SetEmissiveStrength(static_cast<float>(strengthIt.Get<double>()));
            }
        }
        if (source.emissiveFactor.size() >= 3)
        {
            material->SetEmissiveColor({
                static_cast<float>(source.emissiveFactor[0]),
                static_cast<float>(source.emissiveFactor[1]),
                static_cast<float>(source.emissiveFactor[2])});
        }
    }
    else
    {
        material->SetName("DefaultMaterial");
    }

    return material;
}

std::shared_ptr<TextureAsset> BuildBaseColorTextureAsset(const tinygltf::Model& model, const int materialIndex)
{
    if (materialIndex < 0 || materialIndex >= static_cast<int>(model.materials.size()))
    {
        return nullptr;
    }

    const tinygltf::Material& material = model.materials[materialIndex];
    const int textureIndex = material.pbrMetallicRoughness.baseColorTexture.index;
    if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
    {
        return nullptr;
    }

    return BuildTextureAssetFromImage(model, textureIndex, "BaseColor_" + std::to_string(textureIndex));
}

std::shared_ptr<TextureAsset> BuildMetallicRoughnessTextureAsset(const tinygltf::Model& model, const int materialIndex)
{
    if (materialIndex < 0 || materialIndex >= static_cast<int>(model.materials.size()))
    {
        return nullptr;
    }

    const tinygltf::Material& material = model.materials[materialIndex];
    const int textureIndex = material.pbrMetallicRoughness.metallicRoughnessTexture.index;
    if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
    {
        return nullptr;
    }

    return BuildTextureAssetFromImage(model, textureIndex, "MetallicRoughness_" + std::to_string(textureIndex));
}

std::shared_ptr<TextureAsset> BuildNormalTextureAsset(const tinygltf::Model& model, const int materialIndex)
{
    if (materialIndex < 0 || materialIndex >= static_cast<int>(model.materials.size()))
    {
        return nullptr;
    }

    const tinygltf::Material& material = model.materials[materialIndex];
    const int textureIndex = material.normalTexture.index;
    if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
    {
        return nullptr;
    }

    return BuildTextureAssetFromImage(model, textureIndex, "Normal_" + std::to_string(textureIndex));
}

std::shared_ptr<TextureAsset> BuildOcclusionTextureAsset(const tinygltf::Model& model, const int materialIndex)
{
    if (materialIndex < 0 || materialIndex >= static_cast<int>(model.materials.size()))
    {
        return nullptr;
    }

    const tinygltf::Material& material = model.materials[materialIndex];
    const int textureIndex = material.occlusionTexture.index;
    if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
    {
        return nullptr;
    }

    return BuildTextureAssetFromImage(model, textureIndex, "Occlusion_" + std::to_string(textureIndex));
}

std::shared_ptr<TextureAsset> BuildEmissiveTextureAsset(const tinygltf::Model& model, const int materialIndex)
{
    if (materialIndex < 0 || materialIndex >= static_cast<int>(model.materials.size()))
    {
        return nullptr;
    }

    const tinygltf::Material& material = model.materials[materialIndex];
    const int textureIndex = material.emissiveTexture.index;
    if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
    {
        return nullptr;
    }

    return BuildTextureAssetFromImage(model, textureIndex, "Emissive_" + std::to_string(textureIndex));
}

std::shared_ptr<MeshAsset> BuildMeshAsset(const tinygltf::Model& model, const tinygltf::Primitive& primitive, const std::string& name)
{
    const auto positionIt = primitive.attributes.find("POSITION");
    if (positionIt == primitive.attributes.end())
    {
        return nullptr;
    }

    const tinygltf::Accessor& positionAccessor = model.accessors[positionIt->second];
    const float* positions = GetAccessorData<float>(model, positionAccessor);
    if (positions == nullptr || positionAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || positionAccessor.type != TINYGLTF_TYPE_VEC3)
    {
        return nullptr;
    }

    const tinygltf::Accessor* normalAccessor = nullptr;
    const auto normalIt = primitive.attributes.find("NORMAL");
    if (normalIt != primitive.attributes.end())
    {
        normalAccessor = &model.accessors[normalIt->second];
    }

    const tinygltf::Accessor* uvAccessor = nullptr;
    const auto uvIt = primitive.attributes.find("TEXCOORD_0");
    if (uvIt != primitive.attributes.end())
    {
        uvAccessor = &model.accessors[uvIt->second];
    }

    const float* normals = normalAccessor != nullptr ? GetAccessorData<float>(model, *normalAccessor) : nullptr;
    const float* uvs = uvAccessor != nullptr ? GetAccessorData<float>(model, *uvAccessor) : nullptr;

    const tinygltf::Accessor* tangentAccessor = nullptr;
    const auto tangentIt = primitive.attributes.find("TANGENT");
    if (tangentIt != primitive.attributes.end())
    {
        tangentAccessor = &model.accessors[tangentIt->second];
    }
    const float* tangents = tangentAccessor != nullptr ? GetAccessorData<float>(model, *tangentAccessor) : nullptr;

    std::vector<MeshVertex> vertices(positionAccessor.count);
    XMFLOAT3 boundsMin{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    XMFLOAT3 boundsMax{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    for (std::size_t vertexIndex = 0; vertexIndex < positionAccessor.count; ++vertexIndex)
    {
        MeshVertex& vertex = vertices[vertexIndex];
        vertex.position = {positions[vertexIndex * 3 + 0], positions[vertexIndex * 3 + 1], positions[vertexIndex * 3 + 2]};
        vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
        vertex.normal = normals != nullptr ? XMFLOAT3{normals[vertexIndex * 3 + 0], normals[vertexIndex * 3 + 1], normals[vertexIndex * 3 + 2]}
                                           : XMFLOAT3{0.0f, 1.0f, 0.0f};
        if (uvs != nullptr)
        {
            vertex.texCoord = {uvs[vertexIndex * 2 + 0], 1.0f - uvs[vertexIndex * 2 + 1]};
        }
        if (tangents != nullptr)
        {
            vertex.tangent = {
                tangents[vertexIndex * 4 + 0],
                tangents[vertexIndex * 4 + 1],
                tangents[vertexIndex * 4 + 2],
                tangents[vertexIndex * 4 + 3]};
        }

        boundsMin.x = std::min(boundsMin.x, vertex.position.x);
        boundsMin.y = std::min(boundsMin.y, vertex.position.y);
        boundsMin.z = std::min(boundsMin.z, vertex.position.z);
        boundsMax.x = std::max(boundsMax.x, vertex.position.x);
        boundsMax.y = std::max(boundsMax.y, vertex.position.y);
        boundsMax.z = std::max(boundsMax.z, vertex.position.z);
    }

    std::vector<std::uint16_t> indices;
    if (primitive.indices >= 0)
    {
        if (!ReadIndices(model, model.accessors[primitive.indices], indices))
        {
            return nullptr;
        }
    }
    else
    {
        indices.resize(vertices.size());
        for (std::size_t index = 0; index < vertices.size(); ++index)
        {
            indices[index] = static_cast<std::uint16_t>(index);
        }
    }

    const XMFLOAT3 center{
        (boundsMin.x + boundsMax.x) * 0.5f,
        (boundsMin.y + boundsMax.y) * 0.5f,
        (boundsMin.z + boundsMax.z) * 0.5f};
    float radius = 0.0f;
    for (const MeshVertex& vertex : vertices)
    {
        const float dx = vertex.position.x - center.x;
        const float dy = vertex.position.y - center.y;
        const float dz = vertex.position.z - center.z;
        radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
    }

    auto meshAsset = std::make_shared<MeshAsset>();
    meshAsset->SetName(name);
    meshAsset->SetGeometry(std::move(vertices), std::move(indices), {boundsMin, boundsMax, radius});
    return meshAsset;
}

void TraverseNode(const tinygltf::Model& model, const int nodeIndex, const XMMATRIX& parentWorld, GltfLoader::SceneData& outScene)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size()))
    {
        return;
    }

    const tinygltf::Node& node = model.nodes[nodeIndex];
    const XMMATRIX local = ReadNodeTransform(node);
    const XMMATRIX world = local * parentWorld;
    const std::string nodeName = node.name.empty() ? ("Node_" + std::to_string(nodeIndex)) : node.name;

    if (node.mesh >= 0 && node.mesh < static_cast<int>(model.meshes.size()))
    {
        const tinygltf::Mesh& mesh = model.meshes[node.mesh];
        for (std::size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
        {
            const tinygltf::Primitive& primitive = mesh.primitives[primitiveIndex];
            if (primitive.mode != TINYGLTF_MODE_TRIANGLES)
            {
                continue;
            }

            auto meshAsset = BuildMeshAsset(model, primitive, nodeName + "_Primitive_" + std::to_string(primitiveIndex));
            if (meshAsset == nullptr)
            {
                continue;
            }

            auto materialAsset = BuildMaterialAsset(model, primitive.material);
            auto textureAsset = BuildBaseColorTextureAsset(model, primitive.material);
            auto metallicRoughnessTextureAsset = BuildMetallicRoughnessTextureAsset(model, primitive.material);
            auto normalTextureAsset = BuildNormalTextureAsset(model, primitive.material);
            auto occlusionTextureAsset = BuildOcclusionTextureAsset(model, primitive.material);
            auto emissiveTextureAsset = BuildEmissiveTextureAsset(model, primitive.material);
            XMFLOAT4X4 worldStorage{};
            XMStoreFloat4x4(&worldStorage, world);
            outScene.instances.push_back({
                nodeName,
                nodeIndex,
                node.mesh,
                static_cast<std::uint32_t>(primitiveIndex),
                primitive.material,
                std::move(meshAsset),
                std::move(materialAsset),
                std::move(textureAsset),
                std::move(metallicRoughnessTextureAsset),
                std::move(normalTextureAsset),
                std::move(occlusionTextureAsset),
                std::move(emissiveTextureAsset),
                worldStorage});
        }
    }

    for (const int child : node.children)
    {
        TraverseNode(model, child, world, outScene);
    }
}
} // namespace
#endif

bool GltfLoader::LoadScene(const std::string& path, SceneData& outScene, std::string* outErrorMessage) const
{
    outScene = {};

#if defined(PRISM_RENDER_HAS_TINYGLTF)
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string warnings;
    std::string errors;
    const std::filesystem::path scenePath(path);
    const std::string extension = scenePath.extension().string();
    bool loaded = false;
    if (extension == ".glb")
    {
        loaded = loader.LoadBinaryFromFile(&model, &errors, &warnings, path);
    }
    else
    {
        loaded = loader.LoadASCIIFromFile(&model, &errors, &warnings, path);
    }

    if (!loaded)
    {
        if (outErrorMessage != nullptr)
        {
            if (!errors.empty() && !warnings.empty())
            {
                *outErrorMessage = errors + " Warnings: " + warnings;
            }
            else if (!errors.empty())
            {
                *outErrorMessage = errors;
            }
            else if (!warnings.empty())
            {
                *outErrorMessage = warnings;
            }
            else
            {
                *outErrorMessage = "Failed to load glTF file.";
            }
        }
        return false;
    }

    const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : (!model.scenes.empty() ? 0 : -1);
    if (sceneIndex < 0 || sceneIndex >= static_cast<int>(model.scenes.size()))
    {
        if (outErrorMessage != nullptr)
        {
            *outErrorMessage = "No default glTF scene found.";
        }
        return false;
    }

    for (const int nodeIndex : model.scenes[sceneIndex].nodes)
    {
        TraverseNode(model, nodeIndex, XMMatrixIdentity(), outScene);
    }

    return !outScene.instances.empty();
#else
    (void)path;
    if (outErrorMessage != nullptr)
    {
        *outErrorMessage = "glTF import is disabled because tinygltf headers are not available.";
    }
    return false;
#endif
}
} // namespace Prism::Asset
