#include "Asset/IblEnvironmentBuilder.h"

#include "Asset/Texture.h"
#include "RHI/IGraphicsDevice.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Prism::Asset
{
using namespace DirectX;

namespace
{
constexpr std::uint32_t IrradianceResolution = 16;
constexpr std::uint32_t PrefilterResolution = 48;
constexpr std::uint32_t PrefilterCubeCount = 4;
constexpr std::uint32_t IrradianceSampleCount = 64;
constexpr std::uint32_t PrefilterSampleCount = 64;
constexpr std::uint32_t BrdfLutResolution = 128;

XMFLOAT3 ReadColor(const std::uint8_t* pixels, const std::uint32_t index)
{
    const float inv255 = 1.0f / 255.0f;
    return XMFLOAT3(
        static_cast<float>(pixels[index + 0u]) * inv255,
        static_cast<float>(pixels[index + 1u]) * inv255,
        static_cast<float>(pixels[index + 2u]) * inv255);
}

std::array<float, 3> ToArray(const XMFLOAT3& value)
{
    return {value.x, value.y, value.z};
}

XMFLOAT3 FromArray(const std::array<float, 3>& value)
{
    return XMFLOAT3(value[0], value[1], value[2]);
}

std::uint8_t ToByte(const float value)
{
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

float RadicalInverseVdc(std::uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

XMFLOAT2 Hammersley(const std::uint32_t sampleIndex, const std::uint32_t sampleCount)
{
    return XMFLOAT2(
        static_cast<float>(sampleIndex) / static_cast<float>(sampleCount),
        RadicalInverseVdc(sampleIndex));
}

XMVECTOR ImportanceSampleGgx(const XMFLOAT2& xi, const float roughness, FXMVECTOR normal)
{
    const float alpha = roughness * roughness;
    const float phi = 2.0f * XM_PI * xi.x;
    const float cosTheta = std::sqrt((1.0f - xi.y) / (1.0f + (alpha * alpha - 1.0f) * xi.y));
    const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));

    const XMVECTOR tangentSpaceHalf = XMVectorSet(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta, 0.0f);
    const XMVECTOR up = std::abs(XMVectorGetZ(normal)) < 0.999f
        ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
        : XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    const XMVECTOR tangent = XMVector3Normalize(XMVector3Cross(up, normal));
    const XMVECTOR bitangent = XMVector3Cross(normal, tangent);
    return XMVector3Normalize(
        tangent * XMVectorGetX(tangentSpaceHalf) +
        bitangent * XMVectorGetY(tangentSpaceHalf) +
        normal * XMVectorGetZ(tangentSpaceHalf));
}

float GeometrySchlickGgxIbl(const float nDotV, const float roughness)
{
    const float k = (roughness * roughness) * 0.5f;
    return nDotV / std::max(nDotV * (1.0f - k) + k, 0.0001f);
}

float GeometrySmithIbl(const float roughness, const float nDotV, const float nDotL)
{
    return GeometrySchlickGgxIbl(nDotV, roughness) * GeometrySchlickGgxIbl(nDotL, roughness);
}

XMVECTOR GetFaceDirection(const std::uint32_t faceIndex, const float u, const float v)
{
    const float x = 2.0f * u - 1.0f;
    const float y = 1.0f - 2.0f * v;
    switch (faceIndex)
    {
    case 0: return XMVector3Normalize(XMVectorSet(1.0f, y, -x, 0.0f));
    case 1: return XMVector3Normalize(XMVectorSet(-1.0f, y, x, 0.0f));
    case 2: return XMVector3Normalize(XMVectorSet(x, 1.0f, -y, 0.0f));
    case 3: return XMVector3Normalize(XMVectorSet(x, -1.0f, y, 0.0f));
    case 4: return XMVector3Normalize(XMVectorSet(x, y, 1.0f, 0.0f));
    default: return XMVector3Normalize(XMVectorSet(-x, y, -1.0f, 0.0f));
    }
}

void DirectionToCubemapUv(const XMFLOAT3& direction, std::uint32_t& faceIndex, float& u, float& v)
{
    const float absX = std::abs(direction.x);
    const float absY = std::abs(direction.y);
    const float absZ = std::abs(direction.z);

    if (absX >= absY && absX >= absZ)
    {
        if (direction.x > 0.0f)
        {
            faceIndex = 0;
            u = (-direction.z / absX + 1.0f) * 0.5f;
            v = (-direction.y / absX + 1.0f) * 0.5f;
        }
        else
        {
            faceIndex = 1;
            u = (direction.z / absX + 1.0f) * 0.5f;
            v = (-direction.y / absX + 1.0f) * 0.5f;
        }
    }
    else if (absY >= absX && absY >= absZ)
    {
        if (direction.y > 0.0f)
        {
            faceIndex = 2;
            u = (direction.x / absY + 1.0f) * 0.5f;
            v = (direction.z / absY + 1.0f) * 0.5f;
        }
        else
        {
            faceIndex = 3;
            u = (direction.x / absY + 1.0f) * 0.5f;
            v = (-direction.z / absY + 1.0f) * 0.5f;
        }
    }
    else
    {
        if (direction.z > 0.0f)
        {
            faceIndex = 4;
            u = (direction.x / absZ + 1.0f) * 0.5f;
            v = (-direction.y / absZ + 1.0f) * 0.5f;
        }
        else
        {
            faceIndex = 5;
            u = (-direction.x / absZ + 1.0f) * 0.5f;
            v = (-direction.y / absZ + 1.0f) * 0.5f;
        }
    }
}

XMFLOAT3 SampleCubemap(const Texture& source, const XMFLOAT3& direction)
{
    std::uint32_t faceIndex = 0;
    float u = 0.5f;
    float v = 0.5f;
    DirectionToCubemapUv(direction, faceIndex, u, v);

    const std::uint8_t* pixels = source.GetCubemapFacePixels(0, faceIndex);
    const std::uint32_t width = source.GetWidth();
    const std::uint32_t height = source.GetHeight();
    if (pixels == nullptr || width == 0 || height == 0)
    {
        return XMFLOAT3(0.0f, 0.0f, 0.0f);
    }

    const float fx = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(width - 1u);
    const float fy = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(height - 1u);
    const std::uint32_t x0 = static_cast<std::uint32_t>(fx);
    const std::uint32_t y0 = static_cast<std::uint32_t>(fy);
    const std::uint32_t x1 = std::min(x0 + 1u, width - 1u);
    const std::uint32_t y1 = std::min(y0 + 1u, height - 1u);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    const std::uint32_t idx00 = (y0 * width + x0) * 4u;
    const std::uint32_t idx10 = (y0 * width + x1) * 4u;
    const std::uint32_t idx01 = (y1 * width + x0) * 4u;
    const std::uint32_t idx11 = (y1 * width + x1) * 4u;
    const XMFLOAT3 c00 = ReadColor(pixels, idx00);
    const XMFLOAT3 c10 = ReadColor(pixels, idx10);
    const XMFLOAT3 c01 = ReadColor(pixels, idx01);
    const XMFLOAT3 c11 = ReadColor(pixels, idx11);

    const std::array<float, 3> a = ToArray(c00);
    const std::array<float, 3> b = ToArray(c10);
    const std::array<float, 3> c = ToArray(c01);
    const std::array<float, 3> d = ToArray(c11);
    std::array<float, 3> result{};
    for (int channel = 0; channel < 3; ++channel)
    {
        const float top = std::lerp(a[channel], b[channel], tx);
        const float bottom = std::lerp(c[channel], d[channel], tx);
        result[channel] = std::lerp(top, bottom, ty);
    }
    return FromArray(result);
}

std::vector<std::uint8_t> BakeIrradianceFace(const Texture& source, const std::uint32_t faceIndex)
{
    std::vector<std::uint8_t> pixels(IrradianceResolution * IrradianceResolution * 4u, 0u);
    for (std::uint32_t y = 0; y < IrradianceResolution; ++y)
    {
        for (std::uint32_t x = 0; x < IrradianceResolution; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(IrradianceResolution);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(IrradianceResolution);
            const XMVECTOR normal = GetFaceDirection(faceIndex, u, v);
            XMFLOAT3 accum{0.0f, 0.0f, 0.0f};

            for (std::uint32_t sampleIndex = 0; sampleIndex < IrradianceSampleCount; ++sampleIndex)
            {
                const XMFLOAT2 xi = Hammersley(sampleIndex, IrradianceSampleCount);
                const float phi = 2.0f * XM_PI * xi.x;
                const float cosTheta = std::sqrt(1.0f - xi.y);
                const float sinTheta = std::sqrt(xi.y);

                const XMVECTOR tangentSpace = XMVectorSet(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta, 0.0f);
                const XMVECTOR up = std::abs(XMVectorGetZ(normal)) < 0.999f
                    ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
                    : XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
                const XMVECTOR tangent = XMVector3Normalize(XMVector3Cross(up, normal));
                const XMVECTOR bitangent = XMVector3Cross(normal, tangent);
                const XMVECTOR sampleVector = XMVector3Normalize(
                    tangent * XMVectorGetX(tangentSpace) +
                    bitangent * XMVectorGetY(tangentSpace) +
                    normal * XMVectorGetZ(tangentSpace));

                XMFLOAT3 sampleDir{};
                XMStoreFloat3(&sampleDir, sampleVector);
                const XMFLOAT3 sampleColor = SampleCubemap(source, sampleDir);
                const float weight = std::max(0.0f, XMVectorGetX(XMVector3Dot(normal, sampleVector)));
                accum.x += sampleColor.x * weight;
                accum.y += sampleColor.y * weight;
                accum.z += sampleColor.z * weight;
            }

            const float normalization = 1.0f / static_cast<float>(IrradianceSampleCount);
            const std::size_t pixelIndex = (static_cast<std::size_t>(y) * IrradianceResolution + x) * 4u;
            pixels[pixelIndex + 0u] = ToByte(accum.x * normalization);
            pixels[pixelIndex + 1u] = ToByte(accum.y * normalization);
            pixels[pixelIndex + 2u] = ToByte(accum.z * normalization);
            pixels[pixelIndex + 3u] = 255u;
        }
    }
    return pixels;
}

std::vector<std::uint8_t> BakePrefilterFace(const Texture& source, const std::uint32_t faceIndex, const float roughness)
{
    std::vector<std::uint8_t> pixels(PrefilterResolution * PrefilterResolution * 4u, 0u);
    for (std::uint32_t y = 0; y < PrefilterResolution; ++y)
    {
        for (std::uint32_t x = 0; x < PrefilterResolution; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(PrefilterResolution);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(PrefilterResolution);
            const XMVECTOR reflection = GetFaceDirection(faceIndex, u, v);
            XMFLOAT3 accum{0.0f, 0.0f, 0.0f};
            float totalWeight = 0.0f;

            for (std::uint32_t sampleIndex = 0; sampleIndex < PrefilterSampleCount; ++sampleIndex)
            {
                const XMFLOAT2 xi = Hammersley(sampleIndex, PrefilterSampleCount);
                const XMVECTOR halfVector = ImportanceSampleGgx(xi, std::max(roughness, 0.04f), reflection);
                const float reflectionDotHalf = XMVectorGetX(XMVector3Dot(reflection, halfVector));
                const XMVECTOR light = XMVector3Normalize(2.0f * reflectionDotHalf * halfVector - reflection);
                const float nDotL = std::max(0.0f, XMVectorGetX(XMVector3Dot(reflection, light)));
                if (nDotL <= 0.0f)
                {
                    continue;
                }

                XMFLOAT3 sampleDir{};
                XMStoreFloat3(&sampleDir, light);
                const XMFLOAT3 sampleColor = SampleCubemap(source, sampleDir);
                accum.x += sampleColor.x * nDotL;
                accum.y += sampleColor.y * nDotL;
                accum.z += sampleColor.z * nDotL;
                totalWeight += nDotL;
            }

            const float invWeight = totalWeight > 0.0f ? 1.0f / totalWeight : 0.0f;
            const std::size_t pixelIndex = (static_cast<std::size_t>(y) * PrefilterResolution + x) * 4u;
            pixels[pixelIndex + 0u] = ToByte(accum.x * invWeight);
            pixels[pixelIndex + 1u] = ToByte(accum.y * invWeight);
            pixels[pixelIndex + 2u] = ToByte(accum.z * invWeight);
            pixels[pixelIndex + 3u] = 255u;
        }
    }
    return pixels;
}

std::vector<std::uint8_t> GenerateBrdfLutPixels()
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(BrdfLutResolution) * BrdfLutResolution * 4u, 0u);
    const XMVECTOR normal = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

    for (std::uint32_t y = 0; y < BrdfLutResolution; ++y)
    {
        const float roughness = std::max((static_cast<float>(y) + 0.5f) / static_cast<float>(BrdfLutResolution), 0.04f);
        for (std::uint32_t x = 0; x < BrdfLutResolution; ++x)
        {
            const float nDotV = std::max((static_cast<float>(x) + 0.5f) / static_cast<float>(BrdfLutResolution), 0.0001f);
            const XMVECTOR view = XMVectorSet(std::sqrt(std::max(0.0f, 1.0f - nDotV * nDotV)), 0.0f, nDotV, 0.0f);
            float scale = 0.0f;
            float bias = 0.0f;

            for (std::uint32_t sampleIndex = 0; sampleIndex < PrefilterSampleCount; ++sampleIndex)
            {
                const XMFLOAT2 xi = Hammersley(sampleIndex, PrefilterSampleCount);
                const XMVECTOR halfVector = ImportanceSampleGgx(xi, roughness, normal);
                const float viewDotHalf = XMVectorGetX(XMVector3Dot(view, halfVector));
                const XMVECTOR light = XMVector3Normalize(2.0f * viewDotHalf * halfVector - view);
                const float nDotL = std::max(0.0f, XMVectorGetZ(light));
                const float nDotH = std::max(0.0f, XMVectorGetZ(halfVector));
                const float vDotH = std::max(0.0f, viewDotHalf);
                if (nDotL <= 0.0f)
                {
                    continue;
                }

                const float geometry = GeometrySmithIbl(roughness, nDotV, nDotL);
                const float visibility = (geometry * vDotH) / std::max(nDotH * nDotV, 0.0001f);
                const float fresnel = std::pow(1.0f - vDotH, 5.0f);
                scale += (1.0f - fresnel) * visibility;
                bias += fresnel * visibility;
            }

            scale /= static_cast<float>(PrefilterSampleCount);
            bias /= static_cast<float>(PrefilterSampleCount);
            const std::size_t pixelIndex = (static_cast<std::size_t>(y) * BrdfLutResolution + x) * 4u;
            pixels[pixelIndex + 0u] = ToByte(scale);
            pixels[pixelIndex + 1u] = ToByte(bias);
            pixels[pixelIndex + 2u] = 0u;
            pixels[pixelIndex + 3u] = 255u;
        }
    }

    return pixels;
}
} // namespace


IblEnvironmentBuilder::Resources IblEnvironmentBuilder::Build(
    RHI::IGraphicsDevice& device,
    const Texture& sourceEnvironment) const
{
    Resources resources{};
    if (!sourceEnvironment.IsCubemap() || !sourceEnvironment.HasCpuPixels())
    {
        resources.statusMessage = "IBL bake skipped because environment cubemap CPU data is unavailable.";
        return resources;
    }

    std::array<std::vector<std::uint8_t>, 6> irradianceFaces{};
    std::array<const std::uint8_t*, 6> irradianceFacePtrs{};
    for (std::uint32_t faceIndex = 0; faceIndex < 6u; ++faceIndex)
    {
        irradianceFaces[faceIndex] = BakeIrradianceFace(sourceEnvironment, faceIndex);
        irradianceFacePtrs[faceIndex] = irradianceFaces[faceIndex].data();
    }
    resources.irradianceCubemap = std::make_shared<Texture>();
    resources.irradianceCubemap->InitializeCubemapRgba8(
        device, IrradianceResolution, IrradianceResolution, irradianceFacePtrs);

    std::array<std::vector<std::uint8_t>, PrefilterCubeCount * 6u> prefilteredFaces{};
    std::vector<const std::uint8_t*> prefilteredFacePtrs(PrefilterCubeCount * 6u, nullptr);
    for (std::uint32_t cubeIndex = 0; cubeIndex < PrefilterCubeCount; ++cubeIndex)
    {
        const float roughness = PrefilterCubeCount > 1u
            ? static_cast<float>(cubeIndex) / static_cast<float>(PrefilterCubeCount - 1u)
            : 0.0f;
        for (std::uint32_t faceIndex = 0; faceIndex < 6u; ++faceIndex)
        {
            const std::size_t arrayIndex = static_cast<std::size_t>(cubeIndex) * 6u + faceIndex;
            prefilteredFaces[arrayIndex] = BakePrefilterFace(
                sourceEnvironment, faceIndex, roughness);
            prefilteredFacePtrs[arrayIndex] = prefilteredFaces[arrayIndex].data();
        }
    }
    resources.prefilteredSpecularCubemapArray = std::make_shared<Texture>();
    resources.prefilteredSpecularCubemapArray->InitializeCubemapArrayRgba8(
        device,
        PrefilterResolution,
        PrefilterResolution,
        PrefilterCubeCount,
        prefilteredFacePtrs);

    const std::vector<std::uint8_t> brdfLutPixels = GenerateBrdfLutPixels();
    resources.brdfLutTexture = std::make_shared<Texture>();
    resources.brdfLutTexture->InitializeRgba8(
        device, BrdfLutResolution, BrdfLutResolution, brdfLutPixels.data());
    resources.statusMessage =
        "Shared RHI IBL resources baked: irradiance + prefiltered specular + BRDF LUT.";
    return resources;
}
} // namespace Prism::Asset
