#pragma once

#include "RHI/IGraphicsDevice.h"

namespace Prism::RHI
{
class D3D12Context;
}

namespace Prism::RHI::D3D12
{
class D3D12GraphicsDevice final : public IGraphicsDevice
{
public:
    explicit D3D12GraphicsDevice(D3D12Context& context);

    GraphicsApi GetGraphicsApi() const override;
    ShaderBinaryFormat
        GetPreferredShaderBinaryFormat() const override
    {
        return ShaderBinaryFormat::Dxil;
    }
    const GraphicsDeviceCapabilities&
        GetCapabilities() const override;
    DescriptorAllocatorStatistics
        GetDescriptorAllocatorStatistics() const override;
    UploadQueueStatistics
        GetUploadQueueStatistics() const override;
    UploadTicket GetPendingUploadTicket() const override;
    bool IsUploadComplete(
        UploadTicket ticket) const override;
    ResourceRetirementStatistics
        GetResourceRetirementStatistics() const override;
    std::shared_ptr<IBuffer> CreateBuffer(
        const BufferDescription& description,
        const void* initialData = nullptr) override;
    std::shared_ptr<ITexture> CreateTexture(
        const TextureDescription& description,
        const TextureInitialData* initialData = nullptr) override;
    std::shared_ptr<ITransientTexturePool>
        CreateTransientTexturePool(
            const std::vector<TransientTextureRequest>&
                requests) override;
    std::shared_ptr<ITransientBufferPool>
        CreateTransientBufferPool(
            const std::vector<TransientBufferRequest>&
                requests) override;
    std::shared_ptr<ITextureView> CreateTextureView(
        std::shared_ptr<ITexture> texture,
        const TextureViewDescription& description) override;
    std::shared_ptr<ISampler> CreateSampler(const SamplerDescription& description) override;
    std::shared_ptr<IDescriptorSetLayout> CreateDescriptorSetLayout(
        const DescriptorSetLayoutDescription& description) override;
    std::shared_ptr<IDescriptorSet> CreateDescriptorSet(
        std::shared_ptr<IDescriptorSetLayout> layout) override;
    std::shared_ptr<IGraphicsPipeline> CreateGraphicsPipeline(
        const GraphicsPipelineDescription& description) override;
    std::shared_ptr<IComputePipeline> CreateComputePipeline(
        const ComputePipelineDescription& description) override;
    AccelerationStructureBuildSizes
        QueryAccelerationStructureBuildSizes(
            const AccelerationStructureBuildDescription&
                description) const override;
    std::shared_ptr<
        IRayTracingAccelerationStructure>
        CreateAccelerationStructure(
            const AccelerationStructureBuildRequest&
                request) override;

private:
    D3D12Context* m_context = nullptr;
};
} // namespace Prism::RHI::D3D12
