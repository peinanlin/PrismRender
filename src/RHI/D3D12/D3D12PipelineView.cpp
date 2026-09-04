#include "RHI/D3D12/D3D12PipelineView.h"

#include "Core/Assert.h"

namespace Prism::RHI::D3D12
{
D3D12GraphicsPipelineView::D3D12GraphicsPipelineView(
    ID3D12PipelineState* pipelineState,
    ID3D12RootSignature* rootSignature,
    const D3D12_PRIMITIVE_TOPOLOGY primitiveTopology)
    : m_pipelineState(pipelineState)
    , m_rootSignature(rootSignature)
    , m_primitiveTopology(primitiveTopology)
{
    Core::Check(m_pipelineState != nullptr && m_rootSignature != nullptr,
                "D3D12 graphics pipeline views require a PSO and root signature.");
}

GraphicsApi D3D12GraphicsPipelineView::GetGraphicsApi() const { return GraphicsApi::Direct3D12; }
ID3D12PipelineState* D3D12GraphicsPipelineView::GetPipelineState() const { return m_pipelineState.Get(); }
ID3D12RootSignature* D3D12GraphicsPipelineView::GetRootSignature() const { return m_rootSignature.Get(); }
D3D12_PRIMITIVE_TOPOLOGY D3D12GraphicsPipelineView::GetPrimitiveTopology() const { return m_primitiveTopology; }

D3D12ComputePipelineView::D3D12ComputePipelineView(
    ID3D12PipelineState* pipelineState,
    ID3D12RootSignature* rootSignature)
    : m_pipelineState(pipelineState)
    , m_rootSignature(rootSignature)
{
    Core::Check(m_pipelineState != nullptr && m_rootSignature != nullptr,
                "D3D12 compute pipeline views require a PSO and root signature.");
}

GraphicsApi D3D12ComputePipelineView::GetGraphicsApi() const { return GraphicsApi::Direct3D12; }
ID3D12PipelineState* D3D12ComputePipelineView::GetPipelineState() const { return m_pipelineState.Get(); }
ID3D12RootSignature* D3D12ComputePipelineView::GetRootSignature() const { return m_rootSignature.Get(); }
} // namespace Prism::RHI::D3D12
