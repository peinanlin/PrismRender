#include "Renderer/RenderGraphResources.h"

#include "Core/Assert.h"
#include "Renderer/RenderGraph.h"

#include <algorithm>

namespace Prism::Renderer
{
TextureHandle RenderGraphPassParameters::ReadTexture(
    const TextureHandle handle,
    const RHI::ResourceState state,
    const TextureSubresourceRange range)
{
    Core::Check(
        handle.IsValid(),
        "RenderGraph texture reads require a valid handle.");
    m_textureAccesses.push_back({
        handle,
        state,
        range,
        RenderGraphAccessMode::Read});
    return handle;
}

TextureHandle RenderGraphPassParameters::WriteTexture(
    TextureHandle handle,
    const RHI::ResourceState state,
    const TextureSubresourceRange range)
{
    Core::Check(
        handle.IsValid(),
        "RenderGraph texture writes require a valid handle.");
    ++handle.version;
    m_textureAccesses.push_back({
        handle,
        state,
        range,
        RenderGraphAccessMode::Write});
    return handle;
}

BufferHandle RenderGraphPassParameters::ReadBuffer(
    const BufferHandle handle,
    const RHI::ResourceState state,
    const BufferRange range)
{
    Core::Check(
        handle.IsValid(),
        "RenderGraph buffer reads require a valid handle.");
    m_bufferAccesses.push_back({
        handle,
        state,
        range,
        RenderGraphAccessMode::Read});
    return handle;
}

BufferHandle RenderGraphPassParameters::WriteBuffer(
    BufferHandle handle,
    const RHI::ResourceState state,
    const BufferRange range)
{
    Core::Check(
        handle.IsValid(),
        "RenderGraph buffer writes require a valid handle.");
    ++handle.version;
    m_bufferAccesses.push_back({
        handle,
        state,
        range,
        RenderGraphAccessMode::Write});
    return handle;
}

TextureViewHandle
RenderGraphPassParameters::ReadTextureView(
    const TextureViewHandle handle,
    const RHI::ResourceState state)
{
    Core::Check(
        handle.IsValid(),
        "RenderGraph texture-view reads require a valid handle.");
    m_textureViewAccesses.push_back({
        handle,
        state,
        RenderGraphAccessMode::Read});
    return handle;
}

TextureHandle
RenderGraphPassParameters::WriteTextureView(
    TextureViewHandle handle,
    const RHI::ResourceState state)
{
    Core::Check(
        handle.IsValid(),
        "RenderGraph texture-view writes require a valid handle.");
    ++handle.texture.version;
    m_textureViewAccesses.push_back({
        handle,
        state,
        RenderGraphAccessMode::Write});
    return handle.texture;
}

const std::vector<TextureParameterAccess>&
RenderGraphPassParameters::GetTextureAccesses() const
{
    return m_textureAccesses;
}

const std::vector<BufferParameterAccess>&
RenderGraphPassParameters::GetBufferAccesses() const
{
    return m_bufferAccesses;
}

const std::vector<TextureViewParameterAccess>&
RenderGraphPassParameters::GetTextureViewAccesses() const
{
    return m_textureViewAccesses;
}

RHI::ITexture& RenderGraphPassResources::GetTexture(
    const TextureHandle handle) const
{
    const bool declared = std::ranges::any_of(
        m_parameters->GetTextureAccesses(),
        [handle](const TextureParameterAccess& access)
        {
            return access.handle == handle;
        });
    Core::Check(
        declared,
        "A render-graph pass requested an undeclared texture.");
    return m_graph->ResolveTexture(handle);
}

RHI::IBuffer& RenderGraphPassResources::GetBuffer(
    const BufferHandle handle) const
{
    const bool declared = std::ranges::any_of(
        m_parameters->GetBufferAccesses(),
        [handle](const BufferParameterAccess& access)
        {
            return access.handle == handle;
        });
    Core::Check(
        declared,
        "A render-graph pass requested an undeclared buffer.");
    return m_graph->ResolveBuffer(handle);
}

RHI::ITextureView&
RenderGraphPassResources::GetTextureView(
    const TextureViewHandle handle) const
{
    const bool declared = std::ranges::any_of(
        m_parameters->GetTextureViewAccesses(),
        [handle](const TextureViewParameterAccess& access)
        {
            return access.handle == handle;
        });
    Core::Check(
        declared,
        "A render-graph pass requested an undeclared texture view.");
    return m_graph->ResolveTextureView(handle);
}
} // namespace Prism::Renderer
