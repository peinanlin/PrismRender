#pragma once

#include "Renderer/Features/Ocean/OceanQuadtree.h"
#include "RHI/GraphicsResources.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Prism::RHI
{
class IGraphicsDevice;
}

namespace Prism::Scene
{
class Camera;
}

namespace Prism::Renderer
{
struct OceanGeometrySettings;

struct OceanAdaptiveDrawGroup
{
    OceanPatchEdge edgeMask = OceanPatchEdge::None;
    std::uint32_t firstInstance = 0u;
    std::uint32_t instanceCount = 0u;
    std::shared_ptr<RHI::IBuffer> indexBuffer;
    std::uint32_t indexCount = 0u;
};

// View-dependent adaptive geometry. Spectral maps remain shared between
// editor views, but every viewport selects and uploads its own camera LODs.
class OceanSurfaceRenderer
{
public:
    static constexpr std::uint32_t MaximumAdaptiveNodes = 4096u;
    static constexpr float MaximumAdaptiveViewDistanceMeters = 4000.0f;

    void Initialize(
        RHI::IGraphicsDevice& device, std::uint32_t framesInFlight);
    void Reset() noexcept;
    bool UpdateAdaptiveGeometry(
        RHI::IGraphicsDevice& device,
        const OceanGeometrySettings& settings,
        const Scene::Camera& camera,
        std::uint32_t frameIndex,
        std::uint32_t viewportHeight,
        float conservativeDisplacementMeters);

    [[nodiscard]] const std::shared_ptr<RHI::IBuffer>&
        GetVertexBuffer() const noexcept;
    [[nodiscard]] const std::shared_ptr<RHI::IBuffer>&
        GetInstanceBuffer(std::uint32_t frameIndex) const;
    [[nodiscard]] const std::vector<OceanAdaptiveDrawGroup>&
        GetDrawGroups() const noexcept;
    [[nodiscard]] const OceanAdaptiveDrawGroup&
        GetUnifiedDrawGroup() const noexcept;
    [[nodiscard]] const OceanQuadtreeSelection&
        GetSelection() const noexcept;
    [[nodiscard]] bool IsReady() const noexcept;

private:
    void RebuildPatchGeometry(
        RHI::IGraphicsDevice& device, std::uint32_t cellsPerPatch);

    std::uint32_t m_framesInFlight = 0u;
    std::uint32_t m_cellsPerPatch = 0u;
    std::shared_ptr<RHI::IBuffer> m_vertexBuffer;
    std::array<std::shared_ptr<RHI::IBuffer>, 16> m_indexBuffers{};
    std::array<std::uint32_t, 16> m_indexCounts{};
    std::vector<std::shared_ptr<RHI::IBuffer>> m_instanceBuffers;
    std::vector<OceanAdaptiveDrawGroup> m_drawGroups;
    OceanAdaptiveDrawGroup m_unifiedDrawGroup;
    OceanQuadtreeSelection m_selection;
};
} // namespace Prism::Renderer
