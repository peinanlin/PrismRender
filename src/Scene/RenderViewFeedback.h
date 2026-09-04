#pragma once

#include "Scene/RenderFramePacket.h"

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Prism::Scene
{
struct RenderViewFeedbackIdentity
{
    SceneGeneration sceneGeneration;
    RenderSceneDataRevision dataRevision;
    RenderViewId viewId;
    LogicalFrameId logicalFrameId;
};

// Delayed GPU diagnostics are immutable and identify the exact object-index
// mapping that produced them. A newer logical frame may consume the feedback
// only while it still references the same scene/data revision and source view.
class RenderViewFeedback final
{
public:
    RenderViewFeedback(
        RenderViewFeedbackIdentity identity,
        std::vector<GpuVisibilityReason> visibilityReasons,
        Camera cullingCamera)
        : m_identity(identity),
          m_visibilityReasons(std::move(visibilityReasons)),
          m_cullingCamera(std::move(cullingCamera))
    {
        if (!m_identity.sceneGeneration
            || !m_identity.dataRevision
            || !m_identity.viewId
            || !m_identity.logicalFrameId)
        {
            throw std::invalid_argument(
                "RenderViewFeedback requires complete source identity.");
        }
    }

    [[nodiscard]] const RenderViewFeedbackIdentity&
        GetIdentity() const noexcept
    {
        return m_identity;
    }

    [[nodiscard]] const std::vector<GpuVisibilityReason>&
        GetVisibilityReasons() const noexcept
    {
        return m_visibilityReasons;
    }

    [[nodiscard]] const Camera& GetCullingCamera() const noexcept
    {
        return m_cullingCamera;
    }

    [[nodiscard]] bool Matches(
        const RenderFramePacket& packet,
        const RenderViewId expectedSourceView) const noexcept
    {
        const std::shared_ptr<const RenderSceneData>& data =
            packet.GetSceneData();
        return data != nullptr
            && m_identity.sceneGeneration
                == data->GetSceneGeneration()
            && m_identity.dataRevision
                == data->GetDataRevision()
            && m_identity.viewId == expectedSourceView
            && m_identity.logicalFrameId.value
                <= packet.GetLogicalFrameId().value
            && m_visibilityReasons.size()
                == data->GetRenderObjects().size();
    }

private:
    RenderViewFeedbackIdentity m_identity;
    std::vector<GpuVisibilityReason> m_visibilityReasons;
    Camera m_cullingCamera;
};
} // namespace Prism::Scene
