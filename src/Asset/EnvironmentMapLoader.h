#pragma once

#include <memory>
#include <string>

namespace Prism::Asset
{
class Texture;
}

namespace Prism::RHI
{
class IGraphicsDevice;
}

namespace Prism::Asset
{
class EnvironmentMapLoader
{
public:
    std::shared_ptr<Texture> LoadCubemapFromDirectory(
        RHI::IGraphicsDevice& device,
        const std::string& directoryPath,
        std::string* outStatusMessage = nullptr) const;
    std::shared_ptr<Texture> CreateFallbackCubemap(RHI::IGraphicsDevice& device) const;
};
} // namespace Prism::Asset
