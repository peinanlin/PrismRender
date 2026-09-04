#include "Asset/EnvironmentMapLoader.h"

#include "Asset/Texture.h"
#include "RHI/IGraphicsDevice.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(PRISM_RENDER_HAS_TINYGLTF)
#include <stb_image.h>
#endif

namespace Prism::Asset
{
namespace
{
// Preserve the renderer's original neutral fallback. Scene-specific looks
// must be authored by their scene settings instead of changing this shared
// IBL input for every demo.
constexpr std::array<DirectX::XMFLOAT4, 6> FallbackFaceColors{{
    {0.72f, 0.78f, 0.88f, 1.0f},
    {0.30f, 0.33f, 0.38f, 1.0f},
    {0.18f, 0.28f, 0.46f, 1.0f},
    {0.24f, 0.18f, 0.14f, 1.0f},
    {0.58f, 0.66f, 0.78f, 1.0f},
    {0.56f, 0.64f, 0.76f, 1.0f},
}};
}


std::shared_ptr<Texture> EnvironmentMapLoader::LoadCubemapFromDirectory(
    RHI::IGraphicsDevice& device,
    const std::string& directoryPath,
    std::string* outStatusMessage) const
{
#if defined(PRISM_RENDER_HAS_TINYGLTF)
    const std::filesystem::path directory(directoryPath);
    const std::array<std::string, 6> faceBaseNames = {"px", "nx", "py", "ny", "pz", "nz"};
    const std::array<std::string, 5> extensions = {".png", ".jpg", ".jpeg", ".bmp", ".tga"};
    std::array<std::vector<std::uint8_t>, 6> faceStorage{};
    std::array<const std::uint8_t*, 6> facePixels{};
    int expectedWidth = 0;
    int expectedHeight = 0;

    for (std::size_t faceIndex = 0; faceIndex < faceBaseNames.size(); ++faceIndex)
    {
        std::filesystem::path facePath;
        for (const std::string& extension : extensions)
        {
            const std::filesystem::path candidate = directory / (faceBaseNames[faceIndex] + extension);
            if (std::filesystem::exists(candidate))
            {
                facePath = candidate;
                break;
            }
        }
        if (facePath.empty())
        {
            if (outStatusMessage != nullptr)
            {
                *outStatusMessage = "Environment cubemap face is missing: " + faceBaseNames[faceIndex];
            }
            return nullptr;
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load(facePath.string().c_str(), &width, &height, &channels, 4);
        if (pixels == nullptr)
        {
            if (outStatusMessage != nullptr)
            {
                *outStatusMessage = "Failed to load cubemap face: " + facePath.string();
            }
            return nullptr;
        }
        if (faceIndex == 0)
        {
            expectedWidth = width;
            expectedHeight = height;
        }
        else if (width != expectedWidth || height != expectedHeight)
        {
            stbi_image_free(pixels);
            if (outStatusMessage != nullptr)
            {
                *outStatusMessage = "Cubemap faces must share the same resolution.";
            }
            return nullptr;
        }
        faceStorage[faceIndex].assign(
            pixels,
            pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
        stbi_image_free(pixels);
        facePixels[faceIndex] = faceStorage[faceIndex].data();
    }

    auto texture = std::make_shared<Texture>();
    texture->InitializeCubemapRgba8(
        device,
        static_cast<std::uint32_t>(expectedWidth),
        static_cast<std::uint32_t>(expectedHeight),
        facePixels);
    if (outStatusMessage != nullptr)
    {
        *outStatusMessage = "Environment cubemap loaded through the public RHI from: " + directory.string();
    }
    return texture;
#else
    (void)device;
    (void)directoryPath;
    if (outStatusMessage != nullptr)
    {
        *outStatusMessage = "Environment cubemap loading is unavailable because stb_image is not enabled.";
    }
    return nullptr;
#endif
}


std::shared_ptr<Texture> EnvironmentMapLoader::CreateFallbackCubemap(
    RHI::IGraphicsDevice& device) const
{
    auto texture = std::make_shared<Texture>();
    texture->InitializeCubemapSolidColors(
        device,
        FallbackFaceColors.data());
    return texture;
}
} // namespace Prism::Asset
