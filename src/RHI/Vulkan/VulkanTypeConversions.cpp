#include "RHI/Vulkan/VulkanTypeConversions.h"

#include <stdexcept>

namespace Prism::RHI::Vulkan
{
VkPipelineStageFlags QueueCompatiblePipelineStages(VkPipelineStageFlags stages, const CommandQueueType queue)
{
    if (queue != CommandQueueType::Compute) return stages;
    // Shared RHI shader states include graphics stages, but a dedicated
    // compute family must never name those stages in a pipeline barrier.
    constexpr VkPipelineStageFlags supported = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
        | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
        | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
        | VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    stages &= supported;
    return stages != 0 ? stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
}

VkFormat ToNativeFormat(const Format format)
{
    switch (format)
    {
    case Format::Unknown: return VK_FORMAT_UNDEFINED;
    case Format::R8Unorm: return VK_FORMAT_R8_UNORM;
    case Format::R16Float: return VK_FORMAT_R16_SFLOAT;
    case Format::R32Float: return VK_FORMAT_R32_SFLOAT;
    case Format::R32Typeless: return VK_FORMAT_R32_SFLOAT;
    case Format::Rg16Float: return VK_FORMAT_R16G16_SFLOAT;
    case Format::Rgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::Rgba8UnormSrgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::Bgra8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::Bgra8UnormSrgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::Rgba16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::Rgba32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::D32Float: return VK_FORMAT_D32_SFLOAT;
    }
    throw std::invalid_argument("Unsupported Prism format for Vulkan.");
}

VkImageUsageFlags ToNativeImageUsage(const TextureUsage usage)
{
    VkImageUsageFlags result = 0;
    if (HasAnyFlag(usage, TextureUsage::ShaderResource))
    {
        result |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (HasAnyFlag(usage, TextureUsage::RenderTarget))
    {
        result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (HasAnyFlag(usage, TextureUsage::DepthStencil))
    {
        result |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (HasAnyFlag(usage, TextureUsage::UnorderedAccess))
    {
        result |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (HasAnyFlag(usage, TextureUsage::CopySource))
    {
        result |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (HasAnyFlag(usage, TextureUsage::CopyDestination))
    {
        result |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    return result;
}

ResourceStateMapping ToNativeResourceState(const ResourceState state, const bool depthResource)
{
    ResourceStateMapping mapping{};
    if (state == ResourceState::Undefined)
    {
        return mapping;
    }

    mapping.pipelineStages = 0;
    mapping.accessMask = 0;

    if (HasAnyFlag(state, ResourceState::Present))
    {
        mapping.pipelineStages |= VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        mapping.imageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    if (HasAnyFlag(state, ResourceState::RenderTarget))
    {
        mapping.pipelineStages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        mapping.accessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        mapping.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (HasAnyFlag(state, ResourceState::DepthWrite))
    {
        mapping.pipelineStages |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        mapping.accessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        mapping.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    if (HasAnyFlag(state, ResourceState::DepthRead))
    {
        mapping.pipelineStages |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        mapping.accessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        mapping.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    }
    if (HasAnyFlag(state, ResourceState::ShaderResource))
    {
        mapping.pipelineStages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                  | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        mapping.accessMask |= VK_ACCESS_SHADER_READ_BIT;
        mapping.imageLayout = depthResource ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    if (HasAnyFlag(state, ResourceState::UnorderedAccess))
    {
        mapping.pipelineStages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        mapping.accessMask |= VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        mapping.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (HasAnyFlag(state, ResourceState::CopySource))
    {
        mapping.pipelineStages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        mapping.accessMask |= VK_ACCESS_TRANSFER_READ_BIT;
        mapping.imageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    if (HasAnyFlag(state, ResourceState::CopyDestination))
    {
        mapping.pipelineStages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        mapping.accessMask |= VK_ACCESS_TRANSFER_WRITE_BIT;
        mapping.imageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    if (HasAnyFlag(state, ResourceState::VertexBuffer))
    {
        mapping.pipelineStages |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        mapping.accessMask |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    }
    if (HasAnyFlag(state, ResourceState::IndexBuffer))
    {
        mapping.pipelineStages |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        mapping.accessMask |= VK_ACCESS_INDEX_READ_BIT;
    }
    if (HasAnyFlag(state, ResourceState::ConstantBuffer))
    {
        mapping.pipelineStages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                  | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        mapping.accessMask |= VK_ACCESS_UNIFORM_READ_BIT;
    }
    if (HasAnyFlag(state, ResourceState::IndirectArgument))
    {
        mapping.pipelineStages |=
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        mapping.accessMask |=
            VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    }
    if (HasAnyFlag(
            state,
            ResourceState::AccelerationStructure))
    {
        mapping.pipelineStages |=
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
            | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        mapping.accessMask |=
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
            | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    }
    if (HasAnyFlag(state, ResourceState::Common))
    {
        mapping.pipelineStages |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        mapping.accessMask |= VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        mapping.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    if (mapping.pipelineStages == 0)
    {
        mapping.pipelineStages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    return mapping;
}

VkFilter ToNativeFilter(const Filter filter)
{
    return filter == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerMipmapMode ToNativeMipmapMode(const Filter filter)
{
    return filter == Filter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

VkSamplerAddressMode ToNativeAddressMode(const AddressMode addressMode)
{
    switch (addressMode)
    {
    case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case AddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    throw std::invalid_argument("Unsupported Prism address mode for Vulkan.");
}

VkCompareOp ToNativeCompareOperation(const CompareOperation operation)
{
    switch (operation)
    {
    case CompareOperation::Never: return VK_COMPARE_OP_NEVER;
    case CompareOperation::Less: return VK_COMPARE_OP_LESS;
    case CompareOperation::Equal: return VK_COMPARE_OP_EQUAL;
    case CompareOperation::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOperation::Greater: return VK_COMPARE_OP_GREATER;
    case CompareOperation::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOperation::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOperation::Always: return VK_COMPARE_OP_ALWAYS;
    }
    throw std::invalid_argument("Unsupported Prism compare operation for Vulkan.");
}

VkPolygonMode ToNativePolygonMode(const FillMode fillMode)
{
    return fillMode == FillMode::Wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
}

VkCullModeFlags ToNativeCullMode(const CullMode cullMode)
{
    switch (cullMode)
    {
    case CullMode::None: return VK_CULL_MODE_NONE;
    case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    case CullMode::Back: return VK_CULL_MODE_BACK_BIT;
    }
    throw std::invalid_argument("Unsupported Prism cull mode for Vulkan.");
}

VkFrontFace ToNativeFrontFace(const FrontFace frontFace)
{
    return frontFace == FrontFace::CounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
}

VkBlendFactor ToNativeBlendFactor(const BlendFactor factor)
{
    switch (factor)
    {
    case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SourceColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::InverseSourceColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SourceAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::InverseSourceAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DestinationColor: return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::InverseDestinationColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::DestinationAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::InverseDestinationAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    throw std::invalid_argument("Unsupported Prism blend factor for Vulkan.");
}

VkBlendOp ToNativeBlendOperation(const BlendOperation operation)
{
    switch (operation)
    {
    case BlendOperation::Add: return VK_BLEND_OP_ADD;
    case BlendOperation::Subtract: return VK_BLEND_OP_SUBTRACT;
    case BlendOperation::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOperation::Minimum: return VK_BLEND_OP_MIN;
    case BlendOperation::Maximum: return VK_BLEND_OP_MAX;
    }
    throw std::invalid_argument("Unsupported Prism blend operation for Vulkan.");
}

VkShaderStageFlagBits ToNativeShaderStage(const ShaderStage stage)
{
    switch (stage)
    {
    case ShaderStage::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderStage::Hull: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case ShaderStage::Domain: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    case ShaderStage::Pixel: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
    case ShaderStage::RayGeneration: return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    case ShaderStage::AnyHit: return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    case ShaderStage::ClosestHit: return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    case ShaderStage::Miss: return VK_SHADER_STAGE_MISS_BIT_KHR;
    case ShaderStage::Intersection: return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
    case ShaderStage::Callable: return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    }
    throw std::invalid_argument("Unsupported Prism shader stage for Vulkan.");
}

VkSampleCountFlagBits ToNativeSampleCount(const std::uint32_t sampleCount)
{
    switch (sampleCount)
    {
    case 1: return VK_SAMPLE_COUNT_1_BIT;
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    case 32: return VK_SAMPLE_COUNT_32_BIT;
    case 64: return VK_SAMPLE_COUNT_64_BIT;
    default: throw std::invalid_argument("Unsupported Vulkan sample count.");
    }
}
} // namespace Prism::RHI::Vulkan
