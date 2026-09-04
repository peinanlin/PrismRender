#pragma once

#include "Scene/Light.h"
#include "Scene/RenderSceneData.h"
#include "Scene/RenderView.h"

#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <memory>
#include <vector>

namespace Prism::Scene
{
struct LogicalFrameId
{
    std::uint64_t value = 0;
    [[nodiscard]] explicit operator bool() const
    {
        return value != 0;
    }
    auto operator<=>(const LogicalFrameId&) const = default;
};

// Values that may change every logical frame but are shared by all views.
// Feature simulation remains owned by each Feature and is not folded into the
// scene-data dirty state.
struct RenderFrameDynamicData
{
    static constexpr std::size_t MaxAuxiliaryDirectionalLights = 2;
    static constexpr std::size_t MaxPointLights = 128;
    static constexpr std::size_t MaxSpotLights = 16;

    std::uint64_t revision = 0;
    DirectionalLight directionalLight;
    std::array<DirectionalLight,
        MaxAuxiliaryDirectionalLights> auxiliaryDirectionalLights{};
    std::array<PointLight, MaxPointLights> pointLights{};
    std::uint32_t activePointLightCount = 0;
    std::array<SpotLight, MaxSpotLights> spotLights{};
    std::uint32_t activeSpotLightCount = 0;
};

struct RenderFrameFeatureUsage
{
    RenderSceneFeatureUsage scene;
    bool hasDirectionalLight = false;
    bool hasPointLights = false;
    bool hasSpotLights = false;
    bool hasShadowedPointLights = false;
    bool hasShadowedSpotLights = false;
};

class RenderFramePacket final
{
public:
    RenderFramePacket(
        LogicalFrameId logicalFrameId,
        double simulationTimeSeconds,
        std::shared_ptr<const RenderSceneData> sceneData,
        RenderFrameDynamicData dynamicData,
        std::vector<RenderView> views,
        double simulationDeltaSeconds = 1.0 / 60.0);

    [[nodiscard]] LogicalFrameId GetLogicalFrameId() const;
    [[nodiscard]] double GetSimulationTimeSeconds() const;
    [[nodiscard]] double GetSimulationDeltaSeconds() const;
    [[nodiscard]] const std::shared_ptr<const RenderSceneData>&
        GetSceneData() const;
    [[nodiscard]] const RenderFrameDynamicData&
        GetDynamicData() const;
    [[nodiscard]] const RenderFrameFeatureUsage&
        GetFeatureUsage() const;
    [[nodiscard]] const std::vector<RenderView>& GetViews() const;
    [[nodiscard]] const RenderView* FindView(RenderViewId id) const;

private:
    LogicalFrameId m_logicalFrameId;
    double m_simulationTimeSeconds = 0.0;
    double m_simulationDeltaSeconds = 1.0 / 60.0;
    std::shared_ptr<const RenderSceneData> m_sceneData;
    RenderFrameDynamicData m_dynamicData;
    RenderFrameFeatureUsage m_featureUsage;
    std::vector<RenderView> m_views;
};
} // namespace Prism::Scene
