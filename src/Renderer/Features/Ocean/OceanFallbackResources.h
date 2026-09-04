#pragma once

#include <memory>

namespace Prism::RHI
{
class IGraphicsDevice;
class ITextureView;
}

namespace Prism::Renderer
{
// Valid, initialized array descriptors for shaders whose ocean branch is disabled.
// These never replace the simulation's published maps while spectral ocean is enabled.
class OceanFallbackResources
{
public:
    void Initialize(RHI::IGraphicsDevice& device);
    const std::shared_ptr<RHI::ITextureView>& ZeroArray() const { return m_zeroArray; }
    const std::shared_ptr<RHI::ITextureView>& UpNormalArray() const { return m_upNormalArray; }

private:
    std::shared_ptr<RHI::ITextureView> m_zeroArray;
    std::shared_ptr<RHI::ITextureView> m_upNormalArray;
};
}
