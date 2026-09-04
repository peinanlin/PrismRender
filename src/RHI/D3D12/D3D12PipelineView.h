#pragma once

#include "RHI/PipelineState.h"

#include <d3d12.h>
#include <wrl/client.h>

namespace Prism::RHI::D3D12
{
class D3D12GraphicsPipelineBinding : public IGraphicsPipeline
{
public:
    virtual ID3D12PipelineState* GetPipelineState() const = 0;
    virtual ID3D12RootSignature* GetRootSignature() const = 0;
    virtual D3D12_PRIMITIVE_TOPOLOGY GetPrimitiveTopology() const = 0;
};

class D3D12GraphicsPipelineView final : public D3D12GraphicsPipelineBinding
{
public:
    D3D12GraphicsPipelineView(
        ID3D12PipelineState* pipelineState,
        ID3D12RootSignature* rootSignature,
        D3D12_PRIMITIVE_TOPOLOGY primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    GraphicsApi GetGraphicsApi() const override;
    ID3D12PipelineState* GetPipelineState() const;
    ID3D12RootSignature* GetRootSignature() const;
    D3D12_PRIMITIVE_TOPOLOGY GetPrimitiveTopology() const;

private:
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    D3D12_PRIMITIVE_TOPOLOGY m_primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
};

class D3D12ComputePipelineBinding : public IComputePipeline
{
public:
    virtual ID3D12PipelineState* GetPipelineState() const = 0;
    virtual ID3D12RootSignature* GetRootSignature() const = 0;
};

class D3D12ComputePipelineView final : public D3D12ComputePipelineBinding
{
public:
    D3D12ComputePipelineView(ID3D12PipelineState* pipelineState, ID3D12RootSignature* rootSignature);

    GraphicsApi GetGraphicsApi() const override;
    ID3D12PipelineState* GetPipelineState() const;
    ID3D12RootSignature* GetRootSignature() const;

private:
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
};
} // namespace Prism::RHI::D3D12
