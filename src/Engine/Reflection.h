#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Prism::Engine
{
enum class PropertyType
{
    String,
    Float,
    Float3,
    Double3,
    Float4,
    Boolean,
    EntityReference,
    AssetPath
};

enum class PropertyEditorHint
{
    Default,
    Color,
    AngleRadians
};

struct PropertyDescriptor
{
    std::string name;
    PropertyType type = PropertyType::String;
    bool writable = true;
    std::string displayName;
    std::string category;
    PropertyEditorHint editorHint = PropertyEditorHint::Default;
    float minimum = 0.0f;
    float maximum = 0.0f;
    float step = 0.1f;
    bool hasRange = false;
    std::string acceptedAssetType;
};

struct ComponentDescriptor
{
    std::string name;
    bool required = false;
    std::vector<PropertyDescriptor> properties;
};

class ReflectionRegistry
{
public:
    ReflectionRegistry();

    [[nodiscard]] std::span<const ComponentDescriptor> GetComponents() const;
    [[nodiscard]] const ComponentDescriptor* FindComponent(std::string_view name) const;
    [[nodiscard]] const PropertyDescriptor* FindProperty(
        std::string_view component,
        std::string_view property) const;

private:
    std::vector<ComponentDescriptor> m_components;
};

[[nodiscard]] std::string_view ToString(PropertyType type);
} // namespace Prism::Engine
