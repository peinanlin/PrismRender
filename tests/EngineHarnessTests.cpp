#include "Automation/HarnessRunner.h"
#include "Automation/HarnessTools.h"
#include "Automation/McpServer.h"
#include "Automation/PerformanceBaseline.h"
#include "Asset/AssetCache.h"
#include "Asset/CookedAssetIO.h"
#include "Asset/AssetDatabase.h"
#include "Asset/AssetImportService.h"
#include "Asset/AssetRegistry.h"
#include "Asset/AssetStreamingManager.h"
#include "Core/CpuTrace.h"
#include "Engine/CommandSystem.h"
#include "Engine/Components.h"
#include "Engine/EntityId.h"
#include "Engine/WorldSerializer.h"
#include "Renderer/GpuTimingReport.h"
#include "Renderer/Features/Ocean/WaterBenchmarkReport.h"
#include "Renderer/Features/Ocean/OceanSettings.h"
#include "Renderer/Features/Ocean/OceanStatistics.h"
#include "Renderer/RenderGraph.h"
#include "Renderer/PerformanceIdentity.h"
#include "RHI/GraphicsApi.h"
#include "Scene/RenderScene.h"
#include "Scene/WorldRenderSnapshot.h"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using json = nlohmann::json;

void Expect(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

json Request(
    std::string requestId,
    std::string command,
    json arguments = json::object())
{
    return {
        {"requestId", std::move(requestId)},
        {"command", std::move(command)},
        {"arguments", std::move(arguments)}};
}
} // namespace

int main()
{
    try
    {
        Expect(
            Prism::Engine::DirectionalLightComponent{}.intensity == 4.0f
                && Prism::Scene::DirectionalLight{}.intensity == 4.0f,
            "Directional light defaults must remain synchronized at intensity 4.");
        Prism::Engine::EntityIdGenerator generator(42);
        const Prism::Engine::EntityId generated = generator.Generate();
        const auto parsed = Prism::Engine::EntityId::Parse(generated.ToString());
        Expect(parsed.has_value() && *parsed == generated, "Entity UUID round trip failed.");

        Prism::Engine::CommandProcessor processor(std::filesystem::current_path());
        const json description = processor.Execute(Request("describe", "engine.describe"));
        Expect(description.at("success").get<bool>(), "Engine description command failed.");
        Expect(description.at("data").at("components").size() == 9, "Component Reflection is incomplete.");

        Prism::Engine::CommandProcessor atomicProcessor(std::filesystem::current_path());
        const json atomicEntity = atomicProcessor.Execute(Request("atomic-create", "entity.create"));
        const std::string atomicId = atomicEntity.at("data").at("entity").get<std::string>();
        const std::string atomicHash = atomicProcessor.ComputeStateHash();
        const json invalidComponent = atomicProcessor.Execute(Request(
            "atomic-invalid",
            "component.add",
            {{"entity", atomicId},
             {"component", "MeshRenderer"},
             {"properties", {{"unknown", true}}}}));
        Expect(!invalidComponent.at("success").get<bool>(), "An invalid component property was accepted.");
        Expect(atomicProcessor.ComputeStateHash() == atomicHash,
               "A failed command left a partial component mutation behind.");
        Expect(atomicProcessor.Execute(Request(
            "atomic-invalid",
            "component.add",
            {{"entity", atomicId}, {"component", "MeshRenderer"}})).value("idempotentReplay", false),
            "Failed request IDs were not idempotent.");

        const json created = processor.Execute(Request("create-1", "entity.create", {{"name", "Cube"}}));
        Expect(created.at("success").get<bool>(), "Entity creation failed.");
        const std::string cubeId = created.at("data").at("entity").get<std::string>();
        const json duplicateRequest = processor.Execute(Request("create-1", "entity.create", {{"name", "Cube"}}));
        Expect(duplicateRequest.value("idempotentReplay", false), "Request idempotency was not applied.");
        Expect(processor.GetWorld().GetEntityCount() == 1, "An idempotent retry created a second entity.");
        Expect(processor.Execute(Request(
            "add-renderer",
            "component.add",
            {{"entity", cubeId},
             {"component", "MeshRenderer"},
             {"properties", {
                 {"meshAsset", "builtin://editor-preview/meshes/cube"},
                 {"materialAsset", "builtin://editor-preview/materials/Preview_Neutral"}}}}))
                   .at("success").get<bool>(),
               "MeshRenderer creation failed.");
        Expect(processor.Execute(Request(
            "add-material-override",
            "component.add",
            {{"entity", cubeId},
             {"component", "MaterialOverride"},
             {"properties", {
                 {"albedoColor", json::array({0.2f, 0.4f, 0.8f, 1.0f})},
                 {"metallic", 0.7f},
                 {"roughness", 0.18f},
                 {"emissiveStrength", 2.5f},
                 {"useNormalTexture", true}}}}))
                   .at("success").get<bool>(),
               "MaterialOverride creation failed.");
        const json materialOverride = processor.Execute(Request(
            "get-material-override",
            "component.get",
            {{"entity", cubeId},
             {"component", "MaterialOverride"}}));
        Expect(materialOverride.at("success").get<bool>()
                   && materialOverride.at("data").at("properties")
                          .at("roughness").get<float>() == 0.18f
                   && materialOverride.at("data").at("properties")
                          .at("useNormalTexture").get<bool>(),
               "MaterialOverride structured state was not readable.");
        const std::string validMaterialHash = processor.ComputeStateHash();
        Expect(!processor.Execute(Request(
            "invalid-material-override",
            "component.set",
            {{"entity", cubeId},
             {"component", "MaterialOverride"},
             {"properties", {{"roughness", 1.5f}}}}))
                    .at("success").get<bool>()
                   && processor.ComputeStateHash() == validMaterialHash,
               "Invalid MaterialOverride input was accepted or partially applied.");
        Prism::Engine::World materialRoundTripWorld;
        Prism::Engine::WorldSerializer::Deserialize(
            Prism::Engine::WorldSerializer::Serialize(
                processor.GetWorld()),
            materialRoundTripWorld);
        const auto roundTripCubeId = Prism::Engine::EntityId::Parse(cubeId);
        const Prism::Engine::EntityRecord* roundTripCube =
            roundTripCubeId.has_value()
            ? materialRoundTripWorld.FindEntity(*roundTripCubeId)
            : nullptr;
        Expect(roundTripCube != nullptr
                   && roundTripCube->materialOverride.has_value()
                   && roundTripCube->materialOverride->metallic == 0.7f
                   && roundTripCube->materialOverride->albedoColor.z == 0.8f,
               "MaterialOverride was not preserved by World serialization.");
        const json spotAdded = processor.Execute(Request(
            "add-spot",
            "component.add",
            {{"entity", cubeId},
             {"component", "SpotLight"},
             {"properties",
              {{"direction", json::array({0.0f, -1.0f, 0.0f})},
               {"range", 24.0f},
               {"innerAngleRadians", 0.3f},
               {"outerAngleRadians", 0.55f},
               {"castsShadow", true}}}}));
        Expect(
            spotAdded.at("success").get<bool>(),
            "SpotLight was not exposed through the command processor.");
        const json spotDescription = processor.Execute(Request(
            "get-spot",
            "component.get",
            {{"entity", cubeId},
             {"component", "SpotLight"}}));
        Expect(
            spotDescription.at("data")
                        .at("properties")
                        .at("castsShadow")
                        .get<bool>()
                && spotDescription.at("data")
                           .at("properties")
                           .at("range")
                           .get<float>()
                    == 24.0f,
            "SpotLight structured state was not readable.");

        const json transform = processor.Execute(Request(
            "set-transform",
            "component.set",
            {{"entity", cubeId},
             {"component", "Transform"},
             {"properties", {{"position", json::array({1.0f, 2.0f, 3.0f})}}}}));
        Expect(transform.at("success").get<bool>(), "Component property update failed.");

        const std::string beforeTransactionHash = processor.ComputeStateHash();
        Expect(processor.Execute(Request("begin", "transaction.begin")).at("success").get<bool>(),
               "Transaction begin failed.");
        const json light = processor.Execute(Request("create-2", "entity.create", {{"name", "Light"}}));
        Expect(light.at("success").get<bool>(), "Entity creation inside a transaction failed.");
        Expect(processor.Execute(Request(
            "step", "simulation.step", {{"count", 60}})).at("success").get<bool>(),
            "Fixed simulation step failed.");
        Expect(processor.Execute(Request("commit", "transaction.commit")).at("success").get<bool>(),
               "Transaction commit failed.");
        const std::string committedHash = processor.ComputeStateHash();
        Expect(committedHash != beforeTransactionHash, "The committed transaction did not change state.");

        Expect(processor.Execute(Request("undo", "transaction.undo")).at("success").get<bool>(),
               "Undo failed.");
        Expect(processor.ComputeStateHash() == beforeTransactionHash, "Undo did not restore the exact state.");
        Expect(processor.Execute(Request("redo", "transaction.redo")).at("success").get<bool>(),
               "Redo failed.");
        Expect(processor.ComputeStateHash() == committedHash, "Redo did not restore the committed state.");

        const json journal = processor.ExportJournal();
        Prism::Engine::CommandProcessor replayed(std::filesystem::current_path());
        std::string replayError;
        Expect(replayed.ReplayJournal(journal, &replayError), "Deterministic journal replay failed.");
        Expect(replayed.ComputeStateHash() == committedHash, "Replay produced a different state hash.");

        Prism::Engine::CommandProcessor renderProcessor(std::filesystem::current_path());
        const std::string cameraId = renderProcessor.Execute(Request(
            "render-camera-create",
            "entity.create",
            {{"name", "CaptureCamera"}})).at("data").at("entity").get<std::string>();
        Expect(renderProcessor.Execute(Request(
            "render-camera-component",
            "component.add",
            {{"entity", cameraId},
             {"component", "Camera"},
             {"properties", {
                 {"fieldOfViewY", 0.785398163f},
                 {"nearPlane", 0.1f},
                 {"farPlane", 250.0f}}}})).at("success").get<bool>(),
            "Capture Camera creation failed.");
        Expect(renderProcessor.Execute(Request(
            "render-camera-transform",
            "component.set",
            {{"entity", cameraId},
             {"component", "Transform"},
             {"properties", {
                 {"position", json::array({8.0f, 5.4f, -12.0f})},
                 {"rotation", json::array({-0.2f, -0.55f, 0.0f})}}}})).at("success").get<bool>(),
            "Capture Camera transform failed.");

        const std::string sunId = renderProcessor.Execute(Request(
            "render-sun-create",
            "entity.create",
            {{"name", "Sun"}})).at("data").at("entity").get<std::string>();
        Expect(renderProcessor.Execute(Request(
            "render-sun-component",
            "component.add",
            {{"entity", sunId},
             {"component", "DirectionalLight"},
             {"properties", {
                 {"direction", json::array({-0.3f, -0.12f, -0.95f})},
                 {"color", json::array({1.0f, 0.94f, 0.84f})},
                 {"intensity", 1.35f}}}})).at("success").get<bool>(),
            "Capture Sun creation failed.");

        const std::filesystem::path snapshotPath =
            std::filesystem::current_path() / "automation/tests/world-render-snapshot.json";
        const std::filesystem::path receiptPath =
            std::filesystem::current_path() / "automation/tests/world-render-receipt.json";
        std::string snapshotError;
        Expect(renderProcessor.SaveSnapshot(snapshotPath, &snapshotError),
               "Engine Snapshot save failed.");

        Prism::Engine::CommandProcessor loadedRenderProcessor(std::filesystem::current_path());
        Prism::Asset::AssetRegistry renderAssets;
        Prism::Scene::RenderScene renderScene;
        const Prism::Scene::WorldRenderSnapshotResult renderWorld =
            Prism::Scene::LoadWorldRenderSnapshot(
                snapshotPath,
                renderProcessor.ComputeStateHash(),
                loadedRenderProcessor,
                renderAssets,
                renderScene);
        Expect(renderWorld.success, "A valid render Snapshot was rejected.");
        Expect(renderWorld.renderedWorldHash == renderProcessor.ComputeStateHash(),
               "The render Snapshot hash changed after loading.");
        Expect(renderWorld.cameraCount == 1 && renderWorld.directionalLightCount == 1,
               "Render Snapshot validation lost the Camera or Sun.");
        Expect(Prism::Scene::WriteWorldRenderReceipt(
                   receiptPath, renderWorld, "Test RHI", &snapshotError),
               "Rendered-world receipt write failed.");
        std::ifstream receiptInput(receiptPath, std::ios::binary);
        const json receipt = json::parse(receiptInput);
        Expect(receipt.at("format") == "PrismRenderedWorldReceipt"
                   && receipt.at("renderedWorldHash") == renderProcessor.ComputeStateHash(),
               "Rendered-world receipt identity is invalid.");

        Prism::Engine::CommandProcessor invalidRenderProcessor(std::filesystem::current_path());
        const std::filesystem::path invalidSnapshotPath =
            std::filesystem::current_path() / "automation/tests/invalid-world-render-snapshot.json";
        Expect(invalidRenderProcessor.SaveSnapshot(invalidSnapshotPath, &snapshotError),
               "Invalid-world Snapshot fixture save failed.");
        Prism::Engine::CommandProcessor invalidLoadedProcessor(std::filesystem::current_path());
        const Prism::Scene::WorldRenderSnapshotResult invalidRenderWorld =
            Prism::Scene::LoadWorldRenderSnapshot(
                invalidSnapshotPath,
                invalidRenderProcessor.ComputeStateHash(),
                invalidLoadedProcessor,
                renderAssets,
                renderScene);
        Expect(!invalidRenderWorld.success
                   && invalidRenderWorld.errorCode == "world_validation_failed",
               "A render Snapshot without Camera and Sun was not rejected structurally.");

        const std::filesystem::path assetProjectRoot(PRISM_RENDER_PROJECT_DIR);
        const std::filesystem::path importServiceRoot =
            assetProjectRoot
            / "build-windows-ci/asset-import-service-tests";
        std::filesystem::remove_all(importServiceRoot);
        Prism::Asset::AssetImportService importService(
            importServiceRoot);
        std::string importServiceError;
        Expect(importService.Initialize(&importServiceError),
               "Editor Asset Import Service initialization failed.");
        const Prism::Asset::AssetImportResult stagedTexture =
            importService.Import(
                assetProjectRoot
                / "assets/scenes/DuckCM.png");
        Expect(stagedTexture.success
                   && stagedTexture.sourcePath.starts_with(
                       "assets/imported/")
                   && std::filesystem::is_regular_file(
                       importServiceRoot
                       / stagedTexture.sourcePath),
               "External editor asset import was not staged inside the project root.");
        std::filesystem::remove_all(importServiceRoot);

        const std::filesystem::path testManifest =
            assetProjectRoot / "build-windows-ci/asset-tests/AssetManifest.json";
        Prism::Asset::AssetDatabase assetDatabase(assetProjectRoot, testManifest);
        std::string assetError;
        Expect(assetDatabase.Load(&assetError), "Asset Manifest load failed.");
        const auto existingImportedScenes = assetDatabase.GetImportedScenes();
        const Prism::Asset::AssetImportResult importedAsset =
            assetDatabase.ImportGltf(
                assetProjectRoot / "assets/scenes/StartupScene.gltf",
                !existingImportedScenes.empty());
        Expect(importedAsset.success, "glTF Asset import failed.");
        Expect(!importedAsset.sourceAssetId.empty()
                   && importedAsset.assets.size() >= 8,
               "Asset import did not create scene subresources.");
        const auto importedMesh = std::ranges::find_if(
            importedAsset.assets,
            [](const Prism::Asset::AssetRecord& asset)
            {
                return asset.type == Prism::Asset::AssetType::Mesh;
            });
        const auto importedMaterial = std::ranges::find_if(
            importedAsset.assets,
            [](const Prism::Asset::AssetRecord& asset)
            {
                return asset.type == Prism::Asset::AssetType::Material;
            });
        Expect(importedMesh != importedAsset.assets.end()
                   && importedMaterial != importedAsset.assets.end()
                   && importedMesh->assetPath.starts_with("prism-asset://")
                   && importedMaterial->assetPath.starts_with("prism-asset://")
                   && importedMesh->metadata.contains("cooked")
                   && importedMaterial->metadata.contains("cooked"),
               "Imported Mesh and Material do not expose stable Asset Paths.");
        const auto importedTexture = std::ranges::find_if(
            importedAsset.assets,
            [](const Prism::Asset::AssetRecord& asset)
            {
                return asset.type == Prism::Asset::AssetType::Texture;
            });
        Expect(importedTexture != importedAsset.assets.end()
                   && importedTexture->metadata.contains("cooked"),
               "Asset import did not produce a Cooked Texture.");

        const std::filesystem::path standaloneTextureSource =
            assetProjectRoot / "assets/scenes/DuckCM.png";
        const bool standaloneTextureExists =
            std::ranges::any_of(
                assetDatabase.List(Prism::Asset::AssetType::Texture),
                [](const Prism::Asset::AssetRecord& record)
                {
                    return record.sourcePath
                        == "assets/scenes/DuckCM.png";
                });
        const Prism::Asset::AssetImportResult
            standaloneTexture = assetDatabase.ImportTexture(
                standaloneTextureSource,
                standaloneTextureExists);
        Expect(standaloneTexture.success
                   && standaloneTexture.assets.size() == 1
                   && standaloneTexture.assets.front().type
                       == Prism::Asset::AssetType::Texture
                   && standaloneTexture.assets.front().metadata
                       .value("standalone", false)
                   && standaloneTexture.assets.front().metadata
                       .value("width", 0u) > 0
                   && standaloneTexture.assets.front().metadata
                       .contains("cooked"),
               "Standalone image import did not create a Cooked Texture Asset.");

        std::shared_ptr<Prism::Asset::MeshAsset> cookedMesh;
        Expect(Prism::Asset::CookedAssetIO::ReadMesh(
                   assetProjectRoot
                       / importedMesh->metadata.at("cooked").at("path")
                             .get<std::string>(),
                   cookedMesh,
                   &assetError)
                   && cookedMesh != nullptr
                   && !cookedMesh->GetVertices().empty()
                    && !cookedMesh->GetIndices().empty(),
                "The imported .prismmesh file did not round trip.");
        const std::filesystem::path cookedMeshPath =
            assetProjectRoot
            / importedMesh->metadata.at("cooked").at("path")
                  .get<std::string>();
        const Prism::Asset::CookedAssetInfo cookedMeshInfo =
            Prism::Asset::CookedAssetIO::Inspect(
                cookedMeshPath);
        Expect(cookedMeshInfo.valid
                   && cookedMeshInfo.containerVersion
                       == Prism::Asset::CookedAssetIO::CurrentVersion
                   && cookedMeshInfo.payloadVersion
                       == Prism::Asset::CookedAssetIO::CurrentPayloadVersion
                   && cookedMeshInfo.checksumVerified,
               "The imported Cooked Mesh is not a validated v2 container.");
        std::shared_ptr<Prism::Asset::TextureAsset> cookedTexture;
        Expect(Prism::Asset::CookedAssetIO::ReadTexture(
                   assetProjectRoot
                       / importedTexture->metadata.at("cooked").at("path")
                             .get<std::string>(),
                   cookedTexture,
                   &assetError)
                   && cookedTexture != nullptr,
               "The imported .prismtex file did not round trip.");
        Prism::Asset::CookedMaterialData cookedMaterial;
        Expect(Prism::Asset::CookedAssetIO::ReadMaterial(
                   assetProjectRoot
                       / importedMaterial->metadata.at("cooked").at("path")
                             .get<std::string>(),
                   cookedMaterial,
                   &assetError)
                   && cookedMaterial.material != nullptr
                   && std::ranges::all_of(
                       cookedMaterial.textureAssetIds,
                       [](const std::string& assetId)
                       {
                           return !assetId.empty();
                       }),
                "The imported .prismmat file did not round trip.");

        const std::filesystem::path legacyMeshPath =
            assetProjectRoot
            / "build-windows-ci/asset-tests/legacy-v1.prismmesh";
        Prism::Asset::CookedAssetWriteOptions legacyOptions{};
        legacyOptions.containerVersion =
            Prism::Asset::CookedAssetIO::LegacyVersion;
        legacyOptions.enableCompression = false;
        Expect(Prism::Asset::CookedAssetIO::WriteMeshWithOptions(
                   legacyMeshPath,
                   *cookedMesh,
                   legacyOptions,
                   &assetError),
               "Legacy Cooked Mesh fixture write failed.");
        std::shared_ptr<Prism::Asset::MeshAsset> legacyMesh;
        Expect(Prism::Asset::CookedAssetIO::ReadMesh(
                   legacyMeshPath,
                   legacyMesh,
                   &assetError)
                   && legacyMesh != nullptr
                   && legacyMesh->GetVertices().size()
                       == cookedMesh->GetVertices().size(),
               "Cooked v2 reader lost v1 backward compatibility.");

        Prism::Asset::TextureAsset compressibleTexture;
        compressibleTexture.SetName("CompressibleTexture");
        std::vector<std::uint8_t> compressiblePixels(
            64 * 64 * 4,
            0);
        for (std::size_t index = 0; index < 127; ++index)
        {
            compressiblePixels[index] =
                static_cast<std::uint8_t>(index);
        }
        compressiblePixels[127] = 42;
        compressiblePixels[128] = 42;
        compressibleTexture.SetImageData(
            64,
            64,
            compressiblePixels);
        const std::filesystem::path compressedTexturePath =
            assetProjectRoot
            / "build-windows-ci/asset-tests/compressed.prismtex";
        Expect(Prism::Asset::CookedAssetIO::WriteTexture(
                   compressedTexturePath,
                   compressibleTexture,
                   &assetError),
               "Compressible Cooked Texture write failed.");
        const Prism::Asset::CookedAssetInfo compressedTextureInfo =
            Prism::Asset::CookedAssetIO::Inspect(
                compressedTexturePath);
        Expect(compressedTextureInfo.valid
                   && compressedTextureInfo.compression
                       == Prism::Asset::CookedAssetCompression::RunLength
                   && compressedTextureInfo.storedBytes
                       < compressedTextureInfo.uncompressedBytes,
               "Cooked v2 did not select compression for repetitive data.");
        std::shared_ptr<Prism::Asset::TextureAsset>
            compressedTextureRoundTrip;
        Expect(Prism::Asset::CookedAssetIO::ReadTexture(
                   compressedTexturePath,
                   compressedTextureRoundTrip,
                   &assetError)
                   && compressedTextureRoundTrip != nullptr
                   && compressedTextureRoundTrip->GetImageData()
                       == compressiblePixels,
               "Cooked v2 RLE literal/run boundary did not round trip.");

        const std::filesystem::path corruptedMeshPath =
            assetProjectRoot
            / "build-windows-ci/asset-tests/corrupted.prismmesh";
        std::filesystem::copy_file(
            cookedMeshPath,
            corruptedMeshPath,
            std::filesystem::copy_options::overwrite_existing);
        {
            std::fstream corrupted(
                corruptedMeshPath,
                std::ios::binary | std::ios::in | std::ios::out);
            corrupted.seekg(-1, std::ios::end);
            char value = 0;
            corrupted.read(&value, 1);
            value ^= 0x5a;
            corrupted.seekp(-1, std::ios::end);
            corrupted.write(&value, 1);
        }
        std::shared_ptr<Prism::Asset::MeshAsset> corruptedMesh;
        Expect(!Prism::Asset::CookedAssetIO::ReadMesh(
                    corruptedMeshPath,
                    corruptedMesh,
                    &assetError)
                   && !Prism::Asset::CookedAssetIO::Inspect(
                           corruptedMeshPath).valid,
               "Cooked v2 accepted a corrupted payload.");

        const Prism::Asset::AssetImportResult reimportedAsset =
            assetDatabase.ImportGltf(
                assetProjectRoot / "assets/scenes/StartupScene.gltf",
                true);
        Expect(reimportedAsset.success
                   && reimportedAsset.importRevision
                       == importedAsset.importRevision + 1,
               "Asset reimport did not advance the import revision.");
        const auto reimportedMesh = std::ranges::find_if(
            reimportedAsset.assets,
            [](const Prism::Asset::AssetRecord& asset)
            {
                return asset.type == Prism::Asset::AssetType::Mesh;
            });
        Expect(reimportedMesh != reimportedAsset.assets.end()
                   && reimportedMesh->assetId == importedMesh->assetId
                   && reimportedMesh->assetPath == importedMesh->assetPath,
               "Asset reimport changed a stable Mesh ID or Asset Path.");
        const auto reimportedScene = std::ranges::find_if(
            reimportedAsset.assets,
            [](const Prism::Asset::AssetRecord& asset)
            {
                return asset.type
                    == Prism::Asset::AssetType::Scene;
            });
        Expect(
            reimportedScene
                    != reimportedAsset.assets.end()
                && reimportedScene->metadata
                    .contains("instances")
                && reimportedScene->metadata
                    .at("instances").is_array()
                && !reimportedScene->metadata
                    .at("instances").empty()
                && reimportedScene->metadata
                    .at("instances").front()
                    .at("worldMatrix").size() == 16,
            "Asset reimport did not persist streamed Scene instance bindings and transforms.");
        std::vector<Prism::Asset::AssetDiagnostic> validationDiagnostics;
        Expect(assetDatabase.ValidateImportedContent(validationDiagnostics),
               "Freshly reimported asset content was reported as stale.");
        Prism::Asset::AssetStreamingConfiguration
            streamingConfiguration{};
        streamingConfiguration.residentBudgetBytes =
            64ull * 1024ull * 1024ull;
        Prism::Asset::AssetStreamingManager streamingManager(
            streamingConfiguration);
        Expect(streamingManager.Initialize(
                   assetProjectRoot,
                   testManifest,
                   &assetError),
               "Asset Streaming Manager initialization failed.");
        Expect(streamingManager.Request(
                   reimportedAsset.sourceAssetId,
                   10,
                   true,
                   &assetError),
               "Scene streaming request or dependency expansion failed.");
        Expect(streamingManager.WaitForIo(
                   std::chrono::seconds(5)),
               "Asynchronous Cooked Asset reads timed out.");
        const Prism::Asset::AssetStreamingStatistics
            streamingStatistics =
                streamingManager.GetStatistics();
        const auto streamingScene =
            streamingManager.Find(
                reimportedAsset.sourceAssetId);
        Expect(streamingStatistics.completedIoCount >= 7
                   && streamingStatistics.ioBytesRead > 0
                   && streamingStatistics.failedCount == 0
                   && streamingStatistics.readyCount >= 7
                   && streamingScene.has_value()
                   && streamingScene->state
                       == Prism::Asset::AssetResidencyState::
                           WaitingForDependencies,
               "Asset Streaming did not read the dependency closure into ReadyForUpload.");
        const auto referencedStreamingEntries =
            streamingManager.GetEntries();
        Expect(
            std::ranges::all_of(
                referencedStreamingEntries,
                [&](const auto& entry)
                {
                    const bool belongsToRequestedScene =
                        entry.assetId
                            == reimportedAsset.sourceAssetId
                        || std::ranges::find(
                            reimportedScene->dependencies,
                            entry.assetId)
                            != reimportedScene->dependencies.end();
                    if (!belongsToRequestedScene)
                    {
                        return true;
                    }
                    return entry.referenceCount > 0
                        && entry.pinned;
                }),
            "Scene streaming did not retain and pin its dependency closure.");
        Expect(
            streamingManager.Release(
                reimportedAsset.sourceAssetId,
                true),
            "Scene streaming release failed.");
        const auto releasedStreamingEntries =
            streamingManager.GetEntries();
        Expect(
            std::ranges::all_of(
                releasedStreamingEntries,
                [](const auto& entry)
                {
                    return entry.referenceCount == 0
                        && !entry.pinned;
                }),
            "Scene streaming release did not symmetrically release and unpin its dependency closure.");
        const auto importedScenes = assetDatabase.GetImportedScenes();
        Expect(importedScenes.size() == 1,
               "Asset import did not produce one cached Scene record.");
        const Prism::Asset::AssetCacheBundle cacheBundle =
            Prism::Asset::AssetCache(assetProjectRoot).Resolve(
                importedScenes.front());
        Expect(cacheBundle.valid
                   && cacheBundle.files.size() == 3
                   && std::filesystem::is_regular_file(
                       cacheBundle.cachedSourcePath),
                "The content-addressed asset cache bundle is invalid.");

        const std::filesystem::path gcProjectRoot =
            assetProjectRoot
            / "build-windows-ci/asset-cache-gc-tests";
        const std::filesystem::path referencedAssetDirectory =
            gcProjectRoot
            / "automation/cache/assets/referenced-hash";
        const std::filesystem::path orphanAssetDirectory =
            gcProjectRoot
            / "automation/cache/assets/orphan-hash";
        const std::filesystem::path orphanCookedDirectory =
            gcProjectRoot
            / "automation/cache/cooked/orphan-hash";
        std::filesystem::create_directories(
            referencedAssetDirectory);
        std::filesystem::create_directories(
            orphanAssetDirectory);
        std::filesystem::create_directories(
            orphanCookedDirectory);
        {
            std::ofstream(referencedAssetDirectory / "keep.bin",
                          std::ios::binary)
                << "keep";
            std::ofstream(orphanAssetDirectory / "source.bin",
                          std::ios::binary)
                << "orphan-source";
            std::ofstream(orphanCookedDirectory / "asset.bin",
                          std::ios::binary)
                << "orphan-cooked";
        }
        Prism::Asset::AssetCacheGarbageCollectionOptions gcOptions{};
        gcOptions.dryRun = true;
        const Prism::Asset::AssetCacheGarbageCollectionResult gcPreview =
            Prism::Asset::AssetCache(gcProjectRoot).CollectGarbage(
                {"referenced-hash"},
                gcOptions);
        const auto orphanPreview = std::ranges::find_if(
            gcPreview.entries,
            [](const Prism::Asset::AssetCacheGarbageCollectionEntry& entry)
            {
                return entry.contentHash == "orphan-hash";
            });
        Expect(gcPreview.success
                   && gcPreview.selectedEntryCount == 1
                   && gcPreview.removedEntryCount == 0
                   && orphanPreview != gcPreview.entries.end()
                   && orphanPreview->selected
                   && std::filesystem::is_directory(
                       orphanAssetDirectory),
               "Asset Cache GC dry-run did not report the orphan safely.");
        gcOptions.dryRun = false;
        const Prism::Asset::AssetCacheGarbageCollectionResult gcApplied =
            Prism::Asset::AssetCache(gcProjectRoot).CollectGarbage(
                {"referenced-hash"},
                gcOptions);
        Expect(gcApplied.success
                   && gcApplied.removedEntryCount == 1
                   && std::filesystem::is_directory(
                       referencedAssetDirectory)
                   && !std::filesystem::exists(
                       orphanAssetDirectory)
                   && !std::filesystem::exists(
                       orphanCookedDirectory),
               "Asset Cache GC removed a referenced entry or kept an orphan.");

        const Prism::Automation::PerformanceStatistics performanceStatistics =
            Prism::Automation::PerformanceBaseline::ComputeStatistics(
                {10.0, 12.0, 11.0, 13.0});
        Expect(std::abs(performanceStatistics.meanMilliseconds - 11.5) < 0.001
                   && std::abs(performanceStatistics.medianMilliseconds - 11.5) < 0.001
                   && performanceStatistics.p95Milliseconds > 12.8,
               "Performance percentile statistics are invalid.");
        Prism::Automation::PerformanceBaselineRecord performanceBaseline{};
        performanceBaseline.identity.api = "d3d12";
        performanceBaseline.identity.stage = "tonemap";
        performanceBaseline.identity.width = 800;
        performanceBaseline.identity.height = 450;
        performanceBaseline.identity.worldHash = "world-hash";
        performanceBaseline.identity.assetManifestHash =
            "manifest-hash";
        performanceBaseline.identity.adapterName = "Test Adapter";
        performanceBaseline.identity.adapterVendorId = 0x10de;
        performanceBaseline.identity.adapterDeviceId = 0x1234;
        performanceBaseline.identity.dedicatedVideoMemoryBytes =
            8ull * 1024ull * 1024ull * 1024ull;
        performanceBaseline.identity.driverVersionRaw = 42;
        performanceBaseline.identity.driverVersion = "1.2.3.4";
        performanceBaseline.identity.apiVersion = "test-api";
        performanceBaseline.identity.cpuName = "Test CPU";
        performanceBaseline.identity.buildConfiguration = "Debug";
        performanceBaseline.identity.compiler = "MSVC Test";
        performanceBaseline.identity.architecture = "x64";
        performanceBaseline.identity.shaderRevision = "shader-a";
        performanceBaseline.identity.shaderFileCount = 5;
        performanceBaseline.identity.executableHash = "binary-a";
        performanceBaseline.statistics = performanceStatistics;
        performanceBaseline.sampleCount = 4;
        const std::filesystem::path performanceBaselinePath =
            assetProjectRoot
            / "build-windows-ci/performance-tests/baseline.json";
        Expect(Prism::Automation::PerformanceBaseline::Save(
                   performanceBaselinePath,
                   performanceBaseline,
                   &assetError),
               "Performance baseline save failed.");
        Prism::Automation::PerformanceBaselineRecord loadedPerformanceBaseline{};
        Expect(Prism::Automation::PerformanceBaseline::Load(
                   performanceBaselinePath,
                   loadedPerformanceBaseline,
                   &assetError),
                "Performance baseline load failed.");
        Expect(loadedPerformanceBaseline.identity.adapterVendorId
                   == performanceBaseline.identity.adapterVendorId
                   && loadedPerformanceBaseline.identity.cpuName
                       == performanceBaseline.identity.cpuName
                   && loadedPerformanceBaseline.identity.executableHash
                       == performanceBaseline.identity.executableHash,
               "Performance baseline v2 identity was not preserved.");
        Prism::Automation::PerformanceBaselineRecord currentPerformance =
            loadedPerformanceBaseline;
        currentPerformance.statistics.medianMilliseconds *= 1.10;
        currentPerformance.statistics.p95Milliseconds *= 1.10;
        Expect(Prism::Automation::PerformanceBaseline::Compare(
                   loadedPerformanceBaseline,
                   currentPerformance,
                   15.0).passed,
               "A performance sample within tolerance was rejected.");
        currentPerformance.statistics.medianMilliseconds *= 1.20;
        currentPerformance.statistics.p95Milliseconds *= 1.20;
        Expect(!Prism::Automation::PerformanceBaseline::Compare(
                    loadedPerformanceBaseline,
                    currentPerformance,
                    15.0).passed,
                "A performance regression above tolerance was accepted.");
        Prism::Automation::PerformanceBaselineRecord incompatiblePerformance =
            loadedPerformanceBaseline;
        ++incompatiblePerformance.identity.driverVersionRaw;
        Expect(!Prism::Automation::PerformanceBaseline::Compare(
                    loadedPerformanceBaseline,
                    incompatiblePerformance,
                    100.0).identityMatches,
               "A different GPU driver was accepted as the same performance environment.");

        const std::filesystem::path cpuTracePath =
            assetProjectRoot
            / "build-windows-ci/performance-tests/cpu-trace.json";
        Prism::Core::CpuTrace::Initialize(
            cpuTracePath,
            "cpu-trace-test");
        {
            Prism::Core::CpuTraceSpan parent(
                "Parent",
                "test");
            Prism::Core::CpuTraceSpan child(
                "Child",
                "test");
        }
        Expect(Prism::Core::CpuTrace::Flush(true, 0, &assetError),
               "CPU Trace flush failed.");
        Prism::Core::CpuTrace::Initialize({}, {});
        std::ifstream cpuTraceInput(cpuTracePath, std::ios::binary);
        json cpuTraceDocument;
        cpuTraceInput >> cpuTraceDocument;
        const auto parentSpan = std::ranges::find_if(
            cpuTraceDocument.at("spans"),
            [](const json& span)
            {
                return span.at("name") == "Parent";
            });
        const auto childSpan = std::ranges::find_if(
            cpuTraceDocument.at("spans"),
            [](const json& span)
            {
                return span.at("name") == "Child";
            });
        Expect(cpuTraceDocument.at("format") == "PrismCpuTrace"
                   && cpuTraceDocument.at("correlationId")
                       == "cpu-trace-test"
                   && parentSpan != cpuTraceDocument.at("spans").end()
                   && childSpan != cpuTraceDocument.at("spans").end()
                   && childSpan->at("parentSpanId")
                       == parentSpan->at("id"),
               "CPU Trace nesting or correlation contract is invalid.");

        Prism::Renderer::RuntimePerformanceIdentity runtimeIdentity{};
        runtimeIdentity.graphicsApi = "d3d12";
        runtimeIdentity.adapter.name = "Test Adapter";
        runtimeIdentity.adapter.vendorId = 0x10de;
        runtimeIdentity.cpuName = "Test CPU";
        runtimeIdentity.buildConfiguration = "Debug";
        runtimeIdentity.compiler = "MSVC Test";
        runtimeIdentity.architecture = "x64";
        runtimeIdentity.shaderRevision = "shader-a";
        runtimeIdentity.executableHash = "binary-a";
        const json serializedRuntimeIdentity =
            Prism::Renderer::SerializeRuntimePerformanceIdentity(
                runtimeIdentity);
        Expect(serializedRuntimeIdentity.at("format")
                   == "PrismRuntimePerformanceIdentity"
                   && serializedRuntimeIdentity.at("identity")
                       .at("adapterName") == "Test Adapter"
                   && serializedRuntimeIdentity.at("identity")
                       .at("graphicsApi") == "d3d12",
               "Runtime performance identity serialization is invalid.");
        const std::array<Prism::RHI::GpuProfiler::Timing, 3>
            gpuTimings = {{
                {"GBuffer", 1.5f,
                 Prism::RHI::CommandQueueType::Graphics,
                 0.5, 2.0, true},
                {"Bloom", 1.5f,
                 Prism::RHI::CommandQueueType::Compute,
                 1.0, 2.5, true},
                {"Renderer", 3.0f,
                 Prism::RHI::CommandQueueType::Graphics,
                 0.0, 3.0, true}}};
        Prism::RHI::GpuProfiler::TimelineMetadata
            timelineMetadata{};
        timelineMetadata.crossQueueCalibrated = true;
        timelineMetadata.calibrationMethod = "test";
        const json gpuTimingReport =
            Prism::Renderer::BuildGpuTimingReport(
                gpuTimings,
                Prism::RHI::GraphicsApi::Direct3D12,
                timelineMetadata);
        Expect(gpuTimingReport.at("format")
                       == "PrismGpuTimingReport"
                   && gpuTimingReport.at("version") == 2
                   && gpuTimingReport.at("graphicsApi")
                       == "Direct3D 12"
                   && gpuTimingReport.at("passes").size() == 3
                   && gpuTimingReport.at("passes").at(0).at("name")
                       == "GBuffer"
                   && gpuTimingReport.at("passes").at(1).at("queue")
                       == "Compute"
                   && gpuTimingReport.at("timeline")
                       .at("crossQueueCalibrated").get<bool>()
                   && gpuTimingReport.at("timeline")
                       .at("overlapMilliseconds").get<double>() == 1.0,
               "The unified GPU timing report contract is invalid.");

        for (const auto extent : {std::array{1280u, 800u}, std::array{2560u, 1417u}})
        {
            Prism::Renderer::OceanStatistics waterStatistics{};
            waterStatistics.waterOpticsActive = true;
            waterStatistics.outputWidth = extent[0];
            waterStatistics.outputHeight = extent[1];
            waterStatistics.waterCoverageAvailable = true;
            waterStatistics.waterPixelCoverage = 0.5f;
            const Prism::Renderer::RenderGraph emptyGraph;
            const auto metadata = Prism::Renderer::BuildWaterBenchmarkMetadata(
                Prism::Renderer::OceanSettings::HpWaterReference(), waterStatistics, emptyGraph);
            const auto report = Prism::Renderer::BuildGpuTimingReport(
                {}, Prism::RHI::GraphicsApi::Direct3D12, {}, metadata);
            Expect(report.at("capture").at("outputExtent").at(0) == extent[0]
                && report.at("capture").at("outputExtent").at(1) == extent[1]
                && metadata.contains("cpuStages") && metadata.contains("waterMemoryMiB")
                && metadata.contains("waterCoverage") && metadata.contains("waterDispatches")
                && metadata.contains("waterDraws") && metadata.contains("activeViews")
                && metadata.contains("medium") && metadata.contains("history")
                && metadata.at("waterCoverage").at("available").get<bool>(),
                "Water benchmark metadata lost required diagnostics or extent.");
        }

        const json legacy = {
            {"format", "PrismEngineWorld"},
            {"version", 1},
            {"entities", json::array({{
                {"id", "00000000-0000-4000-8000-000000000001"},
                {"name", "LegacyCube"},
                {"position", json::array({2.0f, 0.0f, 0.0f})},
                {"rotation", json::array({0.0f, 0.0f, 0.0f})},
                {"scale", json::array({1.0f, 1.0f, 1.0f})},
                {"mesh", "assets/mesh/cube.gltf"},
                {"material", "assets/material/default"},
                {"visible", true}}})}};
        Prism::Engine::World migrated;
        Prism::Engine::WorldSerializer::Deserialize(legacy, migrated);
        Expect(migrated.GetEntityCount() == 1, "Version 1 world migration lost its entity.");
        Expect(migrated.GetEntities().begin()->second.meshRenderer.has_value(),
               "Version 1 world migration lost MeshRenderer data.");
        Expect(Prism::Engine::WorldSerializer::Serialize(migrated).at("version").get<int>() == 2,
               "Migrated worlds were not written as version 2.");

        const json renderSceneVersion1 = {
            {"format", "PrismRenderScene"},
            {"version", 1},
            {"camera", {
                {"position", json::array({0.0f, 2.0f, -5.0f})},
                {"pitch", 0.1f}, {"yaw", 0.2f},
                {"fieldOfViewY", 0.8f}, {"nearPlane", 0.1f}, {"farPlane", 500.0f}}},
            {"directionalLight", {
                {"direction", json::array({0.0f, -1.0f, 0.0f})},
                {"color", json::array({1.0f, 0.9f, 0.8f})},
                {"intensity", 2.0f}}},
            {"pointLights", json::array({{
                {"position", json::array({1.0f, 2.0f, 3.0f})},
                {"color", json::array({1.0f, 0.0f, 0.0f})},
                {"intensity", 3.0f}, {"range", 12.0f}}})},
            {"objects", json::array({{
                {"name", "OldRenderObject"}, {"visible", true},
                {"mesh", {{"path", "assets/mesh/cube.gltf"}, {"handle", 1}}},
                {"material", {{"path", "assets/material/default"}, {"handle", 2}}},
                {"transform", {
                    {"position", json::array({0.0f, 0.0f, 0.0f})},
                    {"rotation", json::array({0.0f, 0.0f, 0.0f})},
                    {"scale", json::array({1.0f, 1.0f, 1.0f})}}}}})}};
        Prism::Engine::World migratedRenderScene;
        Prism::Engine::WorldSerializer::Deserialize(renderSceneVersion1, migratedRenderScene);
        Expect(migratedRenderScene.GetEntityCount() == 4,
               "PrismRenderScene version 1 migration lost camera, lights, or objects.");
        std::size_t cameraCount = 0;
        std::size_t directionalLightCount = 0;
        std::size_t pointLightCount = 0;
        std::size_t meshRendererCount = 0;
        for (const auto& [id, entity] : migratedRenderScene.GetEntities())
        {
            (void)id;
            cameraCount += entity.camera.has_value() ? 1u : 0u;
            directionalLightCount += entity.directionalLight.has_value() ? 1u : 0u;
            pointLightCount += entity.pointLight.has_value() ? 1u : 0u;
            meshRendererCount += entity.meshRenderer.has_value() ? 1u : 0u;
        }
        Expect(cameraCount == 1 && directionalLightCount == 1
                   && pointLightCount == 1 && meshRendererCount == 1,
               "PrismRenderScene components were not migrated to ECS components.");

        std::istringstream commands(
            "{\"requestId\":\"status-1\",\"command\":\"world.status\",\"arguments\":{}}\n"
            "{\"requestId\":\"status-2\",\"command\":\"entity.list\",\"arguments\":{}}\n");
        std::ostringstream output;
        const Prism::Automation::HarnessRunner runner;
        Expect(runner.Run(commands, output, replayed) == 0, "JSON Lines Harness execution failed.");
        Expect(std::ranges::count(output.str(), '\n') == 2, "Harness did not produce one JSON result per command.");

        Prism::Automation::HarnessTools tools(
            std::filesystem::current_path(), std::filesystem::current_path());
        std::istringstream describeTools(
            "{\"requestId\":\"describe-tools\",\"command\":\"engine.describe\",\"arguments\":{}}\n");
        std::ostringstream describeOutput;
        const Prism::Automation::HarnessRunner toolRunner(&tools);
        Expect(toolRunner.Run(describeTools, describeOutput, replayed) == 0,
               "Harness tool discovery failed.");
        const json toolDescription = json::parse(describeOutput.str());
        const auto& toolCommands = toolDescription.at("data").at("commands");
        Expect(std::ranges::find(toolCommands, "shader.compile") != toolCommands.end()
                   && std::ranges::find(toolCommands, "asset.list") != toolCommands.end()
                   && std::ranges::find(toolCommands, "asset.cache.status") != toolCommands.end()
                   && std::ranges::find(toolCommands, "asset.cache.gc") != toolCommands.end()
                   && std::ranges::find(toolCommands, "asset.reimport") != toolCommands.end()
                   && std::ranges::find(toolCommands, "asset.streaming.plan") != toolCommands.end()
                   && std::ranges::find(toolCommands, "asset.streaming.validate") != toolCommands.end()
                   && std::ranges::find(toolCommands, "crash.inspect") != toolCommands.end()
                   && std::ranges::find(toolCommands, "crash.symbolize") != toolCommands.end()
                   && std::ranges::find(toolCommands, "render.compare_apis") != toolCommands.end()
                   && std::ranges::find(toolCommands, "rdg.describe") != toolCommands.end()
                   && std::ranges::find(toolCommands, "performance.measure") != toolCommands.end()
                   && std::ranges::find(
                       toolCommands,
                       "performance.compare_queue_modes")
                       != toolCommands.end(),
               "engine.describe did not expose the rendering automation commands.");
        Expect(toolDescription.at("data").at("capabilities").at("stableAssetIds").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                        .at("contentAddressedAssetCache").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                        .at("assetCacheGarbageCollection").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                        .at("assetCacheGcDryRun").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                        .at("asynchronousAssetStreaming").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                        .at("assetStreamingRuntimeValidation").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                        .at("assetStreamingSceneActivation").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                        .at("assetStreamingCrossApiGoldenImage").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                        .at("cookedAssetV2").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                        .at("cookedAssetChecksums").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("structuredChildProcessLogs").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("windowsMinidumps").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("pdbSymbolization").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("offlineMinidumpSymbolization").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("buildPdbIdentityMatching").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("rdgPassCulling").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("rdgResourceLifetimes").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("rdgTransientAliasingPlan").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("rdgQueueSchedule").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("rdgDagQueueBatches").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("rdgHistoricalGpuCostModel").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("parallelCommandRecording").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("deferredQueueBatchSubmission").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("nativeMultiQueueInfrastructure").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("independentQueueBatchSubmission").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("nativeTransientAliasing").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("d3d12PlacedResources").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("vulkanAliasMemory").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("nativeAliasingBarriers").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("nativeAsyncCompute").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("computeBloom").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("computeHiZ").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("crossQueueGpuTimestamps").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("queueModeComparison").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("asyncComputeWorkloadBenchmark").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                       .at("cookedAssetFormats").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                        .at("performanceBaselines").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                        .at("runtimePerformanceIdentity").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                        .at("cpuSpanTrace").get<bool>()
                   && toolDescription.at("data").at("capabilities")
                        .at("cpuSpanBudgets").get<bool>()
                   && toolDescription.at("data").at("supportedAssetFormats").size() == 2
                   && toolDescription.at("data")
                       .at("supportedCookedAssetFormats").size() == 3,
                "engine.describe did not expose the Asset Pipeline capabilities.");

        Prism::Automation::McpServer mcpServer(tools, replayed);
        const json mcpInitialize = mcpServer.HandleMessage({
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "initialize"},
            {"params", {{"protocolVersion", "test-version"}}}});
        const json mcpTools = mcpServer.HandleMessage({
            {"jsonrpc", "2.0"},
            {"id", 2},
            {"method", "tools/list"}});
        const json mcpDescribe = mcpServer.HandleMessage({
            {"jsonrpc", "2.0"},
            {"id", 3},
            {"method", "tools/call"},
            {"params",
             {{"name", "prism.describe"},
              {"arguments", json::object()}}}});
        Expect(
            mcpInitialize.at("result").at("protocolVersion")
                    == "test-version"
                && mcpTools.at("result").at("tools").size() == 2
                && mcpDescribe.at("result")
                       .at("structuredContent")
                       .at("success")
                       .get<bool>()
                && !mcpDescribe.at("result")
                        .at("isError")
                        .get<bool>(),
            "MCP initialize, tool discovery, or Harness dispatch failed.");

        const json cacheGcPreview = tools.Execute(Request(
            "cache-gc-preview",
            "asset.cache.gc",
            {{"dryRun", true},
             {"minimumUnusedAgeSeconds", 0}}), replayed);
        Expect(cacheGcPreview.at("success").get<bool>()
                   && cacheGcPreview.at("data").at("dryRun")
                       .get<bool>()
                   && cacheGcPreview.at("data").contains(
                       "reclaimableBytes"),
               "Harness asset.cache.gc dry-run contract is invalid.");

        const std::filesystem::path crashReportPath =
            std::filesystem::current_path()
            / "automation/tests/inspect-crash.json";
        const std::filesystem::path minidumpPath =
            std::filesystem::current_path()
            / "automation/tests/inspect-crash.dmp";
        std::filesystem::create_directories(
            crashReportPath.parent_path());
        {
            std::ofstream minidump(
                minidumpPath,
                std::ios::binary | std::ios::trunc);
            minidump << "test";
            std::ofstream crashReport(
                crashReportPath,
                std::ios::binary | std::ios::trunc);
            crashReport << json{
                {"format", "PrismCrashReport"},
                {"version", 1},
                {"kind", "cpp_exception"},
                {"message", "test failure"},
                {"graphicsApi", "d3d12"},
                {"exitCode", 1},
                {"minidumpPath", minidumpPath.generic_string()},
                {"minidumpWritten", true},
                {"exceptionSymbol", {{"symbol", "TestSymbol"}}},
                {"stack", json::array(
                    {{{"symbol", "TestFrame"}}})},
                {"details", json::object()}}.dump(2);
        }
        const json inspectedCrash = tools.Execute(Request(
            "inspect-crash",
            "crash.inspect",
            {{"path", crashReportPath.generic_string()}}), replayed);
        Expect(inspectedCrash.at("success").get<bool>()
                   && inspectedCrash.at("data").at("minidumpExists")
                       .get<bool>()
                   && inspectedCrash.at("data").at("stack").size() == 1,
               "Harness crash.inspect did not expose Minidump and PDB data.");

        const json restrictedOutput = tools.Execute(Request(
            "restricted-tool-output",
            "shader.compile",
            {{"path", "assets/shaders/Mesh.hlsl"},
             {"entryPoint", "PSMain"},
             {"stage", "pixel"},
             {"format", "dxil"},
             {"output", "assets/shaders/forbidden.dxil"}}), replayed);
        Expect(!restrictedOutput.at("success").get<bool>()
                   && restrictedOutput.at("error").at("code") == "output_path_restricted",
               "Harness generated output was allowed to overwrite a source directory.");

        const json escapedPath = processor.Execute(Request(
            "outside", "world.save", {{"path", "../outside.prismworld.json"}}));
        Expect(!escapedPath.at("success").get<bool>()
                   && escapedPath.at("error").at("code") == "path_outside_project",
               "Harness project-root path confinement failed.");

        std::cout << "PrismEngine Harness tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "PrismEngine Harness tests failed: " << exception.what() << '\n';
        return 1;
    }
}
