#pragma once

#include "RHI/D3D12/D3D12CommandContextAdapter.h"
#include "RHI/D3D12/D3D12Context.h"
#include "RHI/D3D12/D3D12GraphicsDevice.h"
#include "RHI/IRenderBackend.h"

#include <filesystem>

namespace Prism::RHI::D3D12
{
class D3D12RenderBackend final : public IRenderBackend
{
public:
    explicit D3D12RenderBackend(
        const FramePacingConfiguration& framePacing = {});

    void Initialize(Platform::Window& window) override;
    IFrameContext& GetFrameContext() override;
    const IFrameContext& GetFrameContext() const override;
    IGraphicsDevice& GetGraphicsDevice() override;
    const IGraphicsDevice& GetGraphicsDevice() const override;
    ICommandContext& GetCommandContext() override;
    const std::string& GetAdapterName() const override;
    const GraphicsAdapterInfo& GetAdapterInfo() const override;
    FrameAdmissionResult WaitForFrameAdmission() override;
    bool ApplyFramePacingConfiguration(
        const FramePacingConfiguration& configuration,
        std::uint64_t generation,
        std::string* outErrorMessage = nullptr) override;
    const FramePacingState& GetFramePacingState() const override;

    D3D12Context& GetContext();
    const D3D12Context& GetContext() const;
    void RequestTextureCapture(
        std::filesystem::path outputPath) override;
    void RecordTextureCapture(
        const ITexture* texture) override;
    bool ResolveTextureCapture(
        std::string* outErrorMessage = nullptr) override;

private:
    D3D12Context m_context;
    D3D12GraphicsDevice m_device;
    D3D12CommandContextAdapter m_commandContext;
    Microsoft::WRL::ComPtr<ID3D12Resource>
        m_captureReadback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT
        m_captureFootprint{};
    std::filesystem::path m_capturePath;
    std::uint64_t m_captureTotalBytes = 0;
    std::uint32_t m_captureWidth = 0;
    std::uint32_t m_captureHeight = 0;
    bool m_captureRecorded = false;
};
} // namespace Prism::RHI::D3D12
