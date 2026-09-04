#pragma once

#include "RHI/GraphicsAdapterInfo.h"
#include "RHI/FramePacing.h"

#include <filesystem>
#include <string>

namespace Prism::Platform
{
class Window;
}

namespace Prism::RHI
{
class IFrameContext;
class IGraphicsDevice;
class ICommandContext;
class ITexture;

// Composition root for one graphics API backend. High-level rendering code
// receives this interface instead of a D3D12Context or VulkanContext.
class IRenderBackend
{
public:
    virtual ~IRenderBackend() = default;

    virtual void Initialize(Platform::Window& window) = 0;
    virtual IFrameContext& GetFrameContext() = 0;
    virtual const IFrameContext& GetFrameContext() const = 0;
    virtual IGraphicsDevice& GetGraphicsDevice() = 0;
    virtual const IGraphicsDevice& GetGraphicsDevice() const = 0;
    virtual ICommandContext& GetCommandContext() = 0;
    virtual const std::string& GetAdapterName() const = 0;
    virtual const GraphicsAdapterInfo&
        GetAdapterInfo() const = 0;
    virtual FrameAdmissionResult WaitForFrameAdmission() = 0;
    virtual bool ApplyFramePacingConfiguration(
        const FramePacingConfiguration& configuration,
        std::uint64_t generation,
        std::string* outErrorMessage = nullptr) = 0;
    virtual const FramePacingState& GetFramePacingState() const = 0;

    // Readback and presentation timing stay inside the selected API backend.
    virtual void RequestTextureCapture(
        std::filesystem::path outputPath) = 0;
    virtual void RecordTextureCapture(
        const ITexture* texture) = 0;
    virtual bool ResolveTextureCapture(
        std::string* outErrorMessage = nullptr) = 0;
};
} // namespace Prism::RHI
