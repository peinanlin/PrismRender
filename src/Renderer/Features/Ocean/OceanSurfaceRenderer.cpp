#include "Renderer/Features/Ocean/OceanSurfaceRenderer.h"

#include "Asset/Mesh.h"
#include "Core/Assert.h"
#include "Core/DiagnosticLog.h"
#include "Renderer/Features/Ocean/OceanSettings.h"
#include "RHI/IGraphicsDevice.h"
#include "Scene/Camera.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace Prism::Renderer
{
namespace
{
OceanQuadtreeFrustum BuildFrustum(const Scene::Camera& camera)
{
    DirectX::XMFLOAT4X4 matrix{};
    DirectX::XMStoreFloat4x4(&matrix, camera.GetViewProjectionMatrix());
    const auto column = [&matrix](const std::uint32_t index)
    {
        return DirectX::XMFLOAT4{matrix.m[0][index], matrix.m[1][index],
            matrix.m[2][index], matrix.m[3][index]};
    };
    const auto normalize = [](DirectX::XMFLOAT4 plane)
    {
        const float length = std::sqrt(
            plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (length > 1.0e-6f)
        {
            plane.x /= length;
            plane.y /= length;
            plane.z /= length;
            plane.w /= length;
        }
        return plane;
    };
    const auto combine = [&normalize](const DirectX::XMFLOAT4& a,
                             const DirectX::XMFLOAT4& b,
                             const float sign)
    {
        return normalize({a.x + sign * b.x, a.y + sign * b.y,
            a.z + sign * b.z, a.w + sign * b.w});
    };

    // Prism transforms row vectors. D3D's depth interval is [0,w], hence the
    // near plane is column 3 instead of column 4 + column 3.
    const DirectX::XMFLOAT4 x = column(0u);
    const DirectX::XMFLOAT4 y = column(1u);
    const DirectX::XMFLOAT4 z = column(2u);
    const DirectX::XMFLOAT4 w = column(3u);
    OceanQuadtreeFrustum frustum{};
    frustum.enabled = true;
    frustum.planes = {combine(w, x, 1.0f), combine(w, x, -1.0f),
        combine(w, y, 1.0f), combine(w, y, -1.0f), normalize(z),
        combine(w, z, -1.0f)};
    return frustum;
}
} // namespace

void OceanSurfaceRenderer::Initialize(
    RHI::IGraphicsDevice& device, const std::uint32_t framesInFlight)
{
    Core::DiagnosticLog::Write("debug", "ocean_geometry_initialize_begin",
        "Initializing adaptive ocean geometry resources.",
        {{"framesInFlight", framesInFlight}});
    Core::Check(framesInFlight > 0u,
        "Ocean adaptive geometry requires at least one frame in flight.");
    Reset();
    m_framesInFlight = framesInFlight;
    m_instanceBuffers.resize(framesInFlight);
    RHI::BufferDescription description{};
    description.size = static_cast<std::size_t>(MaximumAdaptiveNodes)
        * sizeof(OceanPatchInstanceData);
    description.stride = sizeof(OceanPatchInstanceData);
    description.usage = RHI::BufferUsage::ShaderResource;
    description.memoryAccess = RHI::MemoryAccess::CpuToGpu;
    std::uint32_t bufferIndex = 0u;
    for (auto& buffer : m_instanceBuffers)
    {
        buffer = device.CreateBuffer(description);
        buffer->SetDebugName("Ocean adaptive patch instances");
        Core::DiagnosticLog::Write("debug", "ocean_geometry_instance_buffer",
            "Adaptive ocean instance buffer is ready.",
            {{"bufferIndex", bufferIndex++}});
    }
    Core::DiagnosticLog::Write("debug", "ocean_geometry_initialize_end",
        "Adaptive ocean geometry resources are initialized.");
}

void OceanSurfaceRenderer::Reset() noexcept
{
    m_framesInFlight = 0u;
    m_cellsPerPatch = 0u;
    m_vertexBuffer.reset();
    m_indexBuffers.fill(nullptr);
    m_indexCounts.fill(0u);
    m_instanceBuffers.clear();
    m_drawGroups.clear();
    m_unifiedDrawGroup = {};
    m_selection = {};
}

void OceanSurfaceRenderer::RebuildPatchGeometry(
    RHI::IGraphicsDevice& device, const std::uint32_t cellsPerPatch)
{
    Core::DiagnosticLog::Write("debug", "ocean_geometry_rebuild_begin",
        "Rebuilding adaptive ocean patch topology.");
    const std::uint32_t cells = std::clamp(cellsPerPatch, 1u, 256u);
    const OceanPatchGeometry base = OceanQuadtree::BuildPatchGeometry(
        cells, OceanPatchEdge::None);
    std::vector<Asset::MeshVertex> vertices;
    vertices.reserve(base.vertices.size());
    for (const DirectX::XMFLOAT2 uv : base.vertices)
    {
        Asset::MeshVertex vertex{};
        vertex.position = {uv.x * 2.0f - 1.0f, 0.0f, uv.y * 2.0f - 1.0f};
        vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
        vertex.normal = {0.0f, 1.0f, 0.0f};
        vertex.texCoord = uv;
        vertex.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
        vertices.push_back(vertex);
    }

    RHI::BufferDescription vertexDescription{};
    vertexDescription.size = vertices.size() * sizeof(Asset::MeshVertex);
    vertexDescription.stride = sizeof(Asset::MeshVertex);
    vertexDescription.usage = RHI::BufferUsage::Vertex;
    // Geometry variants can be rebuilt from the WaveWorks Lab while a frame
    // is being recorded.  A GpuOnly buffer with initial data would require an
    // immediate copy submission here; on D3D12 that can wait on the same
    // frame whose command list has not been submitted yet.  The variants are
    // small and changed rarely, so persistently uploadable VB/IB resources
    // avoid the frame-internal queue dependency on every backend.
    vertexDescription.memoryAccess = RHI::MemoryAccess::CpuToGpu;
    m_vertexBuffer = device.CreateBuffer(vertexDescription, vertices.data());
    m_vertexBuffer->SetDebugName("Ocean adaptive patch vertices");

    for (std::uint32_t mask = 0u; mask < m_indexBuffers.size(); ++mask)
    {
        const OceanPatchGeometry variant = OceanQuadtree::BuildPatchGeometry(
            cells, static_cast<OceanPatchEdge>(mask));
        Core::Check(variant.indices.size()
                <= std::numeric_limits<std::uint32_t>::max(),
            "Ocean adaptive patch index count exceeds the RHI draw range.");
        RHI::BufferDescription indexDescription{};
        indexDescription.size = variant.indices.size() * sizeof(std::uint32_t);
        indexDescription.stride = sizeof(std::uint32_t);
        indexDescription.usage = RHI::BufferUsage::Index;
        indexDescription.memoryAccess = RHI::MemoryAccess::CpuToGpu;
        m_indexBuffers[mask] =
            device.CreateBuffer(indexDescription, variant.indices.data());
        m_indexBuffers[mask]->SetDebugName("Ocean adaptive patch indices");
        m_indexCounts[mask] =
            static_cast<std::uint32_t>(variant.indices.size());
    }
    m_cellsPerPatch = cells;
    Core::DiagnosticLog::Write("debug", "ocean_geometry_rebuild_end",
        "Adaptive ocean patch topology is ready.",
        {{"cellsPerPatch", cells},
            {"vertexCount", vertices.size()}});
}

bool OceanSurfaceRenderer::UpdateAdaptiveGeometry(
    RHI::IGraphicsDevice& device,
    const OceanGeometrySettings& settings,
    const Scene::Camera& camera,
    const std::uint32_t frameIndex,
    const std::uint32_t viewportHeight,
    const float conservativeDisplacementMeters)
{
    Core::Check(m_framesInFlight > 0u && frameIndex < m_framesInFlight,
        "Ocean adaptive geometry frame index is invalid.");
    if (m_cellsPerPatch != std::clamp(settings.cellsPerPatch, 1u, 256u))
        RebuildPatchGeometry(device, settings.cellsPerPatch);

    OceanQuadtreeSettings selectionSettings{};
    selectionSettings.minimumPatchSizeMeters = settings.minimumPatchLength;
    selectionSettings.maximumScreenEdgePixels = settings.maximumEdgeLengthPixels;
    selectionSettings.maximumViewDistanceMeters = MaximumAdaptiveViewDistanceMeters;
    selectionSettings.conservativeDisplacementMeters =
        std::max(conservativeDisplacementMeters, 0.0f);
    selectionSettings.maximumLod = settings.maximumLod;
    selectionSettings.maximumNodeCount = MaximumAdaptiveNodes;
    selectionSettings.cellsPerPatch = settings.cellsPerPatch;
    selectionSettings.viewportHeightPixels = std::max(viewportHeight, 1u);
    selectionSettings.verticalFieldOfViewRadians =
        camera.GetFieldOfViewYRadians();
    selectionSettings.meanSeaLevelMeters = settings.meanSeaLevel;

    OceanQuadtreeFrustum frustum = BuildFrustum(camera);
    m_selection = OceanQuadtree::Select(selectionSettings,
        camera.GetPosition(), camera.GetFarPlane(), frustum);
    std::vector<OceanPatchInstanceData> instances =
        OceanQuadtree::BuildInstanceData(m_selection);
    Core::Check(instances.size() <= MaximumAdaptiveNodes,
        "Ocean adaptive instance upload exceeded its fixed buffer capacity.");
    if (instances.empty())
    {
        m_drawGroups.clear();
        m_unifiedDrawGroup = {};
        return false;
    }
    m_instanceBuffers[frameIndex]->Update(
        instances.data(), instances.size() * sizeof(OceanPatchInstanceData));

    m_drawGroups.clear();
    const std::vector<OceanQuadtreeNode> grouped =
        OceanQuadtree::GroupForInstances(m_selection);
    for (std::size_t first = 0u; first < grouped.size();)
    {
        const OceanPatchEdge mask = grouped[first].edgeMask;
        std::size_t end = first + 1u;
        while (end < grouped.size() && grouped[end].edgeMask == mask)
            ++end;
        const std::uint32_t maskIndex = static_cast<std::uint8_t>(mask);
        m_drawGroups.push_back({mask,
            static_cast<std::uint32_t>(first),
            static_cast<std::uint32_t>(end - first),
            m_indexBuffers[maskIndex], m_indexCounts[maskIndex]});
        first = end;
    }
    // Keep the topology groups for statistics/debugging, but render through
    // one regular grid. The vertex shader applies the finite edge mask from
    // each instance. This avoids treating StartInstanceLocation as a portable
    // StructuredBuffer base index (D3D12 and Vulkan expose different system-
    // value behavior) and reduces as many as sixteen draws to one.
    m_unifiedDrawGroup = {OceanPatchEdge::None, 0u,
        static_cast<std::uint32_t>(instances.size()), m_indexBuffers[0],
        m_indexCounts[0]};
    return true;
}

const std::shared_ptr<RHI::IBuffer>&
OceanSurfaceRenderer::GetVertexBuffer() const noexcept
{
    return m_vertexBuffer;
}

const std::shared_ptr<RHI::IBuffer>&
OceanSurfaceRenderer::GetInstanceBuffer(const std::uint32_t frameIndex) const
{
    Core::Check(frameIndex < m_instanceBuffers.size(),
        "Ocean adaptive instance-buffer frame index is invalid.");
    return m_instanceBuffers[frameIndex];
}

const std::vector<OceanAdaptiveDrawGroup>&
OceanSurfaceRenderer::GetDrawGroups() const noexcept
{
    return m_drawGroups;
}

const OceanAdaptiveDrawGroup&
OceanSurfaceRenderer::GetUnifiedDrawGroup() const noexcept
{
    return m_unifiedDrawGroup;
}

const OceanQuadtreeSelection&
OceanSurfaceRenderer::GetSelection() const noexcept
{
    return m_selection;
}

bool OceanSurfaceRenderer::IsReady() const noexcept
{
    return m_vertexBuffer != nullptr && !m_drawGroups.empty();
}
} // namespace Prism::Renderer
