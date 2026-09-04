#include "UI/ImGuiSystem.h"

#include "Core/Assert.h"
#include "Platform/Window.h"
#include "UI/IImGuiRenderer.h"
#include "UI/UiDrawPacket.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>

#include <utility>

namespace Prism::UI
{
ImGuiSystem::ImGuiSystem(
    std::unique_ptr<IImGuiRenderer> renderer)
    : m_renderer(std::move(renderer))
{
    Core::Check(
        m_renderer != nullptr,
        "ImGuiSystem requires an API renderer.");
}

ImGuiSystem::~ImGuiSystem()
{
    Shutdown();
}

void ImGuiSystem::Initialize(
    Platform::Window& window,
    RHI::IRenderBackend& backend)
{
    const std::lock_guard lock(m_backendMutex);
    if (m_initialized)
    {
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |=
        ImGuiConfigFlags_NavEnableKeyboard
        | ImGuiConfigFlags_DockingEnable;
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 3.0f;
    style.FrameRounding = 3.0f;

    Core::Check(
        ImGui_ImplGlfw_InitForOther(
            window.GetNativeWindow(),
            true),
        "Failed to initialize the ImGui GLFW backend.");
    m_renderer->Initialize(backend);
    m_initialized = true;
}

void ImGuiSystem::Shutdown()
{
    const std::lock_guard lock(m_backendMutex);
    if (!m_initialized)
    {
        return;
    }

    if (m_sceneTextureId != 0)
    {
        m_renderer->UnregisterTexture(m_sceneTextureId);
        m_sceneTextureId = 0;
        m_sceneTextureView.reset();
    }
    if (m_gameTextureId != 0)
    {
        m_renderer->UnregisterTexture(m_gameTextureId);
        m_gameTextureId = 0;
        m_gameTextureView.reset();
    }
    m_renderer->Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
}

void ImGuiSystem::BeginFrame()
{
    m_backendMutex.lock();
    try
    {
        Core::Check(
            m_initialized && !m_frameBuildActive,
            "The ImGui system cannot begin a nested frame build.");
        m_renderer->BeginFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        m_frameBuildActive = true;
    }
    catch (...)
    {
        m_backendMutex.unlock();
        throw;
    }
}

void ImGuiSystem::PollPlatformEvents(Platform::Window& window)
{
    const std::lock_guard lock(m_backendMutex);
    Core::Check(
        m_initialized && !m_frameBuildActive,
        "Platform events cannot be polled during an ImGui frame build.");
    window.PollEvents();
}

void ImGuiSystem::AbortFrameBuild() noexcept
{
    if (!m_frameBuildActive)
    {
        return;
    }
    m_frameBuildActive = false;
    m_backendMutex.unlock();
}

void ImGuiSystem::SetViewportTextures(
    std::shared_ptr<const RHI::ITextureView> sceneTextureView,
    std::shared_ptr<const RHI::ITextureView> gameTextureView)
{
    const std::lock_guard lock(m_backendMutex);
    ReplaceTexture(
        std::move(sceneTextureView),
        m_sceneTextureId,
        m_sceneTextureView);
    ReplaceTexture(
        std::move(gameTextureView),
        m_gameTextureId,
        m_gameTextureView);
}

std::uint64_t ImGuiSystem::GetSceneTextureId() const
{
    return m_sceneTextureId;
}

std::uint64_t ImGuiSystem::GetGameTextureId() const
{
    return m_gameTextureId;
}

std::shared_ptr<const UiDrawPacket>
ImGuiSystem::BuildDrawPacket()
{
    std::unique_lock lock(
        m_backendMutex,
        std::adopt_lock);
    Core::Check(
        m_initialized && m_frameBuildActive,
        "The ImGui system has no active frame build.");
    try
    {
        ImGui::Render();
        const UiTextureLease leases[] = {
            {m_sceneTextureId, m_sceneTextureView},
            {m_gameTextureId, m_gameTextureView}};
        std::shared_ptr<const UiDrawPacket> packet =
            UiDrawPacket::Create(
                *ImGui::GetDrawData(),
                leases);
        m_frameBuildActive = false;
        return packet;
    }
    catch (...)
    {
        m_frameBuildActive = false;
        throw;
    }
}

void ImGuiSystem::Render(const UiDrawPacket& drawPacket)
{
    const std::lock_guard lock(m_backendMutex);
    Core::Check(
        m_initialized,
        "The ImGui system is not initialized.");
    m_renderer->RenderDrawData(drawPacket);
}

void ImGuiSystem::ReplaceTexture(
    std::shared_ptr<const RHI::ITextureView> textureView,
    std::uint64_t& textureId,
    std::shared_ptr<const RHI::ITextureView>& retainedView)
{
    Core::Check(
        m_initialized,
        "The ImGui system is not initialized.");
    Core::Check(
        textureView != nullptr,
        "The ImGui system requires a retained texture view.");
    if (textureId != 0)
    {
        m_renderer->UnregisterTexture(textureId);
    }
    textureId = m_renderer->RegisterTexture(*textureView);
    retainedView = std::move(textureView);
    Core::Check(
        textureId != 0,
        "The ImGui renderer returned an invalid texture identifier.");
}
} // namespace Prism::UI
