#pragma once

#include "RHI/GraphicsResources.h"
#include "RHI/ShaderTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Prism::RHI
{
enum class PrimitiveTopology
{
    TriangleList,
    TriangleStrip,
    LineList,
    PatchList
};

enum class VertexElementFormat
{
    Float,
    Float2,
    Float3,
    Float4,
    Uint
};

enum class VertexInputRate
{
    PerVertex,
    PerInstance
};

struct VertexBufferBindingDescription
{
    std::uint32_t binding = 0;
    std::uint32_t stride = 0;
    VertexInputRate inputRate = VertexInputRate::PerVertex;
};

struct VertexAttributeDescription
{
    std::uint32_t location = 0;
    std::uint32_t binding = 0;
    VertexElementFormat format = VertexElementFormat::Float3;
    std::uint32_t offset = 0;
    std::string semanticName;
    std::uint32_t semanticIndex = 0;
};

struct GraphicsPipelineDescription
{
    ShaderBinary vertexShader;
    ShaderBinary hullShader;
    ShaderBinary domainShader;
    ShaderBinary pixelShader;
    std::shared_ptr<IDescriptorSetLayout> descriptorSetLayout;
    std::vector<VertexBufferBindingDescription> vertexBindings;
    std::vector<VertexAttributeDescription> vertexAttributes;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    std::uint32_t patchControlPointCount = 0u;
    RasterizerDescription rasterizer{};
    DepthStencilDescription depthStencil{};
    std::vector<BlendAttachmentDescription> blendAttachments{BlendAttachmentDescription{}};
    std::vector<Format> colorFormats{Format::Rgba8Unorm};
    Format depthFormat = Format::D32Float;
    std::uint32_t sampleCount = 1;
};

struct ComputePipelineDescription
{
    ShaderBinary computeShader;
    std::shared_ptr<IDescriptorSetLayout> descriptorSetLayout;
};

struct TextureBarrier
{
    ITexture* texture = nullptr;
    ResourceState before = ResourceState::Undefined;
    ResourceState after = ResourceState::Undefined;
    std::uint32_t baseMipLevel = 0;
    std::uint32_t mipLevelCount = 0;
    std::uint32_t baseArrayLayer = 0;
    std::uint32_t arrayLayerCount = 0;
};

struct BufferBarrier
{
    IBuffer* buffer = nullptr;
    ResourceState before =
        ResourceState::Undefined;
    ResourceState after =
        ResourceState::Undefined;
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct GlobalBarrier
{
    ResourceState before =
        ResourceState::UnorderedAccess;
    ResourceState after =
        ResourceState::UnorderedAccess;
};

class IGraphicsPipeline : public IGraphicsResource
{
public:
    ~IGraphicsPipeline() override = default;
};

class IComputePipeline : public IGraphicsResource
{
public:
    ~IComputePipeline() override = default;
};

std::uint32_t GetVertexElementSize(VertexElementFormat format);
bool ValidateGraphicsPipelineDescription(
    const GraphicsPipelineDescription& description,
    std::string* outError = nullptr);
bool ValidateComputePipelineDescription(
    const ComputePipelineDescription& description,
    std::string* outError = nullptr);
} // namespace Prism::RHI
