#include "Renderer/Features/Ocean/OceanFallbackResources.h"

#include "RHI/IGraphicsDevice.h"
#include "Renderer/Features/Ocean/SpectralOceanSimulation.h"

#include <array>
#include <cstdint>
#include <utility>

namespace Prism::Renderer
{
void OceanFallbackResources::Initialize(RHI::IGraphicsDevice& device)
{
    if (m_zeroArray != nullptr && m_upNormalArray != nullptr) return;
    const auto create = [&](const std::array<std::uint8_t, 4>& pixel, const char* name)
    {
        std::array<std::array<std::uint8_t, 4>, SpectralOceanSimulation::CascadeCount> pixels{};
        pixels.fill(pixel);
        RHI::TextureDescription description{};
        description.width = description.height = 1u;
        description.arrayLayers = SpectralOceanSimulation::CascadeCount;
        description.mipLevels = 1u;
        description.format = RHI::Format::Rgba8Unorm;
        description.usage = RHI::TextureUsage::ShaderResource;
        RHI::TextureInitialData data{};
        data.data = pixels.data();
        data.rowPitch = data.slicePitch = sizeof(pixel);
        auto texture = device.CreateTexture(description, &data);
        texture->SetDebugName(name);
        RHI::TextureViewDescription view{};
        view.type = RHI::TextureViewType::Sampled;
        view.arrayLayerCount = description.arrayLayers;
        // Every exposed subresource is uploaded; no undefined tail mips are sampled.
        view.mipLevelCount = description.mipLevels;
        return device.CreateTextureView(std::move(texture), view);
    };
    auto zero = create({0u, 0u, 0u, 0u}, "OceanFallback.ZeroArray");
    auto normal = create({0u, 255u, 0u, 0u}, "OceanFallback.UpNormalArray");
    m_zeroArray = std::move(zero);
    m_upNormalArray = std::move(normal);
}
}
