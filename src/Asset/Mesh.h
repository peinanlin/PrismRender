#pragma once

#include <cstdint>
#include <memory>

#include <DirectXMath.h>

namespace Prism::RHI
{
class IGraphicsDevice;
class ICommandContext;
class IBuffer;
}

namespace Prism::Asset
{
class MeshAsset;

struct MeshVertex
{
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT3 normal{0.0f, 1.0f, 0.0f};
    DirectX::XMFLOAT2 texCoord{0.0f, 0.0f};
    DirectX::XMFLOAT4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
};

class Mesh
{
public:
    static std::shared_ptr<Mesh> CreateCube(RHI::IGraphicsDevice& device);
    static std::shared_ptr<Mesh> CreateFromAsset(RHI::IGraphicsDevice& device, const MeshAsset& meshAsset);

    void BindGeometry(RHI::ICommandContext& commandContext) const;
    void Draw(RHI::ICommandContext& commandContext) const;
    void DrawInstanced(RHI::ICommandContext& commandContext, std::uint32_t instanceCount) const;
    const std::shared_ptr<RHI::IBuffer>& GetRhiVertexBuffer() const;
    const std::shared_ptr<RHI::IBuffer>& GetRhiIndexBuffer() const;
    std::uint32_t GetRhiIndexCount() const;
    const DirectX::XMFLOAT3& GetBoundsCenter() const;
    float GetBoundsRadius() const;

private:
    void Initialize(
        RHI::IGraphicsDevice& device,
        const MeshVertex* vertices,
        std::uint32_t vertexCount,
        const std::uint16_t* indices,
        std::uint32_t indexCount,
        const DirectX::XMFLOAT3& boundsCenter,
        float boundsRadius);

    std::shared_ptr<RHI::IBuffer> m_rhiVertexBuffer;
    std::shared_ptr<RHI::IBuffer> m_rhiIndexBuffer;
    std::uint32_t m_rhiIndexCount = 0;
    DirectX::XMFLOAT3 m_boundsCenter{};
    float m_boundsRadius = 1.0f;
};
} // namespace Prism::Asset
