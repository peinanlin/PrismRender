#pragma once

#include "RHI/GraphicsResources.h"
#include "RHI/ShaderTypes.h"

#include <span>

namespace Prism::RHI
{
struct ShaderLayoutStage
{
    const ShaderReflection* reflection = nullptr;
    ShaderStage stage = ShaderStage::Vertex;
};

DescriptorSetLayoutDescription BuildDescriptorSetLayout(
    std::span<const ShaderLayoutStage> stages,
    std::span<const std::uint32_t> dynamicConstantBufferBindings = {});
} // namespace Prism::RHI
