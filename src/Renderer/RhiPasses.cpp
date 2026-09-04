#include "Renderer/RhiPasses.h"

#include "Core/Assert.h"
#include "RHI/ICommandContext.h"

namespace Prism::Renderer
{
void RhiGeometryPass::Execute(
    RHI::ICommandContext& commandContext,
    const RHI::IGraphicsPipeline& pipeline,
    const std::span<const IndexedGeometryDraw> draws) const
{
    const RHI::GraphicsApi graphicsApi = commandContext.GetGraphicsApi();
    Core::Check(pipeline.GetGraphicsApi() == graphicsApi,
                "RHI geometry pipeline and command context use different graphics APIs.");
    commandContext.BindGraphicsPipeline(pipeline);
    const RHI::IGraphicsPipeline* activePipeline =
        &pipeline;
    for (const IndexedGeometryDraw& draw : draws)
    {
        const RHI::IGraphicsPipeline* drawPipeline =
            draw.pipelineOverride != nullptr
            ? draw.pipelineOverride
            : &pipeline;
        Core::Check(
            drawPipeline->GetGraphicsApi() == graphicsApi,
            "RHI geometry draw pipeline and command context use different graphics APIs.");
        if (drawPipeline != activePipeline)
        {
            commandContext.BindGraphicsPipeline(
                *drawPipeline);
            activePipeline = drawPipeline;
        }
        Core::Check(draw.descriptorSet != nullptr, "RHI geometry draws require a descriptor set.");
        Core::Check(draw.vertexBuffer != nullptr && draw.indexBuffer != nullptr,
                    "RHI geometry draws require vertex and index buffers.");
        Core::Check(
            draw.indirectArguments != nullptr
                || (draw.indexCount > 0
                    && draw.instanceCount > 0),
            "RHI geometry draws require direct counts or an indirect argument buffer.");
        Core::Check(draw.descriptorSet->GetGraphicsApi() == graphicsApi
                        && draw.vertexBuffer->GetGraphicsApi() == graphicsApi
                        && draw.indexBuffer->GetGraphicsApi() == graphicsApi,
                    "RHI geometry draw resources and command context use different graphics APIs.");
        commandContext.BindDescriptorSet(*draw.descriptorSet, draw.dynamicBufferOffsets);
        commandContext.BindVertexBuffer(*draw.vertexBuffer);
        commandContext.BindIndexBuffer(*draw.indexBuffer, draw.indexFormat);
        if (draw.indirectArguments != nullptr)
        {
            Core::Check(
                draw.indirectArguments
                    ->GetGraphicsApi()
                    == graphicsApi,
                "RHI indirect arguments and command context use different graphics APIs.");
            commandContext.DrawIndexedIndirect(
                *draw.indirectArguments,
                draw.indirectArgumentOffset,
                draw.indirectMaxDrawCount,
                sizeof(RHI::DrawIndexedIndirectArguments),
                draw.indirectCountBuffer,
                draw.indirectCountOffset);
        }
        else
        {
            commandContext.DrawIndexed(
                draw.indexCount,
                draw.instanceCount,
                0,
                0,
                draw.firstInstance);
        }
    }
}

void FullscreenTrianglePass::Execute(RHI::ICommandContext& commandContext) const
{
    commandContext.Draw(3);
}

void FullscreenTrianglePass::Execute(
    RHI::ICommandContext& commandContext,
    const RHI::IGraphicsPipeline& pipeline,
    const RHI::IDescriptorSet* descriptorSet) const
{
    const RHI::GraphicsApi graphicsApi = commandContext.GetGraphicsApi();
    Core::Check(pipeline.GetGraphicsApi() == graphicsApi,
                "RHI fullscreen pipeline and command context use different graphics APIs.");
    commandContext.BindGraphicsPipeline(pipeline);
    if (descriptorSet != nullptr)
    {
        Core::Check(descriptorSet->GetGraphicsApi() == graphicsApi,
                    "RHI fullscreen descriptor set and command context use different graphics APIs.");
        commandContext.BindDescriptorSet(*descriptorSet);
    }
    Execute(commandContext);
}

void RhiGeometryRenderingPass::Execute(
    RHI::ICommandContext& commandContext,
    const RHI::RenderingInfo& renderingInfo,
    const RHI::IGraphicsPipeline& pipeline,
    const std::span<const IndexedGeometryDraw> draws) const
{
    commandContext.BeginRendering(renderingInfo);
    m_geometryPass.Execute(commandContext, pipeline, draws);
    commandContext.EndRendering();
}

void RhiFullscreenRenderingPass::Execute(
    RHI::ICommandContext& commandContext,
    const RHI::RenderingInfo& renderingInfo,
    const std::span<const FullscreenDraw> draws) const
{
    Core::Check(!draws.empty(), "RHI fullscreen rendering passes require at least one draw.");
    commandContext.BeginRendering(renderingInfo);
    for (const FullscreenDraw& draw : draws)
    {
        Core::Check(draw.pipeline != nullptr, "RHI fullscreen draws require a graphics pipeline.");
        m_fullscreenPass.Execute(commandContext, *draw.pipeline, draw.descriptorSet);
    }
    commandContext.EndRendering();
}

void RhiShadowPass::Execute(
    RHI::ICommandContext& commandContext,
    const RHI::RenderingInfo& renderingInfo,
    const RHI::IGraphicsPipeline& pipeline,
    const std::span<const IndexedGeometryDraw> draws) const
{
    m_renderingPass.Execute(commandContext, renderingInfo, pipeline, draws);
}
} // namespace Prism::Renderer
