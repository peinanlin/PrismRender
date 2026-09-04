#include "RHI/Rendering.h"

namespace Prism::RHI
{
bool ValidateRenderingInfo(const RenderingInfo& renderingInfo, std::string* outError)
{
    const auto fail = [&](const char* message)
    {
        if (outError != nullptr)
        {
            *outError = message;
        }
        return false;
    };

    if (renderingInfo.width == 0 || renderingInfo.height == 0 || renderingInfo.layerCount == 0)
    {
        return fail("Rendering dimensions and layer count must be non-zero.");
    }
    if (renderingInfo.colorAttachments.empty() && !renderingInfo.depthAttachment.has_value())
    {
        return fail("Rendering requires at least one color or depth attachment.");
    }
    for (const RenderingAttachment& attachment : renderingInfo.colorAttachments)
    {
        if (attachment.view == nullptr
            || attachment.view->GetDescription().type != TextureViewType::RenderTarget)
        {
            return fail("Color attachments require render-target texture views.");
        }
    }
    if (renderingInfo.depthAttachment.has_value())
    {
        const RenderingAttachment& attachment = *renderingInfo.depthAttachment;
        if (attachment.view == nullptr
            || attachment.view->GetDescription().type != TextureViewType::DepthStencil)
        {
            return fail("Depth attachments require depth-stencil texture views.");
        }
    }
    return true;
}
} // namespace Prism::RHI
