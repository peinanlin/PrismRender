#pragma once

#include "RHI/DeviceCapabilities.h"
#include "RHI/GraphicsResources.h"
#include "RHI/PipelineState.h"
#include "RHI/PipelineCreationStatistics.h"
#include "RHI/RayTracing.h"
#include "RHI/TransientResources.h"

namespace Prism::RHI
{
class IGraphicsDevice
{
public:
    virtual ~IGraphicsDevice() = default;

    [[nodiscard]] PipelineCreationStatistics GetPipelineCreationStatistics() const noexcept
    {
        return m_pipelineCreationCounters.Read();
    }

    virtual GraphicsApi GetGraphicsApi() const = 0;
    virtual ShaderBinaryFormat
        GetPreferredShaderBinaryFormat() const = 0;
    virtual const GraphicsDeviceCapabilities&
        GetCapabilities() const = 0;
    virtual DescriptorAllocatorStatistics
        GetDescriptorAllocatorStatistics() const = 0;
    virtual UploadQueueStatistics
        GetUploadQueueStatistics() const = 0;
    virtual UploadTicket GetPendingUploadTicket() const = 0;
    virtual bool IsUploadComplete(
        UploadTicket ticket) const = 0;
    virtual ResourceRetirementStatistics
        GetResourceRetirementStatistics() const = 0;
    virtual std::shared_ptr<IBuffer> CreateBuffer(
        const BufferDescription& description,
        const void* initialData = nullptr) = 0;
    virtual std::shared_ptr<ITexture> CreateTexture(
        const TextureDescription& description,
        const TextureInitialData* initialData = nullptr) = 0;
    virtual std::shared_ptr<ITransientTexturePool>
        CreateTransientTexturePool(
            const std::vector<TransientTextureRequest>&
                requests) = 0;
    virtual std::shared_ptr<ITransientBufferPool>
        CreateTransientBufferPool(
            const std::vector<TransientBufferRequest>&
                requests) = 0;
    virtual std::shared_ptr<ITextureView> CreateTextureView(
        std::shared_ptr<ITexture> texture,
        const TextureViewDescription& description) = 0;
    virtual std::shared_ptr<ISampler> CreateSampler(const SamplerDescription& description) = 0;
    virtual std::shared_ptr<IDescriptorSetLayout> CreateDescriptorSetLayout(
        const DescriptorSetLayoutDescription& description) = 0;
    virtual std::shared_ptr<IDescriptorSet> CreateDescriptorSet(
        std::shared_ptr<IDescriptorSetLayout> layout) = 0;
    virtual std::shared_ptr<IGraphicsPipeline> CreateGraphicsPipeline(
        const GraphicsPipelineDescription& description) = 0;
    virtual std::shared_ptr<IComputePipeline> CreateComputePipeline(
        const ComputePipelineDescription& description) = 0;
    virtual AccelerationStructureBuildSizes
        QueryAccelerationStructureBuildSizes(
            const AccelerationStructureBuildDescription&
                description) const = 0;
    virtual std::shared_ptr<
        IRayTracingAccelerationStructure>
        CreateAccelerationStructure(
            const AccelerationStructureBuildRequest&
                request) = 0;
protected:
    PipelineCreationCounters m_pipelineCreationCounters;
};
} // namespace Prism::RHI
