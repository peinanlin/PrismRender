#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace Prism::Asset
{
class TextureAsset;

class ImageLoader
{
public:
    [[nodiscard]] static bool LoadRgba8(
        const std::filesystem::path& path,
        std::shared_ptr<TextureAsset>& outTexture,
        std::string* outError = nullptr);
};
}
