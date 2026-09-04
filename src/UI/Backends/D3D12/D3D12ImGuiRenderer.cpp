#include "UI/Backends/D3D12/D3D12ImGuiRenderer.h"

#include "Core/Assert.h"
#include "RHI/D3D12/D3D12RenderBackend.h"
#include "RHI/D3D12/D3D12Resources.h"
#include "UI/UiDrawPacket.h"

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>

namespace Prism::RHI::D3D12
{
void D3D12ImGuiRenderer::Initialize(
    IRenderBackend& renderBackend)
{
    if (m_initialized)
    {
        return;
    }

    m_backend = dynamic_cast<D3D12RenderBackend*>(
        &renderBackend);
    Core::Check(
        m_backend != nullptr,
        "D3D12 ImGui requires the D3D12 RHI backend.");
    D3D12Context& context = m_backend->GetContext();
    Core::Check(
        ImGui_ImplDX12_Init(
            context.GetDevice(),
            context.GetFramesInFlight(),
            context.GetBackBufferFormat(),
            context.GetShaderVisibleSrvHeap(),
            context.GetImGuiCpuDescriptorHandle(),
            context.GetImGuiGpuDescriptorHandle()),
        "Failed to initialize the ImGui D3D12 renderer.");
    m_initialized = true;
}

void D3D12ImGuiRenderer::Shutdown()
{
    if (!m_initialized)
    {
        return;
    }
    ImGui_ImplDX12_Shutdown();
    m_backend = nullptr;
    m_initialized = false;
}

void D3D12ImGuiRenderer::BeginFrame()
{
    Core::Check(
        m_initialized,
        "The D3D12 ImGui renderer is not initialized.");
    ImGui_ImplDX12_NewFrame();
}

std::uint64_t D3D12ImGuiRenderer::RegisterTexture(
    const ITextureView& textureView)
{
    Core::Check(
        m_initialized && m_backend != nullptr,
        "The D3D12 ImGui renderer is not initialized.");
    const auto* d3d12View = dynamic_cast<
        const D3D12TextureView*>(&textureView);
    Core::Check(
        d3d12View != nullptr,
        "The D3D12 editor requires a D3D12 texture view.");

    D3D12Context& context = m_backend->GetContext();
    const D3D12Context::ShaderVisibleDescriptor descriptor =
        context.AllocateShaderVisibleSrv();
    context.GetDevice()->CopyDescriptorsSimple(
        1,
        descriptor.cpuHandle,
        d3d12View->GetCpuHandle(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return descriptor.gpuHandle.ptr;
}

void D3D12ImGuiRenderer::UnregisterTexture(
    const std::uint64_t textureId)
{
    if (textureId == 0 || m_backend == nullptr)
    {
        return;
    }

    D3D12Context& context = m_backend->GetContext();
    const D3D12_GPU_DESCRIPTOR_HANDLE heapStart =
        context.GetShaderVisibleSrvHeap()
            ->GetGPUDescriptorHandleForHeapStart();
    Core::Check(
        textureId >= heapStart.ptr,
        "The D3D12 ImGui texture identifier is outside the SRV heap.");
    const UINT descriptorSize = context.GetDevice()
        ->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const std::uint64_t byteOffset = textureId - heapStart.ptr;
    Core::Check(
        byteOffset % descriptorSize == 0,
        "The D3D12 ImGui texture identifier is not descriptor aligned.");
    context.RetireShaderVisibleSrvRange(
        static_cast<std::uint32_t>(byteOffset / descriptorSize),
        1);
}

void D3D12ImGuiRenderer::RenderDrawData(
    const UI::UiDrawPacket& drawPacket)
{
    Core::Check(
        m_initialized && m_backend != nullptr,
        "The D3D12 ImGui renderer is not initialized.");

    D3D12Context& context = m_backend->GetContext();
    const D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv =
        context.GetCurrentRtvHandle();
    constexpr float ClearColor[4] = {
        0.035f,
        0.040f,
        0.048f,
        1.0f};
    context.GetCommandList()->OMSetRenderTargets(
        1,
        &backBufferRtv,
        FALSE,
        nullptr);
    context.GetCommandList()->ClearRenderTargetView(
        backBufferRtv,
        ClearColor,
        0,
        nullptr);
    ID3D12DescriptorHeap* descriptorHeaps[] = {
        context.GetShaderVisibleSrvHeap()};
    context.GetCommandList()->SetDescriptorHeaps(
        1,
        descriptorHeaps);
    ImGui_ImplDX12_RenderDrawData(
        &drawPacket.GetDrawData(),
        context.GetCommandList());
}
} // namespace Prism::RHI::D3D12
