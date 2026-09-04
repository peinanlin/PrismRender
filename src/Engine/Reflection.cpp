#include "Engine/Reflection.h"

#include <algorithm>

namespace Prism::Engine
{
ReflectionRegistry::ReflectionRegistry()
{
    m_components = {
        {"Name", true, {{"value", PropertyType::String, true, "Name", "Entity"}}},
        {"Transform", true,
         {{"position", PropertyType::Double3, true, "Position", "Transform", PropertyEditorHint::Default, 0.0f, 0.0f, 0.05f},
          {"rotation", PropertyType::Float3, true, "Rotation", "Transform", PropertyEditorHint::AngleRadians, 0.0f, 0.0f, 0.25f},
          {"scale", PropertyType::Float3, true, "Scale", "Transform", PropertyEditorHint::Default, 0.01f, 100.0f, 0.02f, true}}},
        {"Hierarchy", false, {{"parent", PropertyType::EntityReference, true, "Parent", "Hierarchy"}}},
        {"MeshRenderer", false,
         {{"meshAsset", PropertyType::AssetPath, true, "Mesh", "Renderer", PropertyEditorHint::Default, 0.0f, 0.0f, 0.1f, false, "mesh"},
          {"materialAsset", PropertyType::AssetPath, true, "Material", "Renderer", PropertyEditorHint::Default, 0.0f, 0.0f, 0.1f, false, "material"},
          {"visible", PropertyType::Boolean, true, "Visible", "Renderer"}}},
        {"MaterialOverride", false,
         {{"albedoColor", PropertyType::Float4, true, "Base Color", "Material Instance", PropertyEditorHint::Color},
          {"metallic", PropertyType::Float, true, "Metallic", "Material Instance", PropertyEditorHint::Default, 0.0f, 1.0f, 0.01f, true},
          {"roughness", PropertyType::Float, true, "Roughness", "Material Instance", PropertyEditorHint::Default, 0.02f, 1.0f, 0.01f, true},
          {"emissiveColor", PropertyType::Float3, true, "Emissive Color", "Material Instance", PropertyEditorHint::Color},
          {"emissiveStrength", PropertyType::Float, true, "Emissive Strength", "Material Instance", PropertyEditorHint::Default, 0.0f, 64.0f, 0.05f, true},
          {"normalScale", PropertyType::Float, true, "Normal Scale", "Material Instance", PropertyEditorHint::Default, 0.0f, 4.0f, 0.01f, true},
          {"occlusionStrength", PropertyType::Float, true, "Occlusion Strength", "Material Instance", PropertyEditorHint::Default, 0.0f, 1.0f, 0.01f, true},
          {"alphaCutoff", PropertyType::Float, true, "Alpha Cutoff", "Material Instance", PropertyEditorHint::Default, 0.0f, 1.0f, 0.01f, true},
          {"alphaMode", PropertyType::Float, true, "Alpha Mode", "Material Instance", PropertyEditorHint::Default, 0.0f, 2.0f, 1.0f, true},
          {"useAlbedoTexture", PropertyType::Boolean, true, "Use Base Color Texture", "Textures"},
          {"useMetallicRoughnessTexture", PropertyType::Boolean, true, "Use Metallic-Roughness Texture", "Textures"},
          {"useNormalTexture", PropertyType::Boolean, true, "Use Normal Texture", "Textures"},
          {"useOcclusionTexture", PropertyType::Boolean, true, "Use Occlusion Texture", "Textures"},
          {"useEmissiveTexture", PropertyType::Boolean, true, "Use Emissive Texture", "Textures"}}},
        {"Camera", false,
         {{"fieldOfViewY", PropertyType::Float, true, "Field of View", "Camera", PropertyEditorHint::AngleRadians, 0.0174533f, 3.124139f, 0.25f, true},
          {"nearPlane", PropertyType::Float, true, "Near Plane", "Camera", PropertyEditorHint::Default, 0.001f, 100.0f, 0.01f, true},
          {"farPlane", PropertyType::Float, true, "Far Plane", "Camera", PropertyEditorHint::Default, 1.0f, 100000.0f, 1.0f, true}}},
        {"DirectionalLight", false,
         {{"direction", PropertyType::Float3, true, "Direction", "Directional Light"},
          {"color", PropertyType::Float3, true, "Color", "Directional Light", PropertyEditorHint::Color},
          {"intensity", PropertyType::Float, true, "Intensity", "Directional Light", PropertyEditorHint::Default, 0.0f, 100.0f, 0.05f, true}}},
        {"PointLight", false,
         {{"color", PropertyType::Float3, true, "Color", "Point Light", PropertyEditorHint::Color},
          {"intensity", PropertyType::Float, true, "Intensity", "Point Light", PropertyEditorHint::Default, 0.0f, 100.0f, 0.05f, true},
          {"range", PropertyType::Float, true, "Range", "Point Light", PropertyEditorHint::Default, 0.01f, 10000.0f, 0.1f, true},
          {"castsShadow", PropertyType::Boolean, true, "Cast Shadows", "Point Light"}}},
        {"SpotLight", false,
         {{"direction", PropertyType::Float3, true, "Direction", "Spot Light"},
          {"color", PropertyType::Float3, true, "Color", "Spot Light", PropertyEditorHint::Color},
          {"intensity", PropertyType::Float, true, "Intensity", "Spot Light", PropertyEditorHint::Default, 0.0f, 100.0f, 0.05f, true},
          {"range", PropertyType::Float, true, "Range", "Spot Light", PropertyEditorHint::Default, 0.01f, 10000.0f, 0.1f, true},
          {"innerAngleRadians", PropertyType::Float, true, "Inner Angle", "Spot Light", PropertyEditorHint::AngleRadians, 0.001f, 1.55f, 0.25f, true},
          {"outerAngleRadians", PropertyType::Float, true, "Outer Angle", "Spot Light", PropertyEditorHint::AngleRadians, 0.002f, 1.56f, 0.25f, true},
          {"castsShadow", PropertyType::Boolean, true, "Cast Shadows", "Spot Light"}}}};
}

std::span<const ComponentDescriptor> ReflectionRegistry::GetComponents() const
{
    return m_components;
}

const ComponentDescriptor* ReflectionRegistry::FindComponent(const std::string_view name) const
{
    const auto found = std::ranges::find(m_components, name, &ComponentDescriptor::name);
    return found == m_components.end() ? nullptr : &*found;
}

const PropertyDescriptor* ReflectionRegistry::FindProperty(
    const std::string_view component,
    const std::string_view property) const
{
    const ComponentDescriptor* descriptor = FindComponent(component);
    if (descriptor == nullptr)
    {
        return nullptr;
    }
    const auto found = std::ranges::find(descriptor->properties, property, &PropertyDescriptor::name);
    return found == descriptor->properties.end() ? nullptr : &*found;
}

std::string_view ToString(const PropertyType type)
{
    switch (type)
    {
    case PropertyType::String: return "string";
    case PropertyType::Float: return "float";
    case PropertyType::Float3: return "float3";
    case PropertyType::Double3: return "double3";
    case PropertyType::Float4: return "float4";
    case PropertyType::Boolean: return "boolean";
    case PropertyType::EntityReference: return "entityReference";
    case PropertyType::AssetPath: return "assetPath";
    }
    return "unknown";
}
} // namespace Prism::Engine
