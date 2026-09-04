#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <DirectXMath.h>

namespace Prism::RHI
{
class IGraphicsDevice;
class ITexture;
}

namespace Prism::Asset
{
class Texture
{
public:
    void InitializeSolidColor(RHI::IGraphicsDevice& device, const DirectX::XMFLOAT4& color);
    void InitializeCubemapSolidColors(RHI::IGraphicsDevice& device, const DirectX::XMFLOAT4 faceColors[6]);
    void InitializeRgba8(RHI::IGraphicsDevice& device, std::uint32_t width, std::uint32_t height, const std::uint8_t* pixels);
    void InitializeCubemapRgba8(
        RHI::IGraphicsDevice& device,
        std::uint32_t width,
        std::uint32_t height,
        const std::array<const std::uint8_t*, 6>& facePixels);
    void InitializeCubemapArrayRgba8(
        RHI::IGraphicsDevice& device,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t cubeCount,
        const std::vector<const std::uint8_t*>& facePixels);

    bool HasCpuPixels() const;
    bool IsCubemap() const;
    bool IsCubemapArray() const;
    std::uint32_t GetWidth() const;
    std::uint32_t GetHeight() const;
    std::uint32_t GetCubeCount() const;
    const std::uint8_t* GetCubemapFacePixels(std::uint32_t cubeIndex, std::uint32_t faceIndex) const;
    const std::shared_ptr<RHI::ITexture>& GetRhiTexture() const;
    void SetDebugName(std::string_view name);

private:
    void StoreCpuPixels(std::uint32_t width, std::uint32_t height, bool isCubemap, std::uint32_t cubeCount, const std::vector<std::uint8_t>& pixels);

    std::vector<std::uint8_t> m_cpuPixels;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_cubeCount = 0;
    bool m_isCubemap = false;
    bool m_isCubemapArray = false;
    std::shared_ptr<RHI::ITexture> m_rhiTexture;
};
} // namespace Prism::Asset
