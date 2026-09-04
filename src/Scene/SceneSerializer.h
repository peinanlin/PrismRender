#pragma once

#include <filesystem>
#include <string>

namespace Prism::Asset
{
class AssetRegistry;
}

namespace Prism::Scene
{
class RenderScene;

class SceneSerializer
{
public:
    static bool Save(
        const std::filesystem::path& path,
        const RenderScene& scene,
        const Asset::AssetRegistry& assetRegistry,
        std::string* outMessage = nullptr);
    static bool Load(
        const std::filesystem::path& path,
        RenderScene& scene,
        const Asset::AssetRegistry& assetRegistry,
        std::string* outMessage = nullptr);
};
} // namespace Prism::Scene
