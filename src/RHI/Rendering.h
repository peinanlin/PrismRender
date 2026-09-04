#pragma once

#include "RHI/GraphicsResources.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Prism::RHI
{
enum class LoadOperation
{
    Load,
    Clear,
    Discard
};

enum class StoreOperation
{
    Store,
    Discard
};

struct ClearColorValue
{
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;
};

struct ClearDepthStencilValue
{
    float depth = 1.0f;
    std::uint32_t stencil = 0;
};

struct RenderingAttachment
{
    const ITextureView* view = nullptr;
    LoadOperation loadOperation = LoadOperation::Clear;
    StoreOperation storeOperation = StoreOperation::Store;
    ClearColorValue clearColor{};
    ClearDepthStencilValue clearDepthStencil{};
    ResourceState stateBefore = ResourceState::Undefined;
    ResourceState stateAfter = ResourceState::Undefined;
};

struct RenderingInfo
{
    std::vector<RenderingAttachment> colorAttachments;
    std::optional<RenderingAttachment> depthAttachment;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t layerCount = 1;
};

bool ValidateRenderingInfo(const RenderingInfo& renderingInfo, std::string* outError = nullptr);
} // namespace Prism::RHI
