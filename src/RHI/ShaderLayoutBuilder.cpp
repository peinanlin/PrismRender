#include "RHI/ShaderLayoutBuilder.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace Prism::RHI
{
namespace
{
ShaderStageFlags ToStageFlags(const ShaderStage stage)
{
    switch (stage)
    {
    case ShaderStage::Vertex: return ShaderStageFlags::Vertex;
    case ShaderStage::Hull: return ShaderStageFlags::Hull;
    case ShaderStage::Domain: return ShaderStageFlags::Domain;
    case ShaderStage::Pixel: return ShaderStageFlags::Pixel;
    case ShaderStage::Compute: return ShaderStageFlags::Compute;
    case ShaderStage::RayGeneration: return ShaderStageFlags::RayGeneration;
    case ShaderStage::AnyHit: return ShaderStageFlags::AnyHit;
    case ShaderStage::ClosestHit: return ShaderStageFlags::ClosestHit;
    case ShaderStage::Miss: return ShaderStageFlags::Miss;
    case ShaderStage::Intersection: return ShaderStageFlags::Intersection;
    case ShaderStage::Callable: return ShaderStageFlags::Callable;
    }
    return ShaderStageFlags::None;
}

std::uint32_t ToCanonicalBinding(const ShaderResourceBinding& resource)
{
    return resource.bindingIndex;
}

DescriptorType ToDescriptorType(
    const ShaderResourceBinding& resource,
    const std::span<const std::uint32_t> dynamicBindings)
{
    switch (resource.kind)
    {
    case ShaderResourceKind::ConstantBuffer:
        return std::ranges::find(dynamicBindings, resource.bindingIndex) != dynamicBindings.end()
            ? DescriptorType::DynamicConstantBuffer
            : DescriptorType::ConstantBuffer;
    case ShaderResourceKind::ShaderResource:
        if (resource.shape
            == ShaderResourceShape::
                AccelerationStructure)
        {
            return DescriptorType::
                AccelerationStructure;
        }
        if (resource.shape
            == ShaderResourceShape::Buffer)
        {
            return DescriptorType::
                ReadOnlyStorageBuffer;
        }
        return DescriptorType::SampledTexture;
    case ShaderResourceKind::UnorderedAccess:
        return resource.shape
                   == ShaderResourceShape::Buffer
            ? DescriptorType::StorageBuffer
            : DescriptorType::StorageTexture;
    case ShaderResourceKind::Sampler: return DescriptorType::Sampler;
    default: break;
    }
    throw std::runtime_error("Slang reflection resource cannot be represented by the graphics RHI.");
}
} // namespace

DescriptorSetLayoutDescription BuildDescriptorSetLayout(
    const std::span<const ShaderLayoutStage> stages,
    const std::span<const std::uint32_t> dynamicConstantBufferBindings)
{
    std::unordered_map<std::uint32_t, DescriptorBindingDescription> mergedBindings;
    for (const ShaderLayoutStage& stage : stages)
    {
        if (stage.reflection == nullptr)
        {
            continue;
        }
        for (const ShaderResourceBinding& resource : stage.reflection->resources)
        {
            if (resource.kind == ShaderResourceKind::Unknown
                || resource.kind == ShaderResourceKind::PushConstant)
            {
                continue;
            }
            const std::uint32_t binding = ToCanonicalBinding(resource);
            const DescriptorType type = ToDescriptorType(resource, dynamicConstantBufferBindings);
            const ShaderStageFlags stageFlags = ToStageFlags(stage.stage);
            auto [iterator, inserted] = mergedBindings.emplace(
                binding,
                DescriptorBindingDescription{binding, type, 1u, stageFlags});
            if (!inserted)
            {
                if (iterator->second.type != type)
                {
                    throw std::runtime_error(
                        "Slang reflection reported conflicting descriptor types at binding "
                        + std::to_string(binding)
                        + " for resource '"
                        + resource.name
                        + "' (existing type "
                        + std::to_string(static_cast<int>(
                            iterator->second.type))
                        + ", incoming type "
                        + std::to_string(static_cast<int>(type))
                        + ").");
                }
                iterator->second.stages = iterator->second.stages | stageFlags;
            }
        }
    }

    DescriptorSetLayoutDescription result{};
    result.bindings.reserve(mergedBindings.size());
    for (const auto& [binding, description] : mergedBindings)
    {
        (void)binding;
        result.bindings.push_back(description);
    }
    std::ranges::sort(result.bindings, {}, &DescriptorBindingDescription::binding);
    return result;
}
} // namespace Prism::RHI
