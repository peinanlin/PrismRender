#include "Renderer/Features/VirtualTextureCache.h"

#include "Core/Assert.h"
#include "RHI/IGraphicsDevice.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Prism::Renderer
{
namespace
{
float Hash(const std::uint32_t x, const std::uint32_t y)
{
    std::uint32_t value = x * 0x8da6b343u
        ^ y * 0xd8163841u ^ 0xcb1ab31fu;
    value ^= value >> 13u;
    value *= 0x85ebca6bu;
    value ^= value >> 16u;
    return static_cast<float>(value & 0xffffu) / 65535.0f;
}

std::uint8_t ToByte(const float value)
{
    return static_cast<std::uint8_t>(
        std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}
} // namespace

void VirtualTextureCache::Initialize(
    RHI::IGraphicsDevice& device,
    const std::uint32_t framesInFlight)
{
    Core::Check(
        framesInFlight > 0u,
        "Virtual texture cache requires at least one frame slot.");
    m_device = &device;
    m_retiredAtlases.assign(framesInFlight, {});
    m_pageTable.assign(
        VirtualPageCount * VirtualPageCount,
        0u);
    m_atlasPixels.assign(
        static_cast<std::size_t>(AtlasSize)
            * AtlasSize * 4u,
        0u);

    RHI::BufferDescription pageTableDescription{};
    pageTableDescription.size =
        m_pageTable.size() * sizeof(std::uint32_t);
    pageTableDescription.stride = sizeof(std::uint32_t);
    // The 16x16 page table is a read-only cbuffer in the terrain shader. This
    // lets the CPU publish residency edits directly on both backends.
    pageTableDescription.usage =
        RHI::BufferUsage::Constant;
    pageTableDescription.memoryAccess =
        RHI::MemoryAccess::CpuToGpu;
    m_pageTableBuffer = device.CreateBuffer(
        pageTableDescription,
        m_pageTable.data());
    m_pageTableBuffer->SetDebugName(
        "VirtualTerrain.PageTable");

    PopulateInitialPages();
    m_pageTableBuffer->Update(
        m_pageTable.data(),
        m_pageTable.size() * sizeof(std::uint32_t));
    RebuildAtlasTexture(0u);
}

bool VirtualTextureCache::Update(
    const std::uint32_t frameIndex,
    const DirectX::XMFLOAT3& cameraPosition,
    const float terrainWorldSize,
    const bool enabled)
{
    Core::Check(
        IsInitialized() && frameIndex < m_retiredAtlases.size(),
        "Virtual texture cache is not initialized.");
    // BeginFrame has already waited for this slot's fence. Resources replaced
    // the last time this slot was used can now be released safely.
    m_retiredAtlases[frameIndex] = {};
    ++m_frameNumber;
    m_statistics.requestedPages = 0;
    m_statistics.pageHits = 0;
    m_statistics.pageMisses = 0;
    m_statistics.evictions = 0;
    bool changed = false;
    if (m_resetPending)
    {
        PopulateInitialPages();
        m_resetPending = false;
        changed = true;
    }
    if (!enabled)
    {
        if (changed)
        {
            m_pageTableBuffer->Update(
                m_pageTable.data(),
                m_pageTable.size() * sizeof(std::uint32_t));
            RebuildAtlasTexture(frameIndex);
            ++m_statistics.generation;
        }
        return changed;
    }

    const float safeWorldSize = std::max(
        terrainWorldSize,
        1.0f);
    const float normalizedX = std::clamp(
        cameraPosition.x / safeWorldSize + 0.5f,
        0.0f,
        0.99999f);
    const float normalizedY = std::clamp(
        cameraPosition.z / safeWorldSize + 0.5f,
        0.0f,
        0.99999f);
    const std::int32_t centerX = static_cast<std::int32_t>(
        normalizedX * VirtualPageCount);
    const std::int32_t centerY = static_cast<std::int32_t>(
        normalizedY * VirtualPageCount);

    struct Request
    {
        std::uint32_t page = 0;
        std::uint32_t distance = 0;
    };
    std::vector<Request> requests;
    constexpr std::int32_t Radius = 3;
    for (std::int32_t offsetY = -Radius;
         offsetY <= Radius;
         ++offsetY)
    {
        for (std::int32_t offsetX = -Radius;
             offsetX <= Radius;
             ++offsetX)
        {
            const std::int32_t pageX = centerX + offsetX;
            const std::int32_t pageY = centerY + offsetY;
            if (pageX < 0 || pageY < 0
                || pageX >= static_cast<std::int32_t>(VirtualPageCount)
                || pageY >= static_cast<std::int32_t>(VirtualPageCount))
            {
                continue;
            }
            requests.push_back({
                static_cast<std::uint32_t>(pageY) * VirtualPageCount
                    + static_cast<std::uint32_t>(pageX),
                static_cast<std::uint32_t>(
                    std::abs(offsetX) + std::abs(offsetY))});
        }
    }
    std::ranges::sort(
        requests,
        {},
        &Request::distance);
    m_statistics.requestedPages =
        static_cast<std::uint32_t>(requests.size());

    std::vector<bool> requested(
        m_pageTable.size(),
        false);
    for (const Request& request : requests)
    {
        requested[request.page] = true;
    }

    for (const Request& request : requests)
    {
        const std::uint32_t encoded =
            m_pageTable[request.page];
        if (encoded != 0u)
        {
            const std::uint32_t physicalPage =
                encoded - 1u;
            m_physicalPages[physicalPage]
                .lastUsedFrame = m_frameNumber;
            ++m_statistics.pageHits;
            continue;
        }

        ++m_statistics.pageMisses;
        std::uint32_t physicalPage =
            PhysicalPageCapacity;
        for (std::uint32_t index = 0;
             index < PhysicalPageCapacity;
             ++index)
        {
            if (m_physicalPages[index].virtualPage
                == 0xffffffffu)
            {
                physicalPage = index;
                break;
            }
        }
        if (physicalPage == PhysicalPageCapacity)
        {
            std::uint64_t oldestFrame =
                std::numeric_limits<std::uint64_t>::max();
            for (std::uint32_t index = 0;
                 index < PhysicalPageCapacity;
                 ++index)
            {
                const std::uint32_t resident =
                    m_physicalPages[index].virtualPage;
                if (resident < requested.size()
                    && requested[resident])
                {
                    continue;
                }
                if (m_physicalPages[index].lastUsedFrame
                    < oldestFrame)
                {
                    oldestFrame =
                        m_physicalPages[index].lastUsedFrame;
                    physicalPage = index;
                }
            }
        }
        if (physicalPage == PhysicalPageCapacity)
        {
            continue;
        }

        PhysicalPage& slot =
            m_physicalPages[physicalPage];
        if (slot.virtualPage != 0xffffffffu)
        {
            m_pageTable[slot.virtualPage] = 0u;
            ++m_statistics.evictions;
        }
        slot.virtualPage = request.page;
        slot.lastUsedFrame = m_frameNumber;
        m_pageTable[request.page] = physicalPage + 1u;
        FillPhysicalPage(physicalPage, request.page);
        changed = true;
    }

    m_statistics.residentPages = 0;
    for (const PhysicalPage& page : m_physicalPages)
    {
        if (page.virtualPage != 0xffffffffu)
        {
            ++m_statistics.residentPages;
        }
    }
    if (changed)
    {
        m_pageTableBuffer->Update(
            m_pageTable.data(),
            m_pageTable.size()
                * sizeof(std::uint32_t));
        RebuildAtlasTexture(frameIndex);
        ++m_statistics.generation;
    }
    return changed;
}

void VirtualTextureCache::RequestReset() noexcept
{
    m_resetPending = true;
}

const std::shared_ptr<RHI::ITexture>&
VirtualTextureCache::GetAtlasTexture() const
{
    return m_atlasTexture;
}

const std::shared_ptr<RHI::ITextureView>&
VirtualTextureCache::GetAtlasSampledView() const
{
    return m_atlasSampledView;
}

const std::shared_ptr<RHI::IBuffer>&
VirtualTextureCache::GetPageTableBuffer() const
{
    return m_pageTableBuffer;
}

const VirtualTextureStatistics&
VirtualTextureCache::GetStatistics() const
{
    return m_statistics;
}

bool VirtualTextureCache::IsInitialized() const
{
    return m_device != nullptr
        && m_atlasTexture != nullptr
        && m_pageTableBuffer != nullptr;
}

std::size_t VirtualTextureCache::RetiredAtlasCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(
        m_retiredAtlases.begin(),
        m_retiredAtlases.end(),
        [](const RetiredAtlas& atlas)
        { return atlas.texture != nullptr || atlas.sampledView != nullptr; }));
}

void VirtualTextureCache::PopulateInitialPages()
{
    // Keep one valid page before a terrain camera has been published, so all
    // descriptor sets can bind initialized resources.
    std::ranges::fill(m_physicalPages, PhysicalPage{});
    std::ranges::fill(m_pageTable, 0u);
    std::ranges::fill(m_atlasPixels, 0u);
    m_physicalPages[0] = {0u, m_frameNumber};
    m_pageTable[0] = 1u;
    FillPhysicalPage(0u, 0u);
    m_statistics.residentPages = 1;
}

void VirtualTextureCache::RebuildAtlasTexture(
    const std::uint32_t frameIndex)
{
    Core::Check(
        frameIndex < m_retiredAtlases.size(),
        "Virtual texture atlas retirement uses an invalid frame slot.");
    if (m_atlasTexture != nullptr || m_atlasSampledView != nullptr)
    {
        m_retiredAtlases[frameIndex] = {
            std::move(m_atlasTexture),
            std::move(m_atlasSampledView)};
    }
    RHI::TextureDescription description{};
    description.width = AtlasSize;
    description.height = AtlasSize;
    description.format = RHI::Format::Rgba8Unorm;
    description.usage = RHI::TextureUsage::ShaderResource;
    RHI::TextureInitialData initialData{};
    initialData.data = m_atlasPixels.data();
    initialData.rowPitch =
        static_cast<std::size_t>(AtlasSize) * 4u;
    initialData.slicePitch =
        initialData.rowPitch * AtlasSize;
    m_atlasTexture = m_device->CreateTexture(
        description,
        &initialData);
    m_atlasTexture->SetDebugName(
        "VirtualTerrain.PhysicalAtlas");
    RHI::TextureViewDescription sampled{};
    sampled.type = RHI::TextureViewType::Sampled;
    m_atlasSampledView =
        m_device->CreateTextureView(
            m_atlasTexture,
            sampled);
}

void VirtualTextureCache::FillPhysicalPage(
    const std::uint32_t physicalPage,
    const std::uint32_t virtualPage)
{
    const std::uint32_t physicalX =
        physicalPage % PhysicalPageGrid;
    const std::uint32_t physicalY =
        physicalPage / PhysicalPageGrid;
    const std::uint32_t virtualX =
        virtualPage % VirtualPageCount;
    const std::uint32_t virtualY =
        virtualPage / VirtualPageCount;
    for (std::uint32_t y = 0; y < PageSize; ++y)
    {
        for (std::uint32_t x = 0; x < PageSize; ++x)
        {
            const float noise = Hash(
                virtualX * PageSize + x,
                virtualY * PageSize + y);
            const float broad = Hash(
                virtualX * 7u + x / 16u,
                virtualY * 11u + y / 16u);
            const float moss =
                0.65f * noise + 0.35f * broad;
            const float path = std::abs(
                std::sin(
                    (static_cast<float>(virtualX * PageSize + x)
                     + static_cast<float>(virtualY * PageSize + y) * 0.37f)
                    * 0.035f));
            const float pathMask =
                std::clamp((path - 0.72f) * 5.0f, 0.0f, 1.0f);
            const float red = std::lerp(
                0.12f + moss * 0.10f,
                0.32f,
                pathMask);
            const float green = std::lerp(
                0.28f + moss * 0.26f,
                0.25f,
                pathMask);
            const float blue = std::lerp(
                0.075f + moss * 0.07f,
                0.14f,
                pathMask);
            const std::uint32_t atlasX =
                physicalX * PageSize + x;
            const std::uint32_t atlasY =
                physicalY * PageSize + y;
            const std::size_t pixel =
                (static_cast<std::size_t>(atlasY)
                 * AtlasSize + atlasX) * 4u;
            m_atlasPixels[pixel + 0u] = ToByte(red);
            m_atlasPixels[pixel + 1u] = ToByte(green);
            m_atlasPixels[pixel + 2u] = ToByte(blue);
            m_atlasPixels[pixel + 3u] = 255u;
        }
    }
}
} // namespace Prism::Renderer
