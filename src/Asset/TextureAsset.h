#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

namespace Prism::Asset
{
class TextureAsset
{
public:
    void SetName(std::string name);
    void SetSourcePath(std::string sourcePath);
    void SetSolidColor(const DirectX::XMFLOAT4& solidColor);
    void SetImageData(std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t> rgbaPixels);

    const std::string& GetName() const;
    const std::string& GetSourcePath() const;
    const DirectX::XMFLOAT4& GetSolidColor() const;
    bool HasImageData() const;
    std::uint32_t GetWidth() const;
    std::uint32_t GetHeight() const;
    const std::vector<std::uint8_t>& GetImageData() const;

private:
    std::string m_name;
    std::string m_sourcePath;
    DirectX::XMFLOAT4 m_solidColor{1.0f, 1.0f, 1.0f, 1.0f};
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::vector<std::uint8_t> m_imageData;
};
} // namespace Prism::Asset
