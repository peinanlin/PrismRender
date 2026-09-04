#include "Scene/SceneSession.h"

#include "Asset/AssetRegistry.h"
#include "Engine/CommandSystem.h"
#include "RHI/IGraphicsDevice.h"
#include "Scene/CameraController.h"
#include "Scene/RenderDynamicInputState.h"
#include "Scene/RenderFramePacket.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderSceneData.h"
#include "Scene/RenderSceneExtractor.h"
#include "Scene/RenderSceneObjectMapping.h"
#include "Scene/RenderSceneMailbox.h"
#include "Scene/SceneFraming.h"
#include "Scene/WorldRenderSceneBridge.h"

#include <DirectXMath.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Prism::Scene
{
namespace
{
bool ChangesImmutableSceneData(
    const Engine::SceneChangeCategory categories) noexcept
{
    return Engine::HasSceneChange(
               categories,
               Engine::SceneChangeCategory::EntityTopology)
        || Engine::HasSceneChange(
            categories,
            Engine::SceneChangeCategory::Hierarchy)
        || Engine::HasSceneChange(
            categories,
            Engine::SceneChangeCategory::Transform)
        || Engine::HasSceneChange(
            categories,
            Engine::SceneChangeCategory::Visibility)
        || Engine::HasSceneChange(
            categories,
            Engine::SceneChangeCategory::Material)
        || Engine::HasSceneChange(
            categories,
            Engine::SceneChangeCategory::FullRebuild);
}

void AccumulateExtractionChanges(
    Engine::SceneChangeSet& destination,
    const Engine::SceneChangeSet& source) noexcept
{
    if (!source.HasChanges())
    {
        return;
    }
    destination.revision = std::max(
        destination.revision,
        source.revision);
    destination.categories |= source.categories;
    destination.mutationCount += source.mutationCount;
}
} // namespace

SceneSessionActions::SceneSessionActions(SceneSession& session) noexcept
    : m_session(&session)
{
}

bool SceneSessionActions::SaveWorld(
    const std::filesystem::path& path,
    std::string& message) const
{
    if (!m_session->SynchronizeViewportCameraToWorld(message))
    {
        return false;
    }
    const nlohmann::json result = m_session->ExecuteCommand(
        "save",
        "world.save",
        {{"path", path.string()}});
    if (!result.value("success", false))
    {
        message = result.at("error").value(
            "message",
            std::string("World save failed."));
        return false;
    }
    message = "Engine World saved: " + path.string();
    return true;
}

bool SceneSessionActions::LoadWorld(
    const std::filesystem::path& path,
    std::string& message) const
{
    const nlohmann::json result = m_session->ExecuteCommand(
        "load",
        "world.load",
        {{"path", path.string()}});
    if (!result.value("success", false))
    {
        message = result.at("error").value(
            "message",
            std::string("World load failed."));
        return false;
    }
    m_session->SynchronizeWorldToRenderScene(true);
    message = "Engine World loaded: " + path.string();
    return true;
}

nlohmann::json SceneSessionActions::Execute(
    const std::string_view operation,
    const char* const command,
    nlohmann::json arguments) const
{
    return m_session->ExecuteCommand(
        operation,
        command,
        std::move(arguments));
}

void SceneSessionActions::MarkWorldChanged(
    const bool synchronizeCamera) const
{
    m_session->MarkWorldChanged(synchronizeCamera);
}

SceneSession::SceneSession(
    std::filesystem::path projectRoot,
    Asset::AssetRegistry& assetRegistry)
    : m_projectRoot(
          std::filesystem::absolute(std::move(projectRoot))
              .lexically_normal()),
      m_assetRegistry(&assetRegistry),
      m_scene(std::make_unique<RenderScene>()),
      m_mailbox(std::make_unique<RenderSceneMailbox>()),
      m_commandProcessor(
          std::make_unique<Engine::CommandProcessor>(m_projectRoot)),
      m_sceneCameraController(std::make_unique<CameraController>()),
      m_gameCameraController(std::make_unique<CameraController>()),
      m_dynamicInputState(
          std::make_unique<RenderDynamicInputState>()),
      m_sceneExtractor(std::make_unique<RenderSceneExtractor>(
          assetRegistry,
          ReadRenderScenePublicationMode()))
{
    m_dynamicInputState->ResetForScene(m_sceneGeneration);
}

SceneSession::~SceneSession() = default;

void SceneSession::ReleaseRuntimeRenderResources()
{
    // Packets and extractor snapshots share mesh/material resources with the
    // mutable scene. Release all three owners before the backend context is
    // destroyed so RHI resources can enter its retirement queue safely.
    m_mailbox = std::make_unique<RenderSceneMailbox>();
    m_sceneExtractor->Reset();
    m_scene->ClearRenderObjects();
}

RenderScene& SceneSession::GetRenderScene() noexcept
{
    return *m_scene;
}

const RenderScene& SceneSession::GetRenderScene() const noexcept
{
    return *m_scene;
}

RenderSceneMailbox& SceneSession::GetMailbox() noexcept
{
    return *m_mailbox;
}

Engine::CommandProcessor& SceneSession::GetCommandProcessor() noexcept
{
    return *m_commandProcessor;
}

const Engine::CommandProcessor&
SceneSession::GetCommandProcessor() const noexcept
{
    return *m_commandProcessor;
}

CameraController& SceneSession::GetSceneCameraController() noexcept
{
    return *m_sceneCameraController;
}

CameraController& SceneSession::GetGameCameraController() noexcept
{
    return *m_gameCameraController;
}

RenderDynamicInputState&
SceneSession::GetDynamicInputState() noexcept
{
    return *m_dynamicInputState;
}

const RenderDynamicInputState&
SceneSession::GetDynamicInputState() const noexcept
{
    return *m_dynamicInputState;
}

RenderSceneExtractor&
SceneSession::GetRenderSceneExtractor() noexcept
{
    return *m_sceneExtractor;
}

const RenderSceneExtractor&
SceneSession::GetRenderSceneExtractor() const noexcept
{
    return *m_sceneExtractor;
}

SceneSessionActions SceneSession::GetActions() noexcept
{
    return SceneSessionActions(*this);
}

const SceneSessionIdentity& SceneSession::GetIdentity() const noexcept
{
    return m_identity;
}

SceneGeneration SceneSession::GetSceneGeneration() const noexcept
{
    return m_sceneGeneration;
}

RenderSceneDataRevision
SceneSession::GetSceneDataRevision() const noexcept
{
    return m_sceneDataRevision;
}

std::uint64_t
SceneSession::GetRuntimeAssetBindingRevision() const noexcept
{
    return m_runtimeAssetBindingRevision;
}

std::size_t SceneSession::SynchronizeRuntimeAssetBindingChanges()
{
    std::vector<Asset::RuntimeAssetBindingChange> changes =
        m_assetRegistry->ConsumeRuntimeBindingChanges();
    for (const Asset::RuntimeAssetBindingChange& change : changes)
    {
        m_runtimeAssetBindingRevision = std::max(
            m_runtimeAssetBindingRevision,
            change.revision.value);
    }
    return changes.size();
}

void SceneSession::SetIdentity(SceneSessionIdentity identity)
{
    m_identity = std::move(identity);
}

void SceneSession::SetSourceLabel(std::string label)
{
    m_identity.sourceLabel = std::move(label);
}

void SceneSession::SetLoadMessage(std::string message)
{
    m_identity.loadMessage = std::move(message);
}

void SceneSession::SetActiveDemoScene(const DemoSceneId sceneId) noexcept
{
    m_identity.activeDemoScene = sceneId;
}

void SceneSession::SetUsingFallbackScene(
    const bool usingFallback) noexcept
{
    m_identity.usingFallbackScene = usingFallback;
}

void SceneSession::InitializeWorldFromRenderScene()
{
    Engine::World world;
    WorldRenderSceneBridge::ImportRenderScene(
        *m_scene,
        *m_assetRegistry,
        world);
    m_commandProcessor->InitializeWorld(std::move(world));
    if (m_sceneGeneration.value
        == std::numeric_limits<std::uint64_t>::max())
    {
        throw std::overflow_error(
            "Scene generation capacity exhausted.");
    }
    ++m_sceneGeneration.value;
    m_sceneDataRevision = RenderSceneDataRevision{1};
    m_dynamicInputState->ResetForScene(m_sceneGeneration);
    m_extractionChanges = {
        m_sceneGeneration.value,
        Engine::SceneChangeCategory::EntityTopology,
        1};
    (void)SynchronizeRuntimeAssetBindingChanges();
    m_worldSyncCameraRequested = false;
}

void SceneSession::MarkWorldChanged(
    const bool synchronizeCamera) noexcept
{
    m_commandProcessor->GetSceneChangeTracker().Record(
        Engine::SceneChangeCategory::FullRebuild);
    m_worldSyncCameraRequested =
        m_worldSyncCameraRequested || synchronizeCamera;
}

bool SceneSession::IsWorldDirty() const noexcept
{
    return m_commandProcessor->GetSceneChangeTracker()
        .HasPendingChanges();
}

void SceneSession::SynchronizeWorldToRenderScene(
    const bool synchronizeCamera)
{
    Engine::SceneChangeTracker& changeTracker =
        m_commandProcessor->GetSceneChangeTracker();
    const Engine::SceneChangeSet committedChanges =
        changeTracker.PeekPendingChanges();
    const WorldRenderSyncResult result =
        WorldRenderSceneBridge::SynchronizeToRenderScene(
            m_commandProcessor->GetWorld(),
            *m_assetRegistry,
            *m_scene,
            {synchronizeCamera, true, committedChanges});
    if (!result.unresolvedAssets.empty())
    {
        m_identity.loadMessage =
            "Engine World contains "
            + std::to_string(result.unresolvedAssets.size())
            + " unresolved render asset reference(s).";
    }
    if (ChangesImmutableSceneData(committedChanges.categories))
    {
        if (m_sceneDataRevision.value
            == std::numeric_limits<std::uint64_t>::max())
        {
            throw std::overflow_error(
                "Scene data revision capacity exhausted.");
        }
        ++m_sceneDataRevision.value;
    }
    (void)SynchronizeRuntimeAssetBindingChanges();
    AccumulateExtractionChanges(
        m_extractionChanges,
        committedChanges);
    (void)changeTracker.ConsumePendingChanges();
    m_worldSyncCameraRequested = false;
}

void SceneSession::SynchronizePendingWorldChanges()
{
    const Engine::SceneChangeSet& committedChanges =
        m_commandProcessor->GetSceneChangeTracker()
            .PeekPendingChanges();
    if (!committedChanges.HasChanges())
    {
        return;
    }
    SynchronizeWorldToRenderScene(
        m_worldSyncCameraRequested
        || Engine::HasSceneChange(
            committedChanges.categories,
            Engine::SceneChangeCategory::Camera));
}

bool SceneSession::SynchronizeViewportCameraToWorld(
    std::string& message)
{
    Engine::EntityRecord* cameraEntity = nullptr;
    for (auto& [id, entity] :
         m_commandProcessor->GetWorld().GetEntities())
    {
        (void)id;
        if (entity.camera.has_value())
        {
            cameraEntity =
                m_commandProcessor->GetWorld().FindEntity(entity.id);
            break;
        }
    }
    if (cameraEntity == nullptr)
    {
        message = "The Engine World has no Camera component.";
        return false;
    }

    nlohmann::json result = ExecuteCommand(
        "camera-begin",
        "transaction.begin",
        nlohmann::json::object());
    if (!result.value("success", false))
    {
        message = result.at("error").value(
            "message",
            std::string("Could not begin camera transaction."));
        return false;
    }

    const Camera& camera = m_scene->GetCamera();
    const DirectX::XMFLOAT3& position = camera.GetPosition();
    const Engine::Float3 scale = cameraEntity->transform.scale;
    result = ExecuteCommand(
        "camera-transform",
        "component.set",
        {{"entity", cameraEntity->id.ToString()},
         {"component", "Transform"},
         {"properties",
          {{"position", {position.x, position.y, position.z}},
           {"rotation", {camera.GetPitch(), camera.GetYaw(), 0.0f}},
           {"scale", {scale.x, scale.y, scale.z}}}}});
    if (result.value("success", false))
    {
        result = ExecuteCommand(
            "camera-properties",
            "component.set",
            {{"entity", cameraEntity->id.ToString()},
             {"component", "Camera"},
             {"properties",
              {{"fieldOfViewY", camera.GetFieldOfViewYRadians()},
               {"nearPlane", camera.GetNearPlane()},
               {"farPlane", camera.GetFarPlane()}}}});
    }
    if (!result.value("success", false))
    {
        (void)ExecuteCommand(
            "camera-rollback",
            "transaction.rollback",
            nlohmann::json::object());
        message = result.at("error").value(
            "message",
            std::string("Camera synchronization failed."));
        return false;
    }

    result = ExecuteCommand(
        "camera-commit",
        "transaction.commit",
        nlohmann::json::object());
    if (!result.value("success", false))
    {
        message = result.at("error").value(
            "message",
            std::string("Camera transaction commit failed."));
        return false;
    }
    return true;
}

nlohmann::json SceneSession::ExecuteCommand(
    const std::string_view operation,
    const char* const command,
    nlohmann::json arguments)
{
    return m_commandProcessor->Execute({
        {"requestId", NextRequestId(operation)},
        {"command", command},
        {"arguments", std::move(arguments)}});
}

SceneSessionDemoActivationResult SceneSession::ActivateDemoScene(
    const DemoSceneId sceneId,
    RHI::IGraphicsDevice& device,
    const float aspectRatio)
{
    SceneSessionDemoActivationResult result{};
    if (sceneId == m_identity.activeDemoScene)
    {
        return result;
    }

    result.build = DemoSceneCatalog::Populate(
        sceneId,
        *m_assetRegistry,
        device,
        *m_scene,
        aspectRatio);
    ConfigureViewportNavigation(sceneId);
    InitializeWorldFromRenderScene();

    m_identity.activeDemoScene = sceneId;
    const DemoSceneDescription& description =
        DemoSceneCatalog::GetDescription(sceneId);
    m_identity.sourceLabel =
        "Built-in " + std::string(description.displayName);
    m_identity.loadMessage = result.build.summary;
    m_identity.usingFallbackScene =
        sceneId == DemoSceneId::EditorPreview;
    result.changed = true;
    return result;
}

void SceneSession::ConfigureViewportNavigation(
    const DemoSceneId sceneId)
{
    float moveSpeed = 8.0f;
    switch (sceneId)
    {
    case DemoSceneId::TerrainVirtualTextureLab:
        moveSpeed = 240.0f;
        break;
    case DemoSceneId::OceanLab:
    case DemoSceneId::WaveWorksLab:
    case DemoSceneId::HpWaterOceanLab:
        moveSpeed = 90.0f;
        break;
    case DemoSceneId::PbfLab:
    case DemoSceneId::FluidRenderLab:
    case DemoSceneId::FluidCausticsLab:
    case DemoSceneId::FluidToonLab:
        moveSpeed = 7.0f;
        break;
    case DemoSceneId::LargeWorldLab:
        moveSpeed = 120.0f;
        break;
    case DemoSceneId::AtmosphereLab:
        moveSpeed = 35.0f;
        break;
    case DemoSceneId::GpuDrivenLab:
        moveSpeed = 24.0f;
        break;
    default:
        break;
    }
    m_sceneCameraController->SetMoveSpeed(moveSpeed);
    m_gameCameraController->SetMoveSpeed(moveSpeed);
    const std::optional<RenderSceneBounds> bounds =
        CalculateRenderObjectBounds(*m_scene);
    if (bounds.has_value())
    {
        m_sceneCameraController->SynchronizeOrbitPivot(
            m_scene->GetCamera(),
            bounds->center);
        m_gameCameraController->SynchronizeOrbitPivot(
            m_scene->GetGameCamera(),
            bounds->center);
    }
}

void SceneSession::BeginDynamicFrame(
    const std::uint64_t logicalFrameId,
    const double simulationTimeSeconds,
    const double simulationDeltaSeconds)
{
    m_dynamicInputState->BeginFrame(
        LogicalFrameId{logicalFrameId},
        simulationTimeSeconds,
        simulationDeltaSeconds);
}

void SceneSession::SynchronizeDynamicLights()
{
    (void)m_dynamicInputState->SynchronizeLights(*m_scene);
}

void SceneSession::SynchronizeDynamicView(
    const RenderViewId viewId,
    const Camera& camera,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint64_t historySettingsRevision)
{
    (void)m_dynamicInputState->SynchronizeView(
        viewId,
        camera,
        width,
        height,
        historySettingsRevision);
}

void SceneSession::NotifyDynamicViewHistory(
    const RenderViewId viewId,
    const RenderViewHistoryInvalidation reason)
{
    m_dynamicInputState->NotifyHistoryInvalidation(
        viewId,
        reason);
}

RenderSceneExtractionResult SceneSession::ExtractRenderSceneData()
{
    RenderSceneExtractionResult result = m_sceneExtractor->Extract({
        *m_scene,
        m_sceneGeneration,
        m_sceneDataRevision,
        m_runtimeAssetBindingRevision,
        m_extractionChanges});
    m_extractionChanges = {};
    return result;
}

std::shared_ptr<const RenderFramePacket>
SceneSession::BuildRenderFramePacket(
    std::shared_ptr<const RenderSceneData> sceneData) const
{
    if (sceneData == nullptr)
    {
        throw std::invalid_argument(
            "SceneSession requires scene data to build a frame packet.");
    }
    std::vector<RenderView> views;
    if (const RenderView* const game =
            m_dynamicInputState->FindView(GameRenderViewId);
        game != nullptr)
    {
        RenderView view = *game;
        view.selection = BuildRenderViewSelection(
            *sceneData,
            sceneData->GetDataRevision().value,
            [](const RenderObject& object,
                const RenderSceneObjectMetadata&)
            {
                return !object.editorOnly;
            });
        views.push_back(std::move(view));
    }
    if (const RenderView* const scene =
            m_dynamicInputState->FindView(SceneRenderViewId);
        scene != nullptr)
    {
        RenderView view = *scene;
        view.selection = BuildRenderViewSelection(
            *sceneData,
            sceneData->GetDataRevision().value,
            [](const RenderObject&,
                const RenderSceneObjectMetadata&)
            {
                return true;
            });
        views.push_back(std::move(view));
    }
    return std::make_shared<const RenderFramePacket>(
        m_dynamicInputState->GetLogicalFrameId(),
        m_dynamicInputState->GetSimulationTimeSeconds(),
        std::move(sceneData),
        m_dynamicInputState->GetDynamicData(),
        std::move(views),
        m_dynamicInputState->GetSimulationDeltaSeconds());
}

std::string SceneSession::NextRequestId(
    const std::string_view operation)
{
    return "scene-session-" + std::string(operation) + '-'
        + std::to_string(++m_requestCounter);
}
} // namespace Prism::Scene
