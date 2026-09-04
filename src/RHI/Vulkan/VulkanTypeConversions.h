#pragma once

#include "RHI/GraphicsTypes.h"
#include "RHI/ICommandContext.h"
#include "RHI/ShaderTypes.h"

#include <glad/vulkan.h>

namespace Prism::RHI::Vulkan
{
struct ResourceStateMapping
{
    VkPipelineStageFlags pipelineStages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags accessMask = 0;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

VkFormat ToNativeFormat(Format format);
VkImageUsageFlags ToNativeImageUsage(TextureUsage usage);
ResourceStateMapping ToNativeResourceState(ResourceState state, bool depthResource = false);
VkPipelineStageFlags QueueCompatiblePipelineStages(VkPipelineStageFlags stages, CommandQueueType queue);
VkFilter ToNativeFilter(Filter filter);
VkSamplerMipmapMode ToNativeMipmapMode(Filter filter);
VkSamplerAddressMode ToNativeAddressMode(AddressMode addressMode);
VkCompareOp ToNativeCompareOperation(CompareOperation operation);
VkPolygonMode ToNativePolygonMode(FillMode fillMode);
VkCullModeFlags ToNativeCullMode(CullMode cullMode);
VkFrontFace ToNativeFrontFace(FrontFace frontFace);
VkBlendFactor ToNativeBlendFactor(BlendFactor factor);
VkBlendOp ToNativeBlendOperation(BlendOperation operation);
VkShaderStageFlagBits ToNativeShaderStage(ShaderStage stage);
VkSampleCountFlagBits ToNativeSampleCount(std::uint32_t sampleCount);
} // namespace Prism::RHI::Vulkan
