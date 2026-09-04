#include "Renderer/Features/Ocean/OceanFft.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Prism::Renderer
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;
}

OceanFft::~OceanFft() = default;

bool OceanFft::Configure(const std::uint32_t size) noexcept
{
    if (std::ranges::find(SupportedSizes, size) == SupportedSizes.end())
    {
        return false;
    }
    m_size = size;
    m_stageCount = 0;
    for (std::uint32_t value = size; value > 1u; value >>= 1u)
    {
        ++m_stageCount;
    }
    m_stageConstants.clear();
    m_stageSets.clear();
    m_arrayStageConstants.clear();
    m_arrayStageSets.clear();
    return true;
}

void OceanFft::Reset() noexcept
{
    m_stageSets.clear();
    m_stageConstants.clear();
    m_texturePipeline.reset();
    m_textureLayout.reset();
    m_arrayTexturePipeline.reset();
    m_arrayRadixPipeline.reset();
    m_arrayTextureLayout.reset();
    m_device = nullptr;
    m_size = 0;
    m_stageCount = 0;
    m_arrayLayerCount = 0u;
}

std::uint32_t OceanFft::BitReverse(const std::uint32_t value) const noexcept
{
    if (m_size == 0u || value >= m_size)
    {
        return 0u;
    }
    std::uint32_t reversed = 0u;
    for (std::uint32_t bit = 0u; bit < m_stageCount; ++bit)
    {
        reversed = (reversed << 1u) | ((value >> bit) & 1u);
    }
    return reversed;
}

std::complex<float> OceanFft::Twiddle(const std::uint32_t stage,
    const std::uint32_t lane, const bool inverse) const noexcept
{
    if (stage >= m_stageCount || lane >= (1u << stage))
    {
        return {};
    }
    const std::uint32_t blockSize = 1u << (stage + 1u);
    const float sign = inverse ? 1.0f : -1.0f;
    const float angle = sign * 2.0f * Pi
        * static_cast<float>(lane)
        / static_cast<float>(blockSize);
    return {std::cos(angle), std::sin(angle)};
}

std::vector<OceanFftPass> OceanFft::BuildPasses(
    const std::uint32_t dimensions, const bool inverse,
    const std::uint32_t initialSourceIndex,
    const bool bitReverseEachDimension,
    const bool normalizeEachDimension) const
{
    if (m_size == 0u || dimensions == 0u || dimensions > 2u
        || initialSourceIndex > 1u)
    {
        return {};
    }
    std::vector<OceanFftPass> passes;
    passes.reserve(static_cast<std::size_t>(dimensions) * m_stageCount);
    std::uint32_t sourceIndex = initialSourceIndex;
    for (std::uint32_t direction = 0u; direction < dimensions; ++direction)
    {
        for (std::uint32_t stage = 0u; stage < m_stageCount; ++stage)
        {
            const std::uint32_t destinationIndex = 1u - sourceIndex;
            passes.push_back(OceanFftPass{
                stage,
                direction,
                sourceIndex,
                destinationIndex,
                inverse,
                bitReverseEachDimension && stage == 0u,
                normalizeEachDimension && inverse
                    && stage + 1u == m_stageCount});
            sourceIndex = destinationIndex;
        }
    }
    return passes;
}

bool OceanFft::Transform(std::span<std::complex<float>> values,
    const bool inverse) const noexcept
{
    if (m_size == 0u || values.size() != m_size)
    {
        return false;
    }
    for (std::uint32_t index = 0u; index < m_size; ++index)
    {
        const std::uint32_t reversed = BitReverse(index);
        if (reversed > index)
        {
            std::swap(values[index], values[reversed]);
        }
    }
    for (std::uint32_t stage = 0u; stage < m_stageCount; ++stage)
    {
        const std::uint32_t blockSize = 1u << (stage + 1u);
        const std::uint32_t halfBlock = blockSize >> 1u;
        for (std::uint32_t block = 0u; block < m_size;
            block += blockSize)
        {
            for (std::uint32_t lane = 0u; lane < halfBlock; ++lane)
            {
                const std::complex<float> even = values[block + lane];
                const std::complex<float> odd =
                    values[block + lane + halfBlock]
                    * Twiddle(stage, lane, inverse);
                values[block + lane] = even + odd;
                values[block + lane + halfBlock] = even - odd;
            }
        }
    }
    if (inverse)
    {
        const float normalization = 1.0f / static_cast<float>(m_size);
        for (std::complex<float>& value : values)
        {
            value *= normalization;
        }
    }
    return true;
}

} // namespace Prism::Renderer
