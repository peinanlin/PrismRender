#include "RHI/ShaderTypes.h"

namespace Prism::RHI
{
const void* ShaderBinary::Data() const
{
    return bytecode.empty() ? nullptr : bytecode.data();
}

std::size_t ShaderBinary::Size() const
{
    return bytecode.size();
}

bool ShaderBinary::IsValid() const
{
    return !bytecode.empty() && !entryPoint.empty();
}

std::string_view ToString(const ShaderStage stage)
{
    switch (stage)
    {
    case ShaderStage::Vertex: return "Vertex";
    case ShaderStage::Hull: return "Hull";
    case ShaderStage::Domain: return "Domain";
    case ShaderStage::Pixel: return "Pixel";
    case ShaderStage::Compute: return "Compute";
    case ShaderStage::RayGeneration: return "Ray Generation";
    case ShaderStage::AnyHit: return "Any Hit";
    case ShaderStage::ClosestHit: return "Closest Hit";
    case ShaderStage::Miss: return "Miss";
    case ShaderStage::Intersection: return "Intersection";
    case ShaderStage::Callable: return "Callable";
    }
    return "Unknown";
}

std::string_view ToString(const ShaderBinaryFormat format)
{
    switch (format)
    {
    case ShaderBinaryFormat::Dxil: return "DXIL";
    case ShaderBinaryFormat::SpirV: return "SPIR-V";
    case ShaderBinaryFormat::Dxbc: return "DXBC";
    case ShaderBinaryFormat::MetalSource: return "Metal Source";
    }
    return "Unknown";
}
} // namespace Prism::RHI
