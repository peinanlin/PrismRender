#include "Asset/Mesh.h"

#include "Asset/MeshAsset.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ICommandContext.h"

namespace Prism::Asset
{
std::shared_ptr<Mesh> Mesh::CreateCube(RHI::IGraphicsDevice& device)
{
    return CreateFromAsset(device, *MeshAsset::CreateCube());
}

std::shared_ptr<Mesh> Mesh::CreateFromAsset(RHI::IGraphicsDevice& device, const MeshAsset& meshAsset)
{
    std::shared_ptr<Mesh> mesh = std::make_shared<Mesh>();
    mesh->Initialize(
        device,
        meshAsset.GetVertices().data(),
        static_cast<std::uint32_t>(meshAsset.GetVertices().size()),
        meshAsset.GetIndices().data(),
        static_cast<std::uint32_t>(meshAsset.GetIndices().size()),
        {
            (meshAsset.GetBounds().min.x + meshAsset.GetBounds().max.x) * 0.5f,
            (meshAsset.GetBounds().min.y + meshAsset.GetBounds().max.y) * 0.5f,
            (meshAsset.GetBounds().min.z + meshAsset.GetBounds().max.z) * 0.5f},
        meshAsset.GetBounds().radius);
    mesh->m_rhiVertexBuffer->SetDebugName(
        meshAsset.GetName() + ".VertexBuffer");
    mesh->m_rhiIndexBuffer->SetDebugName(
        meshAsset.GetName() + ".IndexBuffer");
    return mesh;
}

void Mesh::BindGeometry(RHI::ICommandContext& commandContext) const
{
    commandContext.BindVertexBuffer(*m_rhiVertexBuffer);
    commandContext.BindIndexBuffer(*m_rhiIndexBuffer, RHI::IndexFormat::UInt16);
}

void Mesh::Draw(RHI::ICommandContext& commandContext) const
{
    DrawInstanced(commandContext, 1);
}

void Mesh::DrawInstanced(RHI::ICommandContext& commandContext, const std::uint32_t instanceCount) const
{
    BindGeometry(commandContext);
    commandContext.DrawIndexed(m_rhiIndexCount, instanceCount);
}

const std::shared_ptr<RHI::IBuffer>& Mesh::GetRhiVertexBuffer() const
{
    return m_rhiVertexBuffer;
}

const std::shared_ptr<RHI::IBuffer>& Mesh::GetRhiIndexBuffer() const
{
    return m_rhiIndexBuffer;
}

std::uint32_t Mesh::GetRhiIndexCount() const
{
    return m_rhiIndexCount;
}

const DirectX::XMFLOAT3& Mesh::GetBoundsCenter() const
{
    return m_boundsCenter;
}

float Mesh::GetBoundsRadius() const
{
    return m_boundsRadius;
}

void Mesh::Initialize(
    RHI::IGraphicsDevice& device,
    const MeshVertex* vertices,
    const std::uint32_t vertexCount,
    const std::uint16_t* indices,
    const std::uint32_t indexCount,
    const DirectX::XMFLOAT3& boundsCenter,
    const float boundsRadius)
{
    RHI::BufferDescription vertexDescription{};
    vertexDescription.size = sizeof(MeshVertex) * vertexCount;
    vertexDescription.stride = sizeof(MeshVertex);
    vertexDescription.usage = RHI::BufferUsage::Vertex;
    vertexDescription.memoryAccess = RHI::MemoryAccess::GpuOnly;
    m_rhiVertexBuffer = device.CreateBuffer(vertexDescription, vertices);

    RHI::BufferDescription indexDescription{};
    indexDescription.size = sizeof(std::uint16_t) * indexCount;
    indexDescription.stride = sizeof(std::uint16_t);
    indexDescription.usage = RHI::BufferUsage::Index;
    indexDescription.memoryAccess = RHI::MemoryAccess::GpuOnly;
    m_rhiIndexBuffer = device.CreateBuffer(indexDescription, indices);
    m_rhiIndexCount = indexCount;
    m_boundsCenter = boundsCenter;
    m_boundsRadius = boundsRadius;
}
} // namespace Prism::Asset
