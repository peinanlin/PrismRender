#pragma once

#include <cstdint>

namespace Prism::Asset
{
template<typename Tag>
class AssetHandle
{
public:
    AssetHandle() = default;
    explicit AssetHandle(const std::uint32_t value)
        : m_value(value)
    {
    }

    bool IsValid() const
    {
        return m_value != 0;
    }

    std::uint32_t Value() const
    {
        return m_value;
    }

    bool operator==(const AssetHandle& other) const = default;

private:
    std::uint32_t m_value = 0;
};

struct MeshAssetTag
{
};

struct TextureAssetTag
{
};

struct MaterialAssetTag
{
};

using MeshHandle = AssetHandle<MeshAssetTag>;
using TextureHandle = AssetHandle<TextureAssetTag>;
using MaterialHandle = AssetHandle<MaterialAssetTag>;
} // namespace Prism::Asset
