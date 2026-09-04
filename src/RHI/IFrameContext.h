#pragma once

#include "RHI/GraphicsApi.h"
#include "RHI/GraphicsTypes.h"
#include "RHI/FramePacingStatistics.h"

#include <cstdint>
#include <memory>

namespace Prism::RHI
{
class ITexture;
class ITextureView;

enum class FrameResult
{
    Ready,
    SwapChainOutOfDate
};

// Owns the API-independent frame and presentation lifecycle.
// Resource creation and command recording remain separate RHI responsibilities.
class IFrameContext
{
public:
    virtual ~IFrameContext() = default;

    // Set on the frame owner lane before the loop; disabled by default.
    void SetFramePacingEnabled(bool enabled) noexcept { m_framePacingEnabled = enabled; }
    const FramePacingStatistics* GetFramePacingStatistics() const noexcept
    { return m_framePacingEnabled ? &m_framePacing : nullptr; }

    virtual GraphicsApi GetGraphicsApi() const = 0;
    virtual FrameResult BeginFrame() = 0;
    virtual FrameResult EndFrame() = 0;
    virtual void Resize(
        std::uint32_t width,
        std::uint32_t height) = 0;
    virtual void WaitForGpu() = 0;

    virtual std::uint32_t GetCurrentFrameIndex() const = 0;
    virtual std::uint32_t GetFramesInFlight() const = 0;
    virtual std::uint32_t GetFrameWidth() const = 0;
    virtual std::uint32_t GetFrameHeight() const = 0;
    virtual const std::shared_ptr<ITexture>&
        GetCurrentBackBufferTexture() const = 0;
    virtual const ITextureView&
        GetCurrentBackBufferView() const = 0;
    virtual Format GetBackBufferRhiFormat() const = 0;
    virtual const std::shared_ptr<ITexture>&
        GetDepthStencilTexture() const = 0;
    virtual const ITextureView&
        GetDepthStencilView() const = 0;

protected:
    FramePacingStatistics* FramePacing() noexcept
    { return m_framePacingEnabled ? &m_framePacing : nullptr; }
private:
    bool m_framePacingEnabled = false;
    FramePacingStatistics m_framePacing;
};
} // namespace Prism::RHI
