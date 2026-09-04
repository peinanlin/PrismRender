#pragma once

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Prism::RHI
{
class IBuffer;
class IGraphicsDevice;
class ITexture;
class ITextureView;
}

namespace Prism::Renderer
{
struct VirtualTextureStatistics
{
    std::uint32_t residentPages = 0;
    std::uint32_t requestedPages = 0;
    std::uint32_t pageHits = 0;
    std::uint32_t pageMisses = 0;
    std::uint32_t evictions = 0;
    std::uint64_t generation = 0;
};

// Portable software virtual texture baseline. A virtual page table addresses
// a smaller physical atlas, while a CPU LRU selects the pages around the
// camera. Both D3D12 and Vulkan sample the same page-table representation.
class VirtualTextureCache
{
public:
    static constexpr std::uint32_t VirtualPageCount = 16;
    static constexpr std::uint32_t PhysicalPageGrid = 8;
    static constexpr std::uint32_t PageSize = 64;
    static constexpr std::uint32_t AtlasSize =
        PhysicalPageGrid * PageSize;
    static constexpr std::uint32_t PhysicalPageCapacity =
        PhysicalPageGrid * PhysicalPageGrid;

    void Initialize(
        RHI::IGraphicsDevice& device,
        std::uint32_t framesInFlight);
    bool Update(
        std::uint32_t frameIndex,
        const DirectX::XMFLOAT3& cameraPosition,
        float terrainWorldSize,
        bool enabled);
    void RequestReset() noexcept;

    [[nodiscard]] const std::shared_ptr<RHI::ITexture>&
        GetAtlasTexture() const;
    [[nodiscard]] const std::shared_ptr<RHI::ITextureView>&
        GetAtlasSampledView() const;
    [[nodiscard]] const std::shared_ptr<RHI::IBuffer>&
        GetPageTableBuffer() const;
    [[nodiscard]] const VirtualTextureStatistics&
        GetStatistics() const;
    [[nodiscard]] bool IsInitialized() const;
    [[nodiscard]] bool IsResetPending() const noexcept
    {
        return m_resetPending;
    }
    [[nodiscard]] std::size_t RetiredAtlasCount() const noexcept;

private:
    struct PhysicalPage
    {
        std::uint32_t virtualPage = 0xffffffffu;
        std::uint64_t lastUsedFrame = 0;
    };

    void PopulateInitialPages();
    void RebuildAtlasTexture(std::uint32_t frameIndex);
    void FillPhysicalPage(
        std::uint32_t physicalPage,
        std::uint32_t virtualPage);

    RHI::IGraphicsDevice* m_device = nullptr;
    std::shared_ptr<RHI::ITexture> m_atlasTexture;
    std::shared_ptr<RHI::ITextureView> m_atlasSampledView;
    std::shared_ptr<RHI::IBuffer> m_pageTableBuffer;
    std::array<PhysicalPage, PhysicalPageCapacity> m_physicalPages{};
    std::vector<std::uint32_t> m_pageTable;
    std::vector<std::uint8_t> m_atlasPixels;
    struct RetiredAtlas
    {
        std::shared_ptr<RHI::ITexture> texture;
        std::shared_ptr<RHI::ITextureView> sampledView;
    };
    std::vector<RetiredAtlas> m_retiredAtlases;
    VirtualTextureStatistics m_statistics{};
    std::uint64_t m_frameNumber = 0;
    bool m_resetPending = false;
};
} // namespace Prism::Renderer
