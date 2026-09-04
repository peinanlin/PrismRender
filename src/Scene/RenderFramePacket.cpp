#include "Scene/RenderFramePacket.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace Prism::Scene
{
namespace
{
bool HasEnergy(
    const DirectX::XMFLOAT3& color,
    const float intensity)
{
    return intensity > 0.0f
        && (color.x > 0.0f || color.y > 0.0f || color.z > 0.0f);
}
} // namespace

RenderFramePacket::RenderFramePacket(
    const LogicalFrameId logicalFrameId,
    const double simulationTimeSeconds,
    std::shared_ptr<const RenderSceneData> sceneData,
    RenderFrameDynamicData dynamicData,
    std::vector<RenderView> views,
    const double simulationDeltaSeconds)
    : m_logicalFrameId(logicalFrameId),
      m_simulationTimeSeconds(simulationTimeSeconds),
      m_simulationDeltaSeconds(simulationDeltaSeconds),
      m_sceneData(std::move(sceneData)),
      m_dynamicData(std::move(dynamicData)),
      m_views(std::move(views))
{
    if (!m_logicalFrameId)
    {
        throw std::invalid_argument(
            "RenderFramePacket requires a non-zero logical frame ID.");
    }
    if (!std::isfinite(m_simulationTimeSeconds)
        || m_simulationTimeSeconds < 0.0)
    {
        throw std::invalid_argument(
            "RenderFramePacket simulation time must be finite and non-negative.");
    }
    if (!std::isfinite(m_simulationDeltaSeconds)
        || m_simulationDeltaSeconds < 0.0)
    {
        throw std::invalid_argument(
            "RenderFramePacket simulation step must be finite and non-negative.");
    }
    if (m_sceneData == nullptr)
    {
        throw std::invalid_argument(
            "RenderFramePacket requires immutable scene data.");
    }
    if (m_views.empty())
    {
        throw std::invalid_argument(
            "RenderFramePacket requires at least one view.");
    }
    if (m_dynamicData.activePointLightCount
            > m_dynamicData.pointLights.size()
        || m_dynamicData.activeSpotLightCount
            > m_dynamicData.spotLights.size())
    {
        throw std::invalid_argument(
            "RenderFramePacket active light count exceeds capacity.");
    }

    m_featureUsage.scene = m_sceneData->GetFeatureUsage();
    m_featureUsage.hasDirectionalLight = HasEnergy(
        m_dynamicData.directionalLight.color,
        m_dynamicData.directionalLight.intensity);
    for (std::uint32_t index = 0;
         index < m_dynamicData.activePointLightCount;
         ++index)
    {
        const PointLight& light = m_dynamicData.pointLights[index];
        const bool effective = light.range > 0.0f
            && HasEnergy(light.color, light.intensity);
        m_featureUsage.hasPointLights |= effective;
        m_featureUsage.hasShadowedPointLights |=
            effective && light.castsShadow;
    }
    for (std::uint32_t index = 0;
         index < m_dynamicData.activeSpotLightCount;
         ++index)
    {
        const SpotLight& light = m_dynamicData.spotLights[index];
        const bool effective = light.range > 0.0f
            && HasEnergy(light.color, light.intensity);
        m_featureUsage.hasSpotLights |= effective;
        m_featureUsage.hasShadowedSpotLights |=
            effective && light.castsShadow;
    }

    std::unordered_set<std::uint64_t> viewIds;
    const std::size_t objectCount =
        m_sceneData->GetRenderObjects().size();
    for (const RenderView& view : m_views)
    {
        if (!view.id || view.width == 0 || view.height == 0)
        {
            throw std::invalid_argument(
                "RenderFramePacket views require an ID and non-zero extent.");
        }
        if (!viewIds.insert(view.id.value).second)
        {
            throw std::invalid_argument(
                "RenderFramePacket contains a duplicate view ID.");
        }
        if (view.selection != nullptr
            && std::ranges::any_of(
                view.selection->objectIndices,
                [objectCount](const std::uint32_t objectIndex)
                {
                    return objectIndex >= objectCount;
                }))
        {
            throw std::invalid_argument(
                "RenderFramePacket view selection references an unknown object.");
        }
    }
}

LogicalFrameId RenderFramePacket::GetLogicalFrameId() const
{
    return m_logicalFrameId;
}

double RenderFramePacket::GetSimulationTimeSeconds() const
{
    return m_simulationTimeSeconds;
}

double RenderFramePacket::GetSimulationDeltaSeconds() const
{
    return m_simulationDeltaSeconds;
}

const std::shared_ptr<const RenderSceneData>&
RenderFramePacket::GetSceneData() const
{
    return m_sceneData;
}

const RenderFrameDynamicData&
RenderFramePacket::GetDynamicData() const
{
    return m_dynamicData;
}

const RenderFrameFeatureUsage&
RenderFramePacket::GetFeatureUsage() const
{
    return m_featureUsage;
}

const std::vector<RenderView>& RenderFramePacket::GetViews() const
{
    return m_views;
}

const RenderView* RenderFramePacket::FindView(
    const RenderViewId id) const
{
    const auto found = std::ranges::find_if(
        m_views,
        [id](const RenderView& view)
        {
            return view.id == id;
        });
    return found != m_views.end() ? &*found : nullptr;
}
} // namespace Prism::Scene
