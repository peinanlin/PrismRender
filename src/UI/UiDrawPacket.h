#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

struct ImDrawData;

namespace Prism::RHI
{
class ITextureView;
}

namespace Prism::UI
{
struct UiTextureLease
{
    std::uint64_t textureId = 0;
    std::shared_ptr<const RHI::ITextureView> textureView;
};

// Immutable copy of one completed ImGui build. It owns cloned command lists
// and retains every viewport texture referenced by the frame, allowing the
// backend draw to run without reading the mutable ImGui context.
class UiDrawPacket final
{
public:
    static std::shared_ptr<const UiDrawPacket> Create(
        const ImDrawData& drawData,
        std::span<const UiTextureLease> textureLeases);

    ~UiDrawPacket();
    UiDrawPacket(const UiDrawPacket&) = delete;
    UiDrawPacket& operator=(const UiDrawPacket&) = delete;

    [[nodiscard]] ImDrawData& GetDrawData() const;
    [[nodiscard]] std::size_t GetDrawListCount() const noexcept;
    [[nodiscard]] std::size_t GetVertexCount() const noexcept;
    [[nodiscard]] std::size_t GetIndexCount() const noexcept;
    [[nodiscard]] std::size_t GetCommandCount() const noexcept;
    [[nodiscard]] const std::vector<UiTextureLease>&
        GetTextureLeases() const noexcept;

private:
    struct Storage;
    explicit UiDrawPacket(std::unique_ptr<Storage> storage);

    std::unique_ptr<Storage> m_storage;
};
} // namespace Prism::UI
