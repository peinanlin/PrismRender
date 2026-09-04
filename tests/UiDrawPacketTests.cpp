#include "RHI/GraphicsResources.h"
#include "UI/UiDrawPacket.h"

#include <imgui.h>

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
void Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class FakeTextureView final : public Prism::RHI::ITextureView
{
public:
    Prism::RHI::GraphicsApi GetGraphicsApi() const override
    {
        return Prism::RHI::GraphicsApi::Vulkan;
    }

    const Prism::RHI::TextureViewDescription&
        GetDescription() const override
    {
        return m_description;
    }

    const Prism::RHI::ITexture* GetTexture() const override
    {
        return nullptr;
    }

private:
    Prism::RHI::TextureViewDescription m_description{};
};
} // namespace

int main()
{
    try
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = {640.0f, 360.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.Fonts->AddFontDefault();
        Expect(io.Fonts->Build(),
            "ImGui test font atlas did not build.");

        auto texture = std::make_shared<FakeTextureView>();
        std::weak_ptr<FakeTextureView> retainedTexture = texture;
        ImGui::NewFrame();
        ImDrawList* const firstDrawList =
            ImGui::GetForegroundDrawList();
        firstDrawList->AddRectFilled(
            {8.0f, 8.0f},
            {72.0f, 72.0f},
            IM_COL32_WHITE);
        firstDrawList->AddImage(
            static_cast<ImTextureID>(42),
            {80.0f, 8.0f},
            {144.0f, 72.0f});
        ImGui::Render();

        std::shared_ptr<const Prism::UI::UiDrawPacket> packet =
            Prism::UI::UiDrawPacket::Create(
                *ImGui::GetDrawData(),
                std::array<Prism::UI::UiTextureLease, 1>{
                    Prism::UI::UiTextureLease{42, texture}});
        const std::size_t vertexCount = packet->GetVertexCount();
        const std::size_t indexCount = packet->GetIndexCount();
        const std::size_t commandCount = packet->GetCommandCount();
        Expect(packet->GetDrawListCount() > 0
                && vertexCount > 0
                && indexCount > 0
                && commandCount > 0
                && packet->GetTextureLeases().size() == 1,
            "UiDrawPacket did not copy completed draw data.");
        Expect(packet->GetDrawData().OwnerViewport != nullptr
                && packet->GetDrawData().OwnerViewport
                    != ImGui::GetMainViewport()
                && packet->GetDrawData().OwnerViewport
                    ->RendererUserData
                    == ImGui::GetMainViewport()->RendererUserData,
            "UiDrawPacket retained a mutable ImGui viewport instead of "
            "its backend identity.");

        texture.reset();
        Expect(!retainedTexture.expired(),
            "UiDrawPacket did not retain its viewport texture.");

        ImGui::NewFrame();
        ImGui::GetForegroundDrawList()->AddCircleFilled(
            {20.0f, 20.0f},
            10.0f,
            IM_COL32_WHITE);
        ImGui::Render();
        Expect(packet->GetVertexCount() == vertexCount
                && packet->GetIndexCount() == indexCount
                && packet->GetCommandCount() == commandCount,
            "A later ImGui build mutated retained draw data.");

        packet.reset();
        Expect(retainedTexture.expired(),
            "UiDrawPacket retained a texture after packet release.");
        ImGui::DestroyContext();
        std::cout << "UI draw packet tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        if (ImGui::GetCurrentContext() != nullptr)
        {
            ImGui::DestroyContext();
        }
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
