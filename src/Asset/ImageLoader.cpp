#include "Asset/ImageLoader.h"

#include "Asset/TextureAsset.h"

#include <cstdint>
#include <vector>

#if defined(PRISM_RENDER_HAS_TINYGLTF)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#endif

namespace Prism::Asset
{
bool ImageLoader::LoadRgba8(
    const std::filesystem::path& path,
    std::shared_ptr<TextureAsset>& outTexture,
    std::string* outError)
{
#if defined(PRISM_RENDER_HAS_TINYGLTF)
    int width = 0;
    int height = 0;
    int sourceChannels = 0;
    stbi_uc* pixels = stbi_load(
        path.string().c_str(),
        &width,
        &height,
        &sourceChannels,
        STBI_rgb_alpha);
    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        if (outError != nullptr)
        {
            const char* reason = stbi_failure_reason();
            *outError = reason != nullptr
                ? reason
                : "The image decoder returned no pixels.";
        }
        if (pixels != nullptr)
        {
            stbi_image_free(pixels);
        }
        return false;
    }

    const std::size_t byteCount =
        static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height) * 4u;
    std::vector<std::uint8_t> rgba(pixels, pixels + byteCount);
    stbi_image_free(pixels);

    auto texture = std::make_shared<TextureAsset>();
    texture->SetName(path.stem().string());
    texture->SetSourcePath(path.generic_string());
    texture->SetImageData(
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
        std::move(rgba));
    outTexture = std::move(texture);
    return true;
#else
    (void)path;
    outTexture.reset();
    if (outError != nullptr)
    {
        *outError = "Image import requires the vendored tinygltf/stb_image dependency.";
    }
    return false;
#endif
}
}
