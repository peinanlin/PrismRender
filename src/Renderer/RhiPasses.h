#pragma once

#include "RHI/GraphicsResources.h"
#include "RHI/PipelineState.h"
#include "RHI/Rendering.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Prism::RHI
{
class ICommandContext;
}

namespace Prism::Renderer
{
struct IndexedGeometryDraw
{
    const RHI::IGraphicsPipeline* pipelineOverride = nullptr;
    const RHI::IDescriptorSet* descriptorSet = nullptr;
    const RHI::IBuffer* vertexBuffer = nullptr;
    const RHI::IBuffer* indexBuffer = nullptr;
    RHI::IndexFormat indexFormat = RHI::IndexFormat::UInt16;
    std::uint32_t indexCount = 0;
    std::uint32_t instanceCount = 1;
    std::uint32_t firstInstance = 0;
    const RHI::IBuffer* indirectArguments = nullptr;
    std::size_t indirectArgumentOffset = 0;
    std::uint32_t indirectMaxDrawCount = 1;
    const RHI::IBuffer* indirectCountBuffer = nullptr;
    std::size_t indirectCountOffset = 0;
    std::vector<RHI::DynamicBufferOffset> dynamicBufferOffsets;
};

struct FullscreenDraw
{
    const RHI::IGraphicsPipeline* pipeline = nullptr;
    const RHI::IDescriptorSet* descriptorSet = nullptr;
};

class RhiGeometryPass
{
public:
    void Execute(
        RHI::ICommandContext& commandContext,
        const RHI::IGraphicsPipeline& pipeline,
        std::span<const IndexedGeometryDraw> draws) const;
};

class FullscreenTrianglePass
{
public:
    void Execute(RHI::ICommandContext& commandContext) const;
    void Execute(
        RHI::ICommandContext& commandContext,
        const RHI::IGraphicsPipeline& pipeline,
        const RHI::IDescriptorSet* descriptorSet = nullptr) const;
};

class RhiGeometryRenderingPass
{
public:
    void Execute(
        RHI::ICommandContext& commandContext,
        const RHI::RenderingInfo& renderingInfo,
        const RHI::IGraphicsPipeline& pipeline,
        std::span<const IndexedGeometryDraw> draws) const;

private:
    RhiGeometryPass m_geometryPass;
};

class RhiFullscreenRenderingPass
{
public:
    void Execute(
        RHI::ICommandContext& commandContext,
        const RHI::RenderingInfo& renderingInfo,
        std::span<const FullscreenDraw> draws) const;

private:
    FullscreenTrianglePass m_fullscreenPass;
};

class RhiShadowPass
{
public:
    void Execute(
        RHI::ICommandContext& commandContext,
        const RHI::RenderingInfo& renderingInfo,
        const RHI::IGraphicsPipeline& pipeline,
        std::span<const IndexedGeometryDraw> draws) const;

private:
    RhiGeometryRenderingPass m_renderingPass;
};
} // namespace Prism::Renderer
