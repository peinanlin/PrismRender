#include "Asset/AssetRegistry.h"
#include "Asset/Mesh.h"
#include "Engine/CommandSystem.h"
#include "Scene/RenderDynamicInputState.h"
#include "Scene/RenderFramePacket.h"
#include "Scene/RenderScene.h"
#include "Scene/RenderSceneMailbox.h"
#include "Scene/SceneSession.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
void Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool Near(const float left, const float right)
{
    return std::abs(left - right) < 0.0001f;
}
} // namespace

int main()
{
    try
    {
        const std::filesystem::path projectRoot =
            std::filesystem::path(PRISM_RENDER_PROJECT_DIR);
        const std::filesystem::path snapshotPath =
            projectRoot / "artifacts" / "scene-session-tests"
            / "round-trip.prismworld.json";
        std::filesystem::create_directories(snapshotPath.parent_path());

        Prism::Asset::AssetRegistry assets;
        const Prism::Asset::MeshHandle mesh = assets.RegisterMeshAsset(
            "assets/meshes/session-test.mesh", nullptr);
        const Prism::Asset::MaterialHandle material =
            assets.RegisterMaterialAsset(
                "assets/materials/session-test.material", nullptr);

        Prism::Scene::SceneSession session(projectRoot, assets);
        Prism::Scene::RenderScene& scene = session.GetRenderScene();
        scene.GetCamera().SetPerspective(
            0.9f, 16.0f / 9.0f, 0.2f, 900.0f);
        scene.GetCamera().SetPosition({2.0f, 3.0f, -8.0f});
        scene.GetCamera().SetRotation(0.1f, 0.25f);

        Prism::Scene::RenderObject object{};
        object.name = "SessionCube";
        object.meshHandle = mesh;
        object.materialHandle = material;
        object.transform.SetPosition({1.0f, 2.0f, 3.0f});
        scene.AddRenderObject(std::move(object));
        session.InitializeWorldFromRenderScene();
        const Prism::Scene::SceneGeneration initialGeneration =
            session.GetSceneGeneration();
        const Prism::Scene::RenderSceneDataRevision initialDataRevision =
            session.GetSceneDataRevision();

        session.BeginDynamicFrame(1, 0.0);
        session.SynchronizeDynamicLights();
        session.SynchronizeDynamicView(
            Prism::Scene::GameRenderViewId,
            scene.GetGameCamera(),
            1280,
            800,
            1);
        session.SynchronizeDynamicView(
            Prism::Scene::SceneRenderViewId,
            scene.GetCamera(),
            960,
            600,
            1);
        scene.GetGameCamera().SetPosition(
            {15.0f, -0.1f, -4.0f});
        session.BeginDynamicFrame(2, 1.0 / 60.0);
        session.SynchronizeDynamicLights();
        session.SynchronizeDynamicView(
            Prism::Scene::GameRenderViewId,
            scene.GetGameCamera(),
            1280,
            800,
            2);
        session.SynchronizeDynamicView(
            Prism::Scene::SceneRenderViewId,
            scene.GetCamera(),
            960,
            600,
            1);
        const Prism::Scene::RenderDynamicInputState& dynamicInputs =
            session.GetDynamicInputState();
        Expect(
            session.GetSceneGeneration() == initialGeneration
                && session.GetSceneDataRevision()
                    == initialDataRevision
                && dynamicInputs.GetLogicalFrameId()
                    == Prism::Scene::LogicalFrameId{2}
                && dynamicInputs.GetSimulationTimeSeconds()
                    == 1.0 / 60.0
                && dynamicInputs.FindView(
                    Prism::Scene::GameRenderViewId)
                        ->camera.GetPosition().y < 0.0f
                && dynamicInputs.FindView(
                    Prism::Scene::SceneRenderViewId)
                        ->camera.GetPosition().y > 0.0f,
            "Dynamic camera/settings/time input changed object identity, crossed views, or froze simulation state.");

        session.GetCommandProcessor().GetSceneChangeTracker().Record(
            Prism::Engine::SceneChangeCategory::Camera);
        session.SynchronizePendingWorldChanges();
        Expect(
            session.GetSceneDataRevision()
                == initialDataRevision,
            "A committed camera-only update advanced immutable object data.");
        session.GetCommandProcessor().GetSceneChangeTracker().Record(
            Prism::Engine::SceneChangeCategory::Lighting);
        session.SynchronizePendingWorldChanges();
        Expect(
            session.GetSceneDataRevision()
                == initialDataRevision,
            "A committed lighting-only update advanced immutable object data.");

        session.GetRenderSceneExtractor().SetMode(
            Prism::Scene::RenderScenePublicationMode::Versioned);
        const Prism::Scene::RenderSceneExtractionResult firstExtraction =
            session.ExtractRenderSceneData();
        std::shared_ptr<const Prism::Scene::RenderFramePacket>
            firstPacket = session.BuildRenderFramePacket(
                firstExtraction.sceneData);
        (void)session.GetMailbox().PublishFramePacket(
            firstPacket);
        session.BeginDynamicFrame(3, 2.0 / 60.0);
        session.SynchronizeDynamicLights();
        session.SynchronizeDynamicView(
            Prism::Scene::GameRenderViewId,
            scene.GetGameCamera(),
            1280,
            800,
            2);
        session.SynchronizeDynamicView(
            Prism::Scene::SceneRenderViewId,
            scene.GetCamera(),
            960,
            600,
            1);
        const Prism::Scene::RenderSceneExtractionResult secondExtraction =
            session.ExtractRenderSceneData();
        const auto secondPacket = session.BuildRenderFramePacket(
            secondExtraction.sceneData);
        (void)session.GetMailbox().PublishFramePacket(
            secondPacket);
        Expect(
            firstExtraction.rebuilt
                && secondExtraction.reused
                && firstPacket->GetSceneData().get()
                    == secondPacket->GetSceneData().get()
                && firstPacket->GetLogicalFrameId()
                    == Prism::Scene::LogicalFrameId{2}
                && secondPacket->GetLogicalFrameId()
                    == Prism::Scene::LogicalFrameId{3}
                && secondPacket->GetSimulationTimeSeconds()
                    == 2.0 / 60.0
                && secondPacket->FindView(
                    Prism::Scene::GameRenderViewId) != nullptr
                && secondPacket->FindView(
                    Prism::Scene::SceneRenderViewId) != nullptr
                && session.GetMailbox().AcquireLatestPacket().get()
                    == secondPacket.get(),
            "SceneSession did not publish continuous self-consistent packets with reused immutable data.");

        assets.SetRuntimeMesh(
            mesh,
            std::make_shared<Prism::Asset::Mesh>());
        Expect(
            session.SynchronizeRuntimeAssetBindingChanges() == 1
                && session.GetRuntimeAssetBindingRevision() != 0
                && session.GetSceneGeneration() == initialGeneration
                && session.GetSceneDataRevision()
                    == initialDataRevision,
            "Runtime asset publication changed scene/data identity instead of only binding identity.");

        Expect(
            session.GetCommandProcessor().GetWorld().GetEntityCount() >= 3,
            "SceneSession did not import RenderScene state into World.");
        Expect(
            scene.GetRenderObjects().front().entityId.IsValid(),
            "SceneSession bridge did not preserve render-object identity.");

        const std::string beforeHash =
            session.GetCommandProcessor().ComputeStateHash();
        Prism::Scene::SceneSessionActions actions = session.GetActions();
        Expect(
            actions.Execute(
                       "begin",
                       "transaction.begin",
                       nlohmann::json::object())
                .value("success", false),
            "SceneSession transaction begin failed.");
        Expect(
            actions.Execute(
                       "create",
                       "entity.create",
                       {{"name", "SessionTransactionEntity"}})
                .value("success", false),
            "SceneSession command execution failed.");
        Expect(
            !session.IsWorldDirty(),
            "An uncommitted transaction published an intermediate scene change.");
        Expect(
            actions.Execute(
                       "commit",
                       "transaction.commit",
                       nlohmann::json::object())
                .value("success", false),
            "SceneSession transaction commit failed.");
        Expect(
            session.IsWorldDirty()
                && Prism::Engine::HasSceneChange(
                    session.GetCommandProcessor()
                        .GetSceneChangeTracker()
                        .PeekPendingChanges()
                        .categories,
                    Prism::Engine::SceneChangeCategory::EntityTopology),
            "Transaction commit did not publish its categorized scene change.");
        const std::string committedHash =
            session.GetCommandProcessor().ComputeStateHash();
        Expect(
            committedHash != beforeHash,
            "SceneSession transaction did not mutate World state.");
        session.SynchronizePendingWorldChanges();
        Expect(
            !session.IsWorldDirty()
                && session.GetSceneGeneration() == initialGeneration
                && session.GetSceneDataRevision().value
                    == initialDataRevision.value + 1,
            "Bridge synchronization did not consume the committed change set.");
        Expect(
            actions.Execute(
                       "undo",
                       "transaction.undo",
                       nlohmann::json::object())
                .value("success", false)
                && session.GetCommandProcessor().ComputeStateHash()
                    == beforeHash,
            "SceneSession undo did not restore the prior state.");
        Expect(
            session.IsWorldDirty(),
            "Undo did not publish a scene change.");
        session.SynchronizePendingWorldChanges();
        Expect(
            actions.Execute(
                       "redo",
                       "transaction.redo",
                       nlohmann::json::object())
                .value("success", false)
                && session.GetCommandProcessor().ComputeStateHash()
                    == committedHash,
            "SceneSession redo did not restore the committed state.");
        Expect(
            session.IsWorldDirty(),
            "Redo did not publish a scene change.");
        session.SynchronizePendingWorldChanges();

        const nlohmann::json journal =
            session.GetCommandProcessor().ExportJournal();
        Expect(
            journal.is_object() && journal.contains("entries")
                && !journal.at("entries").empty(),
            "SceneSession did not retain the command journal.");
        Prism::Engine::CommandProcessor replayed(projectRoot);
        std::string replayError;
        Expect(
            replayed.ReplayJournal(journal, &replayError)
                && replayed.ComputeStateHash() == committedHash
                && replayed.GetSceneChangeTracker()
                    .HasPendingChanges(),
            "SceneSession journal did not replay deterministically.");

        Prism::Engine::CommandProcessor categorized(projectRoot);
        std::uint32_t categorizedRequest = 0;
        const auto executeCategorized =
            [&](const char* const command, nlohmann::json arguments)
        {
            return categorized.Execute({
                {"requestId",
                 "scene-change-"
                     + std::to_string(++categorizedRequest)},
                {"command", command},
                {"arguments", std::move(arguments)}});
        };
        Expect(
            executeCategorized(
                "transaction.begin",
                nlohmann::json::object())
                .value("success", false),
            "Categorized transaction begin failed.");
        const nlohmann::json parentResult = executeCategorized(
            "entity.create", {{"name", "Parent"}});
        const nlohmann::json childResult = executeCategorized(
            "entity.create", {{"name", "Child"}});
        const std::string parentId =
            parentResult.at("data").at("entity").get<std::string>();
        const std::string childId =
            childResult.at("data").at("entity").get<std::string>();
        Expect(
            executeCategorized(
                "component.add",
                {{"entity", childId},
                 {"component", "Hierarchy"},
                 {"properties", {{"parent", parentId}}}})
                .value("success", false)
                && executeCategorized(
                       "component.add",
                       {{"entity", childId},
                        {"component", "MeshRenderer"}})
                       .value("success", false)
                && executeCategorized(
                       "component.add",
                       {{"entity", childId},
                        {"component", "MaterialOverride"}})
                       .value("success", false)
                && executeCategorized(
                       "component.set",
                       {{"entity", childId},
                        {"component", "Transform"},
                        {"properties", {{"position", {1.0, 2.0, 3.0}}}}})
                       .value("success", false)
                && executeCategorized(
                       "component.set",
                       {{"entity", childId},
                        {"component", "MeshRenderer"},
                        {"properties",
                         {{"visible", false},
                          {"materialAsset", "material://changed"}}}})
                       .value("success", false)
                && executeCategorized(
                       "component.set",
                       {{"entity", childId},
                        {"component", "MaterialOverride"},
                        {"properties", {{"roughness", 0.25f}}}})
                       .value("success", false),
            "Categorized World mutation commands failed.");
        Expect(
            !categorized.GetSceneChangeTracker().HasPendingChanges(),
            "Categorized transaction leaked changes before commit.");
        Expect(
            executeCategorized(
                "transaction.commit",
                nlohmann::json::object())
                .value("success", false),
            "Categorized transaction commit failed.");
        const Prism::Engine::SceneChangeSet categorizedChanges =
            categorized.GetSceneChangeTracker()
                .ConsumePendingChanges();
        Expect(
            categorizedChanges.revision == 1
                && Prism::Engine::HasSceneChange(
                    categorizedChanges.categories,
                    Prism::Engine::SceneChangeCategory::EntityTopology)
                && Prism::Engine::HasSceneChange(
                    categorizedChanges.categories,
                    Prism::Engine::SceneChangeCategory::Hierarchy)
                && Prism::Engine::HasSceneChange(
                    categorizedChanges.categories,
                    Prism::Engine::SceneChangeCategory::Transform)
                && Prism::Engine::HasSceneChange(
                    categorizedChanges.categories,
                    Prism::Engine::SceneChangeCategory::Visibility)
                && Prism::Engine::HasSceneChange(
                    categorizedChanges.categories,
                    Prism::Engine::SceneChangeCategory::Material),
            "World commands did not publish the expected change categories as one revision.");

        const std::string beforeRollback = categorized.ComputeStateHash();
        Expect(
            executeCategorized(
                "transaction.begin",
                nlohmann::json::object())
                    .value("success", false)
                && executeCategorized(
                       "component.set",
                       {{"entity", childId},
                        {"component", "Transform"},
                        {"properties", {{"position", {9.0, 9.0, 9.0}}}}})
                       .value("success", false)
                && executeCategorized(
                       "transaction.rollback",
                       nlohmann::json::object())
                       .value("success", false)
                && categorized.ComputeStateHash() == beforeRollback
                && !categorized.GetSceneChangeTracker()
                    .HasPendingChanges(),
            "Rollback published or retained an intermediate scene change.");

        // A failing command restores its own partial writes while the outer
        // transaction remains unpublished. Rolling that transaction back
        // must then restore the state from before its earlier valid command.
        const std::size_t entityCountBeforeFailedTransaction =
            categorized.GetWorld().GetEntityCount();
        Expect(
            executeCategorized(
                "transaction.begin",
                nlohmann::json::object())
                .value("success", false),
            "Failed-command transaction begin failed.");
        const nlohmann::json temporaryEntityResult =
            executeCategorized(
                "entity.create",
                {{"name", "FailedTransactionEntity"}});
        const auto temporaryEntityId =
            Prism::Engine::EntityId::Parse(
                temporaryEntityResult.at("data")
                    .at("entity")
                    .get<std::string>());
        Expect(
            temporaryEntityResult.value("success", false)
                && temporaryEntityId.has_value(),
            "Failed-command transaction setup failed.");
        const nlohmann::json failedMutation = executeCategorized(
            "component.add",
            {{"entity", temporaryEntityId->ToString()},
             {"component", "MeshRenderer"},
             {"properties",
              {{"visible", false},
               {"zzInvalidProperty", 1}}}});
        const Prism::Engine::EntityRecord* const temporaryEntity =
            categorized.GetWorld().FindEntity(*temporaryEntityId);
        Expect(
            !failedMutation.value("success", true)
                && temporaryEntity != nullptr
                && !temporaryEntity->meshRenderer.has_value()
                && !categorized.GetSceneChangeTracker()
                    .HasPendingChanges(),
            "A failed command leaked partial component or publication state.");
        Expect(
            executeCategorized(
                "transaction.rollback",
                nlohmann::json::object())
                    .value("success", false)
                && categorized.GetWorld().GetEntityCount()
                    == entityCountBeforeFailedTransaction
                && categorized.GetWorld().FindEntity(
                    *temporaryEntityId) == nullptr
                && !categorized.GetSceneChangeTracker()
                    .HasPendingChanges(),
            "A failed transaction rollback leaked World or publication state.");

        std::string message;
        Expect(
            actions.SaveWorld(snapshotPath, message),
            "SceneSession world save failed.");
        const DirectX::XMFLOAT3 savedCameraPosition =
            scene.GetCamera().GetPosition();

        Expect(
            actions.Execute(
                       "mutate-camera",
                       "component.set",
                       {{"entity",
                         session.GetCommandProcessor()
                             .GetWorld()
                             .GetEntities()
                             .begin()
                             ->first.ToString()},
                        {"component", "Transform"},
                        {"properties",
                         {{"position", {40.0f, 50.0f, 60.0f}}}}})
                .contains("success"),
            "SceneSession mutation command did not produce a response.");
        Expect(
            actions.LoadWorld(snapshotPath, message),
            "SceneSession world load failed.");
        Expect(
            Near(scene.GetCamera().GetPosition().x, savedCameraPosition.x)
                && Near(
                    scene.GetCamera().GetPosition().y,
                    savedCameraPosition.y)
                && Near(
                    scene.GetCamera().GetPosition().z,
                    savedCameraPosition.z),
            "SceneSession load did not synchronize World camera state back to RenderScene.");

        session.MarkWorldChanged(true);
        Expect(
            session.IsWorldDirty(),
            "SceneSession did not retain its World dirty state.");
        session.SynchronizeWorldToRenderScene(true);
        Expect(
            !session.IsWorldDirty(),
            "SceneSession did not clear dirty state after synchronization.");

        const Prism::Scene::SceneGeneration generationBeforeRebuild =
            session.GetSceneGeneration();
        session.InitializeWorldFromRenderScene();
        Expect(
            session.GetSceneGeneration().value
                    == generationBeforeRebuild.value + 1
                && session.GetSceneDataRevision().value == 1,
            "Programmatic scene topology rebuild did not advance generation and reset data revision.");

        Prism::Scene::SceneSessionIdentity identity{};
        identity.activeDemoScene = Prism::Scene::DemoSceneId::Showcase;
        identity.sourceLabel = "Session test scene";
        identity.loadMessage = "Session test initialized";
        identity.usingFallbackScene = false;
        session.SetIdentity(identity);
        Expect(
            session.GetIdentity().activeDemoScene
                    == Prism::Scene::DemoSceneId::Showcase
                && !session.GetIdentity().usingFallbackScene,
            "SceneSession did not own scene identity state.");

        session.BeginDynamicFrame(4, 3.0 / 60.0);
        session.SynchronizeDynamicView(
            Prism::Scene::GameRenderViewId,
            scene.GetGameCamera(),
            1280,
            800,
            3);
        const auto shutdownExtraction =
            session.ExtractRenderSceneData();
        const auto shutdownPacket = session.BuildRenderFramePacket(
            shutdownExtraction.sceneData);
        (void)session.GetMailbox().PublishFramePacket(shutdownPacket);
        Expect(session.GetMailbox().HasPublishedFrame(),
            "SceneSession shutdown fixture did not retain a packet.");
        session.ReleaseRuntimeRenderResources();
        Expect(session.GetRenderScene().GetRenderObjects().empty()
                && !session.GetMailbox().HasPublishedFrame(),
            "SceneSession did not release scene and packet render bindings before backend shutdown.");

        std::filesystem::remove(snapshotPath);
        std::cout << "SceneSession tests passed.\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SceneSession tests failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
