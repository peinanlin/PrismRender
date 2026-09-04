#include "UI/UiDrawPacket.h"

#include <imgui.h>

#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace Prism::UI
{
struct UiDrawPacket::Storage
{
    ImDrawData drawData;
    ImGuiViewport ownerViewport;
    std::vector<std::unique_ptr<ImDrawList>> drawLists;
    std::vector<UiTextureLease> textureLeases;
    std::size_t commandCount = 0;
};

std::shared_ptr<const UiDrawPacket> UiDrawPacket::Create(
    const ImDrawData& source,
    const std::span<const UiTextureLease> textureLeases)
{
    if (!source.Valid)
    {
        throw std::invalid_argument(
            "UiDrawPacket requires completed ImGui draw data.");
    }

    auto storage = std::make_unique<Storage>();
    storage->drawData.Valid = true;
    storage->drawData.DisplayPos = source.DisplayPos;
    storage->drawData.DisplaySize = source.DisplaySize;
    storage->drawData.FramebufferScale = source.FramebufferScale;
    if (source.OwnerViewport == nullptr)
    {
        throw std::invalid_argument(
            "UiDrawPacket requires an owning ImGui viewport.");
    }
    // The renderer backends use RendererUserData to select their frame-ring
    // buffers. Keep that backend-owned identity without retaining the mutable
    // ImGui viewport or context object itself.
    storage->ownerViewport.RendererUserData =
        source.OwnerViewport->RendererUserData;
    storage->drawData.OwnerViewport = &storage->ownerViewport;
    storage->drawLists.reserve(
        static_cast<std::size_t>(source.CmdListsCount));

    for (const ImDrawList* const sourceList : source.CmdLists)
    {
        if (sourceList == nullptr)
        {
            throw std::invalid_argument(
                "UiDrawPacket cannot copy a null draw list.");
        }
        for (const ImDrawCmd& command : sourceList->CmdBuffer)
        {
            if (command.UserCallback != nullptr)
            {
                throw std::invalid_argument(
                    "UiDrawPacket does not permit callbacks that could access mutable ImGui state.");
            }
        }
        std::unique_ptr<ImDrawList> clone(
            sourceList->CloneOutput());
        storage->commandCount +=
            static_cast<std::size_t>(clone->CmdBuffer.Size);
        storage->drawData.AddDrawList(clone.get());
        storage->drawLists.push_back(std::move(clone));
    }

    std::unordered_set<std::uint64_t> retainedIds;
    for (const UiTextureLease& lease : textureLeases)
    {
        if (lease.textureId == 0 || !lease.textureView)
        {
            throw std::invalid_argument(
                "Ui texture leases require an ID and retained texture view.");
        }
        if (retainedIds.insert(lease.textureId).second)
        {
            storage->textureLeases.push_back(lease);
        }
    }
    return std::shared_ptr<const UiDrawPacket>(
        new UiDrawPacket(std::move(storage)));
}

UiDrawPacket::UiDrawPacket(std::unique_ptr<Storage> storage)
    : m_storage(std::move(storage))
{
}

UiDrawPacket::~UiDrawPacket() = default;

ImDrawData& UiDrawPacket::GetDrawData() const
{
    return m_storage->drawData;
}

std::size_t UiDrawPacket::GetDrawListCount() const noexcept
{
    return m_storage->drawLists.size();
}

std::size_t UiDrawPacket::GetVertexCount() const noexcept
{
    return static_cast<std::size_t>(
        m_storage->drawData.TotalVtxCount);
}

std::size_t UiDrawPacket::GetIndexCount() const noexcept
{
    return static_cast<std::size_t>(
        m_storage->drawData.TotalIdxCount);
}

std::size_t UiDrawPacket::GetCommandCount() const noexcept
{
    return m_storage->commandCount;
}

const std::vector<UiTextureLease>&
UiDrawPacket::GetTextureLeases() const noexcept
{
    return m_storage->textureLeases;
}
} // namespace Prism::UI
