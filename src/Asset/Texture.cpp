#include "Asset/Texture.h"

#include "Core/Assert.h"
#include "RHI/IGraphicsDevice.h"

#include <algorithm>
#include <array>
#include <vector>


namespace Prism::Asset
{
namespace
{
std::uint8_t ToByte(const float value)
{
    const float saturated = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(saturated * 255.0f + 0.5f);
}
} // namespace


void Texture::InitializeSolidColor(RHI::IGraphicsDevice& device, const DirectX::XMFLOAT4& color)
{
    const std::array<std::uint8_t, 4> pixel = {
        ToByte(color.x),
        ToByte(color.y),
        ToByte(color.z),
        ToByte(color.w),
    };
    InitializeRgba8(device, 1, 1, pixel.data());
}

void Texture::InitializeCubemapSolidColors(
    RHI::IGraphicsDevice& device,
    const DirectX::XMFLOAT4 faceColors[6])
{
    std::array<std::array<std::uint8_t, 4>, 6> pixels{};
    std::array<const std::uint8_t*, 6> facePixels{};
    for (std::size_t face = 0; face < pixels.size(); ++face)
    {
        pixels[face] = {
            ToByte(faceColors[face].x),
            ToByte(faceColors[face].y),
            ToByte(faceColors[face].z),
            ToByte(faceColors[face].w)};
        facePixels[face] = pixels[face].data();
    }
    InitializeCubemapRgba8(device, 1, 1, facePixels);
}

void Texture::InitializeRgba8(
    RHI::IGraphicsDevice& device,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint8_t* pixels)
{
    Core::Check(width > 0 && height > 0, "RHI RGBA8 textures require non-zero dimensions.");
    Core::Check(pixels != nullptr, "RHI RGBA8 textures require pixel data.");
    StoreCpuPixels(width, height, false, 0, std::vector<std::uint8_t>(pixels, pixels + static_cast<std::size_t>(width) * height * 4u));
    RHI::TextureDescription description{};
    description.width = width;
    description.height = height;
    description.format = RHI::Format::Rgba8Unorm;
    description.usage = RHI::TextureUsage::ShaderResource | RHI::TextureUsage::CopyDestination;
    RHI::TextureInitialData initialData{};
    initialData.data = pixels;
    initialData.rowPitch = static_cast<std::size_t>(width) * 4u;
    initialData.slicePitch = initialData.rowPitch * height;
    m_rhiTexture = device.CreateTexture(description, &initialData);
}

void Texture::InitializeCubemapRgba8(
    RHI::IGraphicsDevice& device,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::array<const std::uint8_t*, 6>& facePixels)
{
    Core::Check(width > 0 && height > 0, "RHI cubemaps require non-zero dimensions.");
    const std::size_t faceSize = static_cast<std::size_t>(width) * height * 4u;
    std::vector<std::uint8_t> pixels(faceSize * facePixels.size());
    for (std::size_t face = 0; face < facePixels.size(); ++face)
    {
        Core::Check(facePixels[face] != nullptr, "RHI cubemap faces require pixel data.");
        std::copy(facePixels[face], facePixels[face] + faceSize, pixels.begin() + face * faceSize);
    }
    StoreCpuPixels(width, height, true, 1, pixels);
    RHI::TextureDescription description{};
    description.dimension = RHI::TextureDimension::TextureCube;
    description.width = width;
    description.height = height;
    description.arrayLayers = 6;
    description.format = RHI::Format::Rgba8Unorm;
    description.usage = RHI::TextureUsage::ShaderResource | RHI::TextureUsage::CopyDestination;
    RHI::TextureInitialData initialData{};
    initialData.data = m_cpuPixels.data();
    initialData.rowPitch = static_cast<std::size_t>(width) * 4u;
    initialData.slicePitch = initialData.rowPitch * height;
    m_rhiTexture = device.CreateTexture(description, &initialData);
}

void Texture::InitializeCubemapArrayRgba8(
    RHI::IGraphicsDevice& device,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t cubeCount,
    const std::vector<const std::uint8_t*>& facePixels)
{
    Core::Check(width > 0 && height > 0, "RHI cubemap arrays require non-zero dimensions.");
    Core::Check(cubeCount > 0, "RHI cubemap arrays require at least one cube.");
    Core::Check(facePixels.size() == static_cast<std::size_t>(cubeCount) * 6u,
                "RHI cubemap array face count must match its cube count.");
    const std::size_t faceSize = static_cast<std::size_t>(width) * height * 4u;
    std::vector<std::uint8_t> pixels(faceSize * facePixels.size());
    for (std::size_t face = 0; face < facePixels.size(); ++face)
    {
        Core::Check(facePixels[face] != nullptr, "RHI cubemap array faces require pixel data.");
        std::copy(
            facePixels[face],
            facePixels[face] + faceSize,
            pixels.begin() + static_cast<std::ptrdiff_t>(face * faceSize));
    }
    StoreCpuPixels(width, height, true, cubeCount, pixels);

    RHI::TextureDescription description{};
    description.dimension = RHI::TextureDimension::TextureCube;
    description.width = width;
    description.height = height;
    description.arrayLayers = cubeCount * 6u;
    description.format = RHI::Format::Rgba8Unorm;
    description.usage = RHI::TextureUsage::ShaderResource | RHI::TextureUsage::CopyDestination;
    RHI::TextureInitialData initialData{};
    initialData.data = m_cpuPixels.data();
    initialData.rowPitch = static_cast<std::size_t>(width) * 4u;
    initialData.slicePitch = initialData.rowPitch * height;
    m_rhiTexture = device.CreateTexture(description, &initialData);
}


bool Texture::HasCpuPixels() const
{
    return !m_cpuPixels.empty();
}

bool Texture::IsCubemap() const
{
    return m_isCubemap;
}

bool Texture::IsCubemapArray() const
{
    return m_isCubemapArray;
}

std::uint32_t Texture::GetWidth() const
{
    return m_width;
}

std::uint32_t Texture::GetHeight() const
{
    return m_height;
}

std::uint32_t Texture::GetCubeCount() const
{
    return m_cubeCount;
}

const std::uint8_t* Texture::GetCubemapFacePixels(const std::uint32_t cubeIndex, const std::uint32_t faceIndex) const
{
    if (!m_isCubemap || cubeIndex >= m_cubeCount || faceIndex >= 6u || m_cpuPixels.empty())
    {
        return nullptr;
    }

    const std::size_t faceSize = static_cast<std::size_t>(m_width) * m_height * 4u;
    const std::size_t offset = (static_cast<std::size_t>(cubeIndex) * 6u + faceIndex) * faceSize;
    return m_cpuPixels.data() + offset;
}

const std::shared_ptr<RHI::ITexture>& Texture::GetRhiTexture() const
{
    return m_rhiTexture;
}

void Texture::SetDebugName(
    const std::string_view name)
{
    if (m_rhiTexture != nullptr)
    {
        m_rhiTexture->SetDebugName(name);
    }
}


void Texture::StoreCpuPixels(
    const std::uint32_t width,
    const std::uint32_t height,
    const bool isCubemap,
    const std::uint32_t cubeCount,
    const std::vector<std::uint8_t>& pixels)
{
    m_width = width;
    m_height = height;
    m_isCubemap = isCubemap;
    m_isCubemapArray = isCubemap && cubeCount > 1u;
    m_cubeCount = isCubemap ? std::max(cubeCount, 1u) : 0u;
    m_cpuPixels = pixels;
}
} // namespace Prism::Asset
