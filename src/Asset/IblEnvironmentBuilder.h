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
class IblEnvironmentBuilder
{
public:
    struct Resources
    {
        std::shared_ptr<Texture> irradianceCubemap;
        std::shared_ptr<Texture> prefilteredSpecularCubemapArray;
        std::shared_ptr<Texture> brdfLutTexture;
        std::string statusMessage;
    };

    Resources Build(RHI::IGraphicsDevice& device, const Texture& sourceEnvironment) const;
};
} // namespace Prism::Asset
