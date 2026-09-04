#include "RHI/D3D12/D3D12GraphicsDevice.h"

#include "Core/Assert.h"
#include "RHI/D3D12/D3D12Debug.h"
#include "RHI/D3D12/D3D12PipelineView.h"
#include "RHI/D3D12/D3D12Resources.h"
#include "RHI/D3D12/D3D12TransientResources.h"
#include "RHI/D3D12/D3D12TypeConversions.h"
#include "RHI/D3D12/D3D12Context.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Prism::RHI::D3D12
{
namespace
{
class D3D12AccelerationStructure final
    : public IRayTracingAccelerationStructure
{
public:
    D3D12AccelerationStructure(
        D3D12Context& context,
        AccelerationStructureBuildDescription
            description,
        const AccelerationStructureBuildSizes sizes,
        Microsoft::WRL::ComPtr<ID3D12Resource>
            resource)
        : m_context(&context)
        , m_description(std::move(description))
        , m_sizes(sizes)
        , m_resource(std::move(resource))
    {
    }

    ~D3D12AccelerationStructure() override
    {
        if (m_context != nullptr
            && m_resource != nullptr)
        {
            m_context->RetireResource(
                std::move(m_resource));
        }
    }

    GraphicsApi GetGraphicsApi() const override
    {
        return GraphicsApi::Direct3D12;
    }

    const AccelerationStructureBuildDescription&
    GetDescription() const override
    {
        return m_description;
    }

    const AccelerationStructureBuildSizes&
    GetBuildSizes() const override
    {
        return m_sizes;
    }

    std::uint64_t GetDeviceAddress() const override
    {
        return m_resource->GetGPUVirtualAddress();
    }

    std::uint64_t GetNativeHandleBits() const override
    {
        return GetDeviceAddress();
    }

    ID3D12Resource* GetResource() const
    {
        return m_resource.Get();
    }

private:
    D3D12Context* m_context = nullptr;
    AccelerationStructureBuildDescription
        m_description;
    AccelerationStructureBuildSizes m_sizes;
    Microsoft::WRL::ComPtr<ID3D12Resource>
        m_resource;
};

D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS
ToNativeBuildFlags(
    const AccelerationStructureBuildFlags flags)
{
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS result =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
    if (HasAnyFlag(
            flags,
            AccelerationStructureBuildFlags::AllowUpdate))
    {
        result |=
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    }
    if (HasAnyFlag(
            flags,
            AccelerationStructureBuildFlags::AllowCompaction))
    {
        result |=
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
    }
    if (HasAnyFlag(
            flags,
            AccelerationStructureBuildFlags::PreferFastTrace))
    {
        result |=
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    }
    if (HasAnyFlag(
            flags,
            AccelerationStructureBuildFlags::PreferFastBuild))
    {
        result |=
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    }
    if (HasAnyFlag(
            flags,
            AccelerationStructureBuildFlags::MinimizeMemory))
    {
        result |=
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY;
    }
    return result;
}

D3D12_RAYTRACING_INSTANCE_FLAGS
ToNativeInstanceFlags(
    const RayTracingInstanceFlags flags)
{
    D3D12_RAYTRACING_INSTANCE_FLAGS result =
        D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
    if (HasAnyFlag(
            flags,
            RayTracingInstanceFlags::
                TriangleCullDisable))
    {
        result |=
            D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    }
    if (HasAnyFlag(
            flags,
            RayTracingInstanceFlags::
                TriangleFrontCounterClockwise))
    {
        result |=
            D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
    }
    if (HasAnyFlag(
            flags,
            RayTracingInstanceFlags::ForceOpaque))
    {
        result |=
            D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
    }
    if (HasAnyFlag(
            flags,
            RayTracingInstanceFlags::
                ForceNonOpaque))
    {
        result |=
            D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
    }
    return result;
}

Microsoft::WRL::ComPtr<ID3D12Resource>
CreateRayTracingBuffer(
    D3D12Context& context,
    const std::uint64_t size,
    const D3D12_RESOURCE_STATES initialState)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC description{};
    description.Dimension =
        D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.Layout =
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags =
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    Core::ThrowIfFailed(
        context.GetDevice()->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &description,
            initialState,
            nullptr,
            IID_PPV_ARGS(&resource)),
        "Failed to allocate a DXR acceleration-structure buffer.");
    return resource;
}

D3D12_DESCRIPTOR_RANGE_TYPE ToRangeType(const DescriptorType type)
{
    switch (type)
    {
    case DescriptorType::ConstantBuffer: return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    case DescriptorType::DynamicConstantBuffer: return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    case DescriptorType::SampledTexture: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    case DescriptorType::AccelerationStructure: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    case DescriptorType::ReadOnlyStorageBuffer: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    case DescriptorType::Sampler: return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    case DescriptorType::StorageBuffer:
    case DescriptorType::StorageTexture: return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    }
    throw std::invalid_argument("Unsupported D3D12 descriptor range type.");
}

std::uint32_t ToShaderRegister(const DescriptorBindingDescription& binding)
{
    switch (binding.type)
    {
    case DescriptorType::ConstantBuffer: return binding.binding;
    case DescriptorType::DynamicConstantBuffer: return binding.binding;
    case DescriptorType::SampledTexture: return binding.binding >= 16 ? binding.binding - 16 : binding.binding;
    case DescriptorType::AccelerationStructure:
        return binding.binding >= 16
            ? binding.binding - 16
            : binding.binding;
    case DescriptorType::ReadOnlyStorageBuffer:
        return binding.binding >= 16
            ? binding.binding - 16
            : binding.binding;
    case DescriptorType::StorageBuffer:
    case DescriptorType::StorageTexture: return binding.binding >= 32 ? binding.binding - 32 : binding.binding;
    case DescriptorType::Sampler: return binding.binding >= 48 ? binding.binding - 48 : binding.binding;
    }
    return binding.binding;
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootSignature(
    D3D12Context& context,
    const std::shared_ptr<IDescriptorSetLayout>& layout)
{
    std::vector<D3D12_DESCRIPTOR_RANGE> resourceRanges;
    std::vector<D3D12_DESCRIPTOR_RANGE> samplerRanges;
    if (layout != nullptr)
    {
        const auto nativeLayout = std::dynamic_pointer_cast<D3D12DescriptorSetLayout>(layout);
        Core::Check(nativeLayout != nullptr, "D3D12 pipelines require a D3D12 descriptor-set layout.");
        for (const DescriptorBindingDescription& binding : nativeLayout->GetDescription().bindings)
        {
            if (binding.type == DescriptorType::DynamicConstantBuffer)
            {
                continue;
            }
            const auto& location = nativeLayout->GetBindingLocation(binding.binding);
            D3D12_DESCRIPTOR_RANGE range{};
            range.RangeType = ToRangeType(binding.type);
            range.NumDescriptors = binding.descriptorCount;
            range.BaseShaderRegister = ToShaderRegister(binding);
            range.RegisterSpace = 0;
            range.OffsetInDescriptorsFromTableStart = location.offset;
            (location.sampler ? samplerRanges : resourceRanges).push_back(range);
        }
    }

    std::vector<D3D12_ROOT_PARAMETER> parameters;
    if (!resourceRanges.empty())
    {
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(resourceRanges.size());
        parameter.DescriptorTable.pDescriptorRanges = resourceRanges.data();
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters.push_back(parameter);
    }
    if (!samplerRanges.empty())
    {
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(samplerRanges.size());
        parameter.DescriptorTable.pDescriptorRanges = samplerRanges.data();
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters.push_back(parameter);
    }
    if (layout != nullptr)
    {
        const auto nativeLayout = std::dynamic_pointer_cast<D3D12DescriptorSetLayout>(layout);
        for (const std::uint32_t bindingIndex : nativeLayout->GetDynamicBufferBindings())
        {
            const DescriptorBindingDescription& binding = *std::ranges::find_if(
                nativeLayout->GetDescription().bindings,
                [bindingIndex](const DescriptorBindingDescription& candidate)
                {
                    return candidate.binding == bindingIndex;
                });
            D3D12_ROOT_PARAMETER parameter{};
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameter.Descriptor.ShaderRegister = ToShaderRegister(binding);
            parameter.Descriptor.RegisterSpace = 0;
            parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameters.push_back(parameter);
        }
    }

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT serializeResult = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(serializeResult))
    {
        const char* message = errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer())
                                                : "Unknown root-signature serialization error.";
        throw std::runtime_error(message);
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    Core::ThrowIfFailed(
        context.GetDevice()->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&rootSignature)),
        "Failed to create a public D3D12 root signature.");
    return rootSignature;
}

D3D12_PRIMITIVE_TOPOLOGY ToPrimitiveTopology(
    const PrimitiveTopology topology, const std::uint32_t patchControlPointCount)
{
    switch (topology)
    {
    case PrimitiveTopology::TriangleList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case PrimitiveTopology::LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case PrimitiveTopology::PatchList:
        Core::Check(patchControlPointCount >= 1u && patchControlPointCount <= 32u,
            "D3D12 patch-list topology supports one to thirty-two control points.");
        return static_cast<D3D12_PRIMITIVE_TOPOLOGY>(
            static_cast<std::uint32_t>(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST)
            + patchControlPointCount - 1u);
    }
    throw std::invalid_argument("Unsupported D3D12 primitive topology.");
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE ToPrimitiveTopologyType(const PrimitiveTopology topology)
{
    if (topology == PrimitiveTopology::LineList)
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    if (topology == PrimitiveTopology::PatchList)
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

DXGI_FORMAT ToVertexFormat(const VertexElementFormat format)
{
    switch (format)
    {
    case VertexElementFormat::Float: return DXGI_FORMAT_R32_FLOAT;
    case VertexElementFormat::Float2: return DXGI_FORMAT_R32G32_FLOAT;
    case VertexElementFormat::Float3: return DXGI_FORMAT_R32G32B32_FLOAT;
    case VertexElementFormat::Float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VertexElementFormat::Uint: return DXGI_FORMAT_R32_UINT;
    }
    throw std::invalid_argument("Unsupported D3D12 vertex format.");
}

const char* GetSemanticName(const std::uint32_t location)
{
    constexpr std::array<const char*, 5> Semantics = {"POSITION", "COLOR", "NORMAL", "TEXCOORD", "TANGENT"};
    Core::Check(location < Semantics.size(), "D3D12 public vertex locations above four require semantic metadata.");
    return Semantics[location];
}

class D3D12GraphicsPipeline final : public D3D12GraphicsPipelineBinding
{
public:
    D3D12GraphicsPipeline(
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline,
        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature,
        const D3D12_PRIMITIVE_TOPOLOGY topology)
        : m_pipeline(std::move(pipeline)), m_rootSignature(std::move(rootSignature)), m_topology(topology) {}

    GraphicsApi GetGraphicsApi() const override { return GraphicsApi::Direct3D12; }
    void SetDebugName(const std::string_view name) override
    {
        SetD3D12DebugName(m_pipeline.Get(), name);
        const std::string rootName =
            std::string(name) + ".RootSignature";
        SetD3D12DebugName(
            m_rootSignature.Get(),
            rootName);
    }
    ID3D12PipelineState* GetPipelineState() const override { return m_pipeline.Get(); }
    ID3D12RootSignature* GetRootSignature() const override { return m_rootSignature.Get(); }
    D3D12_PRIMITIVE_TOPOLOGY GetPrimitiveTopology() const override { return m_topology; }

private:
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    D3D12_PRIMITIVE_TOPOLOGY m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
};

class D3D12ComputePipeline final : public D3D12ComputePipelineBinding
{
public:
    D3D12ComputePipeline(
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline,
        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature)
        : m_pipeline(std::move(pipeline)), m_rootSignature(std::move(rootSignature)) {}

    GraphicsApi GetGraphicsApi() const override { return GraphicsApi::Direct3D12; }
    void SetDebugName(const std::string_view name) override
    {
        SetD3D12DebugName(m_pipeline.Get(), name);
        const std::string rootName =
            std::string(name) + ".RootSignature";
        SetD3D12DebugName(
            m_rootSignature.Get(),
            rootName);
    }
    ID3D12PipelineState* GetPipelineState() const override { return m_pipeline.Get(); }
    ID3D12RootSignature* GetRootSignature() const override { return m_rootSignature.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
};
} // namespace

D3D12GraphicsDevice::D3D12GraphicsDevice(D3D12Context& context) : m_context(&context) {}
GraphicsApi D3D12GraphicsDevice::GetGraphicsApi() const { return GraphicsApi::Direct3D12; }
const GraphicsDeviceCapabilities&
D3D12GraphicsDevice::GetCapabilities() const
{
    return m_context->GetDeviceCapabilities();
}

DescriptorAllocatorStatistics
D3D12GraphicsDevice::GetDescriptorAllocatorStatistics() const
{
    return m_context->GetDescriptorAllocatorStatistics();
}

UploadQueueStatistics
D3D12GraphicsDevice::GetUploadQueueStatistics() const
{
    return m_context->GetUploadQueueStatistics();
}

UploadTicket
D3D12GraphicsDevice::GetPendingUploadTicket() const
{
    return m_context->GetPendingUploadTicket();
}

bool D3D12GraphicsDevice::IsUploadComplete(
    const UploadTicket ticket) const
{
    return m_context->IsUploadComplete(ticket);
}

ResourceRetirementStatistics
D3D12GraphicsDevice::GetResourceRetirementStatistics() const
{
    return m_context
        ->GetResourceRetirementStatistics();
}

std::shared_ptr<IBuffer> D3D12GraphicsDevice::CreateBuffer(
    const BufferDescription& description,
    const void* initialData)
{
    return std::make_shared<D3D12Buffer>(*m_context, description, initialData);
}

std::shared_ptr<ITexture> D3D12GraphicsDevice::CreateTexture(
    const TextureDescription& description,
    const TextureInitialData* initialData)
{
    return std::make_shared<D3D12Texture>(*m_context, description, initialData);
}

std::shared_ptr<ITransientTexturePool>
D3D12GraphicsDevice::CreateTransientTexturePool(
    const std::vector<TransientTextureRequest>& requests)
{
    return std::make_shared<D3D12TransientTexturePool>(
        *m_context,
        requests);
}

std::shared_ptr<ITransientBufferPool>
D3D12GraphicsDevice::CreateTransientBufferPool(
    const std::vector<TransientBufferRequest>& requests)
{
    return std::make_shared<D3D12TransientBufferPool>(
        *m_context,
        requests);
}

std::shared_ptr<ITextureView> D3D12GraphicsDevice::CreateTextureView(
    std::shared_ptr<ITexture> texture,
    const TextureViewDescription& description)
{
    auto nativeTexture = std::dynamic_pointer_cast<D3D12Texture>(texture);
    Core::Check(nativeTexture != nullptr, "D3D12 texture views require D3D12 textures.");
    return std::make_shared<D3D12TextureView>(*m_context, std::move(nativeTexture), description);
}

std::shared_ptr<ISampler> D3D12GraphicsDevice::CreateSampler(const SamplerDescription& description)
{
    return std::make_shared<D3D12Sampler>(description);
}

std::shared_ptr<IDescriptorSetLayout> D3D12GraphicsDevice::CreateDescriptorSetLayout(
    const DescriptorSetLayoutDescription& description)
{
    return std::make_shared<D3D12DescriptorSetLayout>(description);
}

std::shared_ptr<IDescriptorSet> D3D12GraphicsDevice::CreateDescriptorSet(
    std::shared_ptr<IDescriptorSetLayout> layout)
{
    auto nativeLayout = std::dynamic_pointer_cast<D3D12DescriptorSetLayout>(layout);
    Core::Check(nativeLayout != nullptr, "D3D12 descriptor sets require a D3D12 layout.");
    return std::make_shared<D3D12DescriptorSet>(*m_context, std::move(nativeLayout));
}

std::shared_ptr<IGraphicsPipeline> D3D12GraphicsDevice::CreateGraphicsPipeline(
    const GraphicsPipelineDescription& description)
{
    std::string validationError;
    Core::Check(ValidateGraphicsPipelineDescription(description, &validationError), validationError.c_str());
    Core::Check(description.vertexShader.format == ShaderBinaryFormat::Dxil
                    || description.vertexShader.format == ShaderBinaryFormat::Dxbc,
                "D3D12 graphics pipelines require DXIL or DXBC shaders.");
    auto rootSignature = CreateRootSignature(*m_context, description.descriptorSetLayout);

    std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    inputElements.reserve(description.vertexAttributes.size());
    for (const VertexAttributeDescription& attribute : description.vertexAttributes)
    {
        const auto binding = std::ranges::find_if(description.vertexBindings, [&](const auto& candidate)
        {
            return candidate.binding == attribute.binding;
        });
        Core::Check(binding != description.vertexBindings.end(), "D3D12 vertex attribute references an unknown binding.");
        inputElements.push_back({
            attribute.semanticName.empty() ? GetSemanticName(attribute.location) : attribute.semanticName.c_str(),
            attribute.semanticIndex,
            ToVertexFormat(attribute.format), attribute.binding,
            attribute.offset,
            binding->inputRate == VertexInputRate::PerInstance
                ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            binding->inputRate == VertexInputRate::PerInstance ? 1u : 0u});
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC native{};
    native.pRootSignature = rootSignature.Get();
    native.VS = ToNativeShaderBytecode(description.vertexShader);
    if (description.hullShader.IsValid()) native.HS = ToNativeShaderBytecode(description.hullShader);
    if (description.domainShader.IsValid()) native.DS = ToNativeShaderBytecode(description.domainShader);
    if (description.pixelShader.IsValid()) native.PS = ToNativeShaderBytecode(description.pixelShader);
    native.BlendState.AlphaToCoverageEnable = FALSE;
    native.BlendState.IndependentBlendEnable = TRUE;
    for (std::size_t index = 0; index < description.blendAttachments.size(); ++index)
    {
        native.BlendState.RenderTarget[index] = ToNativeBlendDescription(description.blendAttachments[index]).RenderTarget[0];
    }
    native.SampleMask = std::numeric_limits<UINT>::max();
    native.RasterizerState = ToNativeRasterizerDescription(description.rasterizer);
    native.DepthStencilState = ToNativeDepthStencilDescription(description.depthStencil);
    native.InputLayout = {inputElements.data(), static_cast<UINT>(inputElements.size())};
    native.PrimitiveTopologyType = ToPrimitiveTopologyType(description.topology);
    native.NumRenderTargets = static_cast<UINT>(description.colorFormats.size());
    for (std::size_t index = 0; index < description.colorFormats.size(); ++index)
        native.RTVFormats[index] = ToNativeFormat(description.colorFormats[index]);
    native.DSVFormat = IsDepthFormat(description.depthFormat) ? ToNativeFormat(description.depthFormat) : DXGI_FORMAT_UNKNOWN;
    native.SampleDesc.Count = description.sampleCount;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Core::ThrowIfFailed(
        m_context->GetDevice()->CreateGraphicsPipelineState(&native, IID_PPV_ARGS(&pipeline)),
        "Failed to create a public D3D12 graphics pipeline.");
    m_pipelineCreationCounters.RecordGraphics();
    auto result = std::make_shared<D3D12GraphicsPipeline>(
        std::move(pipeline), std::move(rootSignature),
        ToPrimitiveTopology(description.topology, description.patchControlPointCount));
    std::string name = "GraphicsPSO." +
        description.vertexShader.entryPoint;
    if (description.pixelShader.IsValid())
    {
        name += "+" +
            description.pixelShader.entryPoint;
    }
    result->SetDebugName(name);
    return result;
}

std::shared_ptr<IComputePipeline> D3D12GraphicsDevice::CreateComputePipeline(
    const ComputePipelineDescription& description)
{
    std::string validationError;
    Core::Check(ValidateComputePipelineDescription(description, &validationError), validationError.c_str());
    Core::Check(description.computeShader.format == ShaderBinaryFormat::Dxil
                    || description.computeShader.format == ShaderBinaryFormat::Dxbc,
                "D3D12 compute pipelines require DXIL or DXBC shaders.");
    auto rootSignature = CreateRootSignature(*m_context, description.descriptorSetLayout);
    D3D12_COMPUTE_PIPELINE_STATE_DESC native{};
    native.pRootSignature = rootSignature.Get();
    native.CS = ToNativeShaderBytecode(description.computeShader);
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Core::ThrowIfFailed(
        m_context->GetDevice()->CreateComputePipelineState(&native, IID_PPV_ARGS(&pipeline)),
        "Failed to create a public D3D12 compute pipeline.");
    m_pipelineCreationCounters.RecordCompute();
    auto result = std::make_shared<D3D12ComputePipeline>(
        std::move(pipeline),
        std::move(rootSignature));
    result->SetDebugName(
        "ComputePSO." +
        description.computeShader.entryPoint);
    return result;
}

AccelerationStructureBuildSizes
D3D12GraphicsDevice::QueryAccelerationStructureBuildSizes(
    const AccelerationStructureBuildDescription&
        description) const
{
    std::string validationError;
    Core::Check(
        ValidateAccelerationStructureBuildDescription(
            description,
            &validationError),
        validationError.c_str());
    Core::Check(
        m_context->GetDeviceCapabilities()
            .features.rayTracingAccelerationStructure,
        "The selected D3D12 adapter does not support DXR acceleration structures.");

    Microsoft::WRL::ComPtr<ID3D12Device5> rayTracingDevice;
    Core::ThrowIfFailed(
        m_context->GetDevice()->QueryInterface(
            IID_PPV_ARGS(&rayTracingDevice)),
        "Failed to query ID3D12Device5 for DXR.");

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>
        nativeGeometries;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
        nativeInputs{};
    nativeInputs.Type =
        description.type
                == AccelerationStructureType::BottomLevel
            ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL
            : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    nativeInputs.Flags =
        ToNativeBuildFlags(description.flags);
    nativeInputs.DescsLayout =
        D3D12_ELEMENTS_LAYOUT_ARRAY;

    if (description.type
        == AccelerationStructureType::BottomLevel)
    {
        nativeGeometries.reserve(
            description.geometries.size());
        for (const RayTracingTrianglesDescription& geometry :
             description.geometries)
        {
            D3D12_RAYTRACING_GEOMETRY_DESC native{};
            native.Type =
                D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            native.Flags =
                D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            if (HasAnyFlag(
                    geometry.flags,
                    RayTracingGeometryFlags::Opaque))
            {
                native.Flags |=
                    D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            }
            if (HasAnyFlag(
                    geometry.flags,
                    RayTracingGeometryFlags::
                        NoDuplicateAnyHitInvocation))
            {
                native.Flags |=
                    D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
            }
            native.Triangles.VertexFormat =
                DXGI_FORMAT_R32G32B32_FLOAT;
            native.Triangles.VertexCount =
                geometry.vertexCount;
            native.Triangles.VertexBuffer.StrideInBytes =
                geometry.vertexStride;
            native.Triangles.IndexCount =
                geometry.indexCount;
            native.Triangles.IndexFormat =
                geometry.indexCount == 0
                ? DXGI_FORMAT_UNKNOWN
                : geometry.indexFormat
                          == IndexFormat::UInt16
                    ? DXGI_FORMAT_R16_UINT
                    : DXGI_FORMAT_R32_UINT;
            nativeGeometries.push_back(native);
        }
        nativeInputs.NumDescs =
            static_cast<UINT>(
                nativeGeometries.size());
        nativeInputs.pGeometryDescs =
            nativeGeometries.data();
    }
    else
    {
        nativeInputs.NumDescs =
            description.instanceCount;
    }

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO
        nativeSizes{};
    rayTracingDevice
        ->GetRaytracingAccelerationStructurePrebuildInfo(
            &nativeInputs,
            &nativeSizes);
    Core::Check(
        nativeSizes.ResultDataMaxSizeInBytes > 0
            && nativeSizes.ScratchDataSizeInBytes > 0,
        "DXR returned invalid acceleration-structure build sizes.");
    return {
        nativeSizes.ResultDataMaxSizeInBytes,
        nativeSizes.ScratchDataSizeInBytes,
        nativeSizes.UpdateScratchDataSizeInBytes};
}

std::shared_ptr<IRayTracingAccelerationStructure>
D3D12GraphicsDevice::CreateAccelerationStructure(
    const AccelerationStructureBuildRequest&
        request)
{
    std::string validationError;
    Core::Check(
        ValidateAccelerationStructureBuildRequest(
            request,
            GraphicsApi::Direct3D12,
            &validationError),
        validationError.c_str());
    const AccelerationStructureBuildSizes sizes =
        QueryAccelerationStructureBuildSizes(
            request.description);
    Microsoft::WRL::ComPtr<ID3D12Resource> resultBuffer =
        CreateRayTracingBuffer(
            *m_context,
            sizes.accelerationStructureBytes,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    Microsoft::WRL::ComPtr<ID3D12Resource> scratchBuffer =
        CreateRayTracingBuffer(
            *m_context,
            sizes.buildScratchBytes,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto result =
        std::make_shared<
            D3D12AccelerationStructure>(
            *m_context,
            request.description,
            sizes,
            std::move(resultBuffer));

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>
        nativeGeometries;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC>
        nativeInstances;
    std::shared_ptr<IBuffer> instanceBuffer;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
        nativeInputs{};
    nativeInputs.Type =
        request.description.type
                == AccelerationStructureType::BottomLevel
            ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL
            : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    nativeInputs.Flags =
        ToNativeBuildFlags(
            request.description.flags);
    nativeInputs.DescsLayout =
        D3D12_ELEMENTS_LAYOUT_ARRAY;

    if (request.description.type
        == AccelerationStructureType::BottomLevel)
    {
        nativeGeometries.reserve(
            request.geometries.size());
        for (const RayTracingGeometryBuildInput& input :
             request.geometries)
        {
            const auto* vertexBuffer =
                dynamic_cast<const D3D12Buffer*>(
                    input.vertexBuffer.get());
            const auto* indexBuffer =
                dynamic_cast<const D3D12Buffer*>(
                    input.indexBuffer.get());
            Core::Check(
                vertexBuffer != nullptr,
                "DXR requires D3D12 vertex buffers.");
            Core::Check(
                input.vertexBufferOffset
                        + static_cast<std::uint64_t>(
                              input.description
                                  .vertexCount)
                              * input.description
                                    .vertexStride
                    <= input.vertexBuffer
                           ->GetDescription()
                           .size,
                "DXR vertex input exceeds its buffer.");
            D3D12_RAYTRACING_GEOMETRY_DESC native{};
            native.Type =
                D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            native.Flags =
                D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            if (HasAnyFlag(
                    input.description.flags,
                    RayTracingGeometryFlags::Opaque))
            {
                native.Flags |=
                    D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            }
            if (HasAnyFlag(
                    input.description.flags,
                    RayTracingGeometryFlags::
                        NoDuplicateAnyHitInvocation))
            {
                native.Flags |=
                    D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
            }
            native.Triangles.VertexBuffer.StartAddress =
                vertexBuffer->GetGpuVirtualAddress()
                + input.vertexBufferOffset;
            native.Triangles.VertexBuffer.StrideInBytes =
                input.description.vertexStride;
            native.Triangles.VertexCount =
                input.description.vertexCount;
            native.Triangles.VertexFormat =
                DXGI_FORMAT_R32G32B32_FLOAT;
            native.Triangles.IndexCount =
                input.description.indexCount;
            if (input.description.indexCount > 0)
            {
                Core::Check(
                    indexBuffer != nullptr,
                    "Indexed DXR geometry requires a D3D12 index buffer.");
                const std::uint64_t indexStride =
                    input.description.indexFormat
                            == IndexFormat::UInt16
                        ? 2
                        : 4;
                Core::Check(
                    input.indexBufferOffset
                            + indexStride
                                  * input.description
                                        .indexCount
                        <= input.indexBuffer
                               ->GetDescription()
                               .size,
                    "DXR index input exceeds its buffer.");
                native.Triangles.IndexBuffer =
                    indexBuffer->GetGpuVirtualAddress()
                    + input.indexBufferOffset;
                native.Triangles.IndexFormat =
                    input.description.indexFormat
                            == IndexFormat::UInt16
                        ? DXGI_FORMAT_R16_UINT
                        : DXGI_FORMAT_R32_UINT;
            }
            nativeGeometries.push_back(native);
        }
        nativeInputs.NumDescs =
            static_cast<UINT>(
                nativeGeometries.size());
        nativeInputs.pGeometryDescs =
            nativeGeometries.data();
    }
    else
    {
        nativeInstances.reserve(
            request.instances.size());
        for (const RayTracingInstanceDescription& instance :
             request.instances)
        {
            D3D12_RAYTRACING_INSTANCE_DESC native{};
            std::memcpy(
                native.Transform,
                instance.transform.data(),
                sizeof(native.Transform));
            native.InstanceID = instance.instanceId;
            native.InstanceMask = instance.mask;
            native.InstanceContributionToHitGroupIndex =
                instance.hitGroupOffset;
            native.Flags =
                ToNativeInstanceFlags(instance.flags);
            native.AccelerationStructure =
                instance.bottomLevel
                    ->GetDeviceAddress();
            nativeInstances.push_back(native);
        }
        BufferDescription bufferDescription{};
        bufferDescription.size =
            nativeInstances.size()
            * sizeof(
                D3D12_RAYTRACING_INSTANCE_DESC);
        bufferDescription.stride =
            sizeof(
                D3D12_RAYTRACING_INSTANCE_DESC);
        bufferDescription.usage =
            BufferUsage::
                AccelerationStructureBuildInput;
        bufferDescription.memoryAccess =
            MemoryAccess::CpuToGpu;
        instanceBuffer = CreateBuffer(
            bufferDescription,
            nativeInstances.data());
        const auto* nativeInstanceBuffer =
            dynamic_cast<const D3D12Buffer*>(
                instanceBuffer.get());
        nativeInputs.NumDescs =
            static_cast<UINT>(
                nativeInstances.size());
        nativeInputs.InstanceDescs =
            nativeInstanceBuffer
                ->GetGpuVirtualAddress();
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC
        build{};
    build.Inputs = nativeInputs;
    build.DestAccelerationStructureData =
        result->GetDeviceAddress();
    build.ScratchAccelerationStructureData =
        scratchBuffer->GetGPUVirtualAddress();
    m_context->ExecuteImmediate(
        [&](ID3D12GraphicsCommandList* commandList)
        {
            Microsoft::WRL::ComPtr<
                ID3D12GraphicsCommandList4>
                rayTracingCommandList;
            Core::ThrowIfFailed(
                commandList->QueryInterface(
                    IID_PPV_ARGS(
                        &rayTracingCommandList)),
                "Failed to query a DXR command list.");
            rayTracingCommandList
                ->BuildRaytracingAccelerationStructure(
                    &build,
                    0,
                    nullptr);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type =
                D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource =
                result->GetResource();
            commandList->ResourceBarrier(
                1,
                &barrier);
        });
    return result;
}
} // namespace Prism::RHI::D3D12
