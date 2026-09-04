#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Prism::RHI
{
    enum class ShaderStage
    {
        Vertex,
        Hull,
        Domain,
        Pixel,
        Compute,
        RayGeneration,
        AnyHit,
        ClosestHit,
        Miss,
        Intersection,
        Callable
    };

    enum class ShaderBinaryFormat
    {
        Dxil,
        SpirV,
        Dxbc,
        MetalSource
    };

    enum class ShaderResourceKind
    {
        Unknown,
        ConstantBuffer,
        ShaderResource,
        UnorderedAccess,
        Sampler,
        PushConstant
    };

    enum class ShaderResourceShape
    {
        Unknown,
        Buffer,
        Texture,
        AccelerationStructure
    };

    struct ShaderResourceBinding
    {
        std::string name;
        ShaderResourceKind kind = ShaderResourceKind::Unknown;
        ShaderResourceShape shape =
            ShaderResourceShape::Unknown;
        // Canonical RHI binding: b=0..15, t=16..31, u=32..47,
        // s=48..63. Shader compilers normalize backend register indices.
        std::uint32_t bindingIndex = 0;
        std::uint32_t bindingSpace = 0;
        std::size_t byteSize = 0;
    };

    struct ShaderReflection
    {
        std::vector<ShaderResourceBinding> resources;
    };

    struct ShaderBinary
    {
        ShaderStage stage = ShaderStage::Vertex;
        ShaderBinaryFormat format = ShaderBinaryFormat::Dxil;
        std::string entryPoint;
        std::string emittedEntryPoint;
        std::vector<std::uint8_t> bytecode;
        ShaderReflection reflection;
        std::string diagnostics;

        const void* Data() const;
        std::size_t Size() const;
        bool IsValid() const;
    };

    std::string_view ToString(ShaderStage stage);
    std::string_view ToString(ShaderBinaryFormat format);
} // namespace Prism::RHI
