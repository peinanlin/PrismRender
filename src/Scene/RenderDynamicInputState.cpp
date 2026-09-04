#include "Scene/RenderDynamicInputState.h"

#include "Scene/Camera.h"
#include "Scene/RenderScene.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Prism::Scene
{
namespace
{
template <typename T>
void AdvanceRevision(T& revision, const char* const message)
{
    if (revision == std::numeric_limits<T>::max())
    {
        throw std::overflow_error(message);
    }
    ++revision;
}

bool EqualFloat3(
    const DirectX::XMFLOAT3& left,
    const DirectX::XMFLOAT3& right) noexcept
{
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool EqualDirectionalLight(
    const DirectionalLight& left,
    const DirectionalLight& right) noexcept
{
    return EqualFloat3(left.direction, right.direction)
        && left.intensity == right.intensity
        && EqualFloat3(left.color, right.color);
}

bool EqualPointLight(
    const PointLight& left,
    const PointLight& right) noexcept
{
    return EqualFloat3(left.position, right.position)
        && left.range == right.range
        && EqualFloat3(left.color, right.color)
        && left.intensity == right.intensity
        && left.castsShadow == right.castsShadow;
}

bool EqualSpotLight(
    const SpotLight& left,
    const SpotLight& right) noexcept
{
    return EqualFloat3(left.position, right.position)
        && left.range == right.range
        && EqualFloat3(left.direction, right.direction)
        && left.outerAngleRadians == right.outerAngleRadians
        && EqualFloat3(left.color, right.color)
        && left.intensity == right.intensity
        && left.innerAngleRadians == right.innerAngleRadians
        && left.castsShadow == right.castsShadow;
}

bool EqualCamera(const Camera& left, const Camera& right) noexcept
{
    const Core::Double3& leftWorld = left.GetWorldPosition();
    const Core::Double3& rightWorld = right.GetWorldPosition();
    if (leftWorld.x != rightWorld.x
        || leftWorld.y != rightWorld.y
        || leftWorld.z != rightWorld.z
        || left.GetPitch() != right.GetPitch()
        || left.GetYaw() != right.GetYaw()
        || left.GetFieldOfViewYRadians()
            != right.GetFieldOfViewYRadians()
        || left.GetAspectRatio() != right.GetAspectRatio()
        || left.GetNearPlane() != right.GetNearPlane()
        || left.GetFarPlane() != right.GetFarPlane())
    {
        return false;
    }

    DirectX::XMFLOAT4X4 leftProjection{};
    DirectX::XMFLOAT4X4 rightProjection{};
    DirectX::XMStoreFloat4x4(
        &leftProjection,
        left.GetProjectionMatrix());
    DirectX::XMStoreFloat4x4(
        &rightProjection,
        right.GetProjectionMatrix());
    const float* const leftValues = &leftProjection._11;
    const float* const rightValues = &rightProjection._11;
    return std::equal(
        leftValues,
        leftValues + 16,
        rightValues);
}
} // namespace

void RenderDynamicInputState::ResetForScene(
    const SceneGeneration sceneGeneration)
{
    if (!sceneGeneration)
    {
        throw std::invalid_argument(
            "Dynamic render inputs require a non-zero scene generation.");
    }
    m_sceneGeneration = sceneGeneration;
    m_logicalFrameId = {};
    m_simulationTimeSeconds = 0.0;
    m_simulationDeltaSeconds = 1.0 / 60.0;
    m_dynamicData = {};
    m_views = {};
    for (ViewState& state : m_views)
    {
        state.pendingInvalidation =
            RenderViewHistoryInvalidation::Scene;
    }
}

void RenderDynamicInputState::BeginFrame(
    const LogicalFrameId logicalFrameId,
    const double simulationTimeSeconds,
    const double simulationDeltaSeconds)
{
    if (!logicalFrameId)
    {
        throw std::invalid_argument(
            "Dynamic render inputs require a non-zero logical frame ID.");
    }
    if (!std::isfinite(simulationTimeSeconds)
        || simulationTimeSeconds < 0.0)
    {
        throw std::invalid_argument(
            "Dynamic render input time must be finite and non-negative.");
    }
    if (!std::isfinite(simulationDeltaSeconds)
        || simulationDeltaSeconds < 0.0)
    {
        throw std::invalid_argument(
            "Dynamic render input step must be finite and non-negative.");
    }
    if (m_logicalFrameId
        && logicalFrameId.value <= m_logicalFrameId.value)
    {
        throw std::invalid_argument(
            "Dynamic render input frames must advance monotonically.");
    }

    for (ViewState& state : m_views)
    {
        if (state.initialized)
        {
            state.view.previousCamera = state.view.camera;
            state.view.previousCameraValid = true;
            state.view.historyInvalidation =
                RenderViewHistoryInvalidation::None;
        }
    }
    m_logicalFrameId = logicalFrameId;
    m_simulationTimeSeconds = simulationTimeSeconds;
    m_simulationDeltaSeconds = simulationDeltaSeconds;
}

bool RenderDynamicInputState::SynchronizeLights(
    const RenderScene& scene)
{
    if (!m_logicalFrameId)
    {
        throw std::logic_error(
            "BeginFrame must precede dynamic light synchronization.");
    }

    RenderFrameDynamicData next = m_dynamicData;
    next.directionalLight = scene.GetDirectionalLight();
    next.auxiliaryDirectionalLights =
        scene.GetAuxiliaryDirectionalLights();
    next.pointLights = scene.GetPointLights();
    next.activePointLightCount =
        scene.GetActivePointLightCount();
    next.spotLights = scene.GetSpotLights();
    next.activeSpotLightCount =
        scene.GetActiveSpotLightCount();

    bool changed = m_dynamicData.revision == 0
        || !EqualDirectionalLight(
            m_dynamicData.directionalLight,
            next.directionalLight)
        || m_dynamicData.activePointLightCount
            != next.activePointLightCount
        || m_dynamicData.activeSpotLightCount
            != next.activeSpotLightCount;
    for (std::size_t index = 0;
         !changed && index < next.auxiliaryDirectionalLights.size();
         ++index)
    {
        changed = !EqualDirectionalLight(
            m_dynamicData.auxiliaryDirectionalLights[index],
            next.auxiliaryDirectionalLights[index]);
    }
    for (std::size_t index = 0;
         !changed && index < next.activePointLightCount;
         ++index)
    {
        changed = !EqualPointLight(
            m_dynamicData.pointLights[index],
            next.pointLights[index]);
    }
    for (std::size_t index = 0;
         !changed && index < next.activeSpotLightCount;
         ++index)
    {
        changed = !EqualSpotLight(
            m_dynamicData.spotLights[index],
            next.spotLights[index]);
    }
    if (!changed)
    {
        return false;
    }

    next.revision = m_dynamicData.revision;
    AdvanceRevision(
        next.revision,
        "Dynamic light revision capacity exhausted.");
    m_dynamicData = std::move(next);
    return true;
}

bool RenderDynamicInputState::SynchronizeView(
    const RenderViewId viewId,
    const Camera& camera,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint64_t historySettingsRevision)
{
    if (!m_logicalFrameId)
    {
        throw std::logic_error(
            "BeginFrame must precede dynamic view synchronization.");
    }
    if (width == 0 || height == 0 || historySettingsRevision == 0)
    {
        throw std::invalid_argument(
            "Dynamic views require a non-zero extent and settings revision.");
    }

    ViewState& state = GetViewState(viewId);
    if (!state.initialized)
    {
        state.view.id = viewId;
        state.view.camera = camera;
        state.view.width = width;
        state.view.height = height;
        state.view.cameraRevision = 1;
        state.view.historyRevision = 1;
        state.view.cameraCutRevision =
            HasRenderViewHistoryInvalidation(
                state.pendingInvalidation,
                RenderViewHistoryInvalidation::CameraCut)
            ? 1u : 0u;
        state.view.historyInvalidation =
            RenderViewHistoryInvalidation::Initialization
            | state.pendingInvalidation;
        state.historySettingsRevision = historySettingsRevision;
        state.pendingInvalidation =
            RenderViewHistoryInvalidation::None;
        state.initialized = true;
        return true;
    }

    bool changed = false;
    RenderViewHistoryInvalidation invalidation =
        RenderViewHistoryInvalidation::None;
    if (!EqualCamera(state.view.camera, camera))
    {
        state.view.camera = camera;
        AdvanceRevision(
            state.view.cameraRevision,
            "Dynamic camera revision capacity exhausted.");
        changed = true;
    }
    if (state.view.width != width || state.view.height != height)
    {
        state.view.width = width;
        state.view.height = height;
        invalidation = invalidation
            | RenderViewHistoryInvalidation::Extent;
        changed = true;
    }
    if (state.historySettingsRevision != historySettingsRevision)
    {
        state.historySettingsRevision = historySettingsRevision;
        invalidation = invalidation
            | RenderViewHistoryInvalidation::Settings;
        changed = true;
    }
    if (invalidation != RenderViewHistoryInvalidation::None)
    {
        AdvanceRevision(
            state.view.historyRevision,
            "Render view history revision capacity exhausted.");
        state.view.historyInvalidation = invalidation;
    }
    return changed;
}

void RenderDynamicInputState::NotifyHistoryInvalidation(
    const RenderViewId viewId,
    const RenderViewHistoryInvalidation reason)
{
    if (reason == RenderViewHistoryInvalidation::None)
    {
        return;
    }
    ViewState& state = GetViewState(viewId);
    if (!state.initialized)
    {
        state.pendingInvalidation =
            state.pendingInvalidation | reason;
        return;
    }
    AdvanceRevision(
        state.view.historyRevision,
        "Render view history revision capacity exhausted.");
    state.view.historyInvalidation = reason;
    if (HasRenderViewHistoryInvalidation(
            reason,
            RenderViewHistoryInvalidation::CameraCut))
    {
        AdvanceRevision(
            state.view.cameraCutRevision,
            "Render view camera-cut revision capacity exhausted.");
    }
}

SceneGeneration RenderDynamicInputState::GetSceneGeneration() const noexcept
{
    return m_sceneGeneration;
}

LogicalFrameId RenderDynamicInputState::GetLogicalFrameId() const noexcept
{
    return m_logicalFrameId;
}

double RenderDynamicInputState::GetSimulationTimeSeconds() const noexcept
{
    return m_simulationTimeSeconds;
}

double RenderDynamicInputState::GetSimulationDeltaSeconds() const noexcept
{
    return m_simulationDeltaSeconds;
}

const RenderFrameDynamicData&
RenderDynamicInputState::GetDynamicData() const noexcept
{
    return m_dynamicData;
}

const RenderView* RenderDynamicInputState::FindView(
    const RenderViewId viewId) const
{
    const ViewState& state = GetViewState(viewId);
    return state.initialized ? &state.view : nullptr;
}

std::size_t RenderDynamicInputState::ViewIndex(
    const RenderViewId viewId)
{
    if (viewId == GameRenderViewId)
    {
        return 0;
    }
    if (viewId == SceneRenderViewId)
    {
        return 1;
    }
    throw std::invalid_argument(
        "Dynamic input state only accepts stable Game and Scene view IDs.");
}

RenderDynamicInputState::ViewState&
RenderDynamicInputState::GetViewState(const RenderViewId viewId)
{
    return m_views[ViewIndex(viewId)];
}

const RenderDynamicInputState::ViewState&
RenderDynamicInputState::GetViewState(const RenderViewId viewId) const
{
    return m_views[ViewIndex(viewId)];
}
} // namespace Prism::Scene
