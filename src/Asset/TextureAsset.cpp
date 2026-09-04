#include "Asset/TextureAsset.h"

#include <utility>

namespace Prism::Asset
{
void TextureAsset::SetName(std::string name)
{
    m_name = std::move(name);
}

void TextureAsset::SetSourcePath(std::string sourcePath)
{
    m_sourcePath = std::move(sourcePath);
}

void TextureAsset::SetSolidColor(const DirectX::XMFLOAT4& solidColor)
{
    m_solidColor = solidColor;
}

void TextureAsset::SetImageData(const std::uint32_t width, const std::uint32_t height, std::vector<std::uint8_t> rgbaPixels)
{
    m_width = width;
    m_height = height;
    m_imageData = std::move(rgbaPixels);
}

const std::string& TextureAsset::GetName() const
{
    return m_name;
}

const std::string& TextureAsset::GetSourcePath() const
{
    return m_sourcePath;
}

const DirectX::XMFLOAT4& TextureAsset::GetSolidColor() const
{
    return m_solidColor;
}

bool TextureAsset::HasImageData() const
{
    return m_width > 0 && m_height > 0 && !m_imageData.empty();
}

std::uint32_t TextureAsset::GetWidth() const
{
    return m_width;
}

std::uint32_t TextureAsset::GetHeight() const
{
    return m_height;
}

const std::vector<std::uint8_t>& TextureAsset::GetImageData() const
{
    return m_imageData;
}
} // namespace Prism::Asset
