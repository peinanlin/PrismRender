#include "Scene/WorldRenderSnapshot.h"

#include "Asset/AssetRegistry.h"
#include "Core/CpuTrace.h"
#include "Engine/CommandSystem.h"
#include "Engine/World.h"
#include "Scene/RenderScene.h"

#include <fstream>
#include <stdexcept>
#include <utility>

#include <json.hpp>

namespace Prism::Scene
{
namespace
{
using json = nlohmann::json;

json SerializeUnresolvedAssets(const std::vector<UnresolvedRenderAsset>& assets)
{
    json serialized = json::array();
    for (const UnresolvedRenderAsset& asset : assets)
    {
        serialized.push_back({
            {"entityId", asset.entityId},
            {"entityName", asset.entityName},
            {"assetType", asset.assetType},
            {"assetPath", asset.assetPath}});
    }
    return serialized;
}

json SerializeAssetDiagnostics(
    const std::vector<Asset::AssetDiagnostic>& diagnostics)
{
    json serialized = json::array();
    for (const Asset::AssetDiagnostic& diagnostic : diagnostics)
    {
        serialized.push_back({
            {"severity", Asset::AssetDatabase::ToString(diagnostic.severity)},
            {"code", diagnostic.code},
            {"message", diagnostic.message},
            {"path", diagnostic.path}});
    }
    return serialized;
}

void Fail(
    WorldRenderSnapshotResult& result,
    std::string code,
    std::string message)
{
    result.success = false;
    result.errorCode = std::move(code);
    result.errorMessage = std::move(message);
}
} // namespace

WorldRenderSnapshotResult LoadWorldRenderSnapshot(
    const std::filesystem::path& snapshotPath,
    std::string expectedWorldHash,
    Engine::CommandProcessor& processor,
    const Asset::AssetRegistry& assetRegistry,
    RenderScene& scene)
{
    Core::CpuTraceSpan traceSpan(
        "WorldSnapshotLoad",
        "world");
    WorldRenderSnapshotResult result{};
    result.snapshotPath = std::filesystem::absolute(snapshotPath).lexically_normal();
    result.expectedWorldHash = std::move(expectedWorldHash);

    std::string loadError;
    if (!processor.LoadSnapshotFile(result.snapshotPath, &loadError))
    {
        Fail(result, "snapshot_load_failed", loadError);
        return result;
    }

    result.renderedWorldHash = processor.ComputeStateHash();
    if (!result.expectedWorldHash.empty()
        && result.renderedWorldHash != result.expectedWorldHash)
    {
        Fail(
            result,
            "world_hash_mismatch",
            "The renderer loaded a snapshot whose state hash differs from the Harness hash.");
        return result;
    }

    const Engine::World& world = processor.GetWorld();
    result.entityCount = world.GetEntityCount();
    for (const auto& [id, entity] : world.GetEntities())
    {
        (void)id;
        result.cameraCount += entity.camera.has_value() ? 1u : 0u;
        result.directionalLightCount += entity.directionalLight.has_value() ? 1u : 0u;
    }

    if (result.cameraCount != 1 || result.directionalLightCount != 1)
    {
        Fail(
            result,
            "world_validation_failed",
            "Deterministic rendering requires exactly one Camera and one DirectionalLight component.");
        return result;
    }

    WorldRenderSyncResult syncResult =
        WorldRenderSceneBridge::SynchronizeToRenderScene(world, assetRegistry, scene);
    result.renderObjectCount = syncResult.renderObjectCount;
    result.unresolvedAssets = std::move(syncResult.unresolvedAssets);
    if (!result.unresolvedAssets.empty())
    {
        Fail(
            result,
            "unresolved_render_assets",
            "The Engine World contains MeshRenderer asset paths that are not available to the renderer.");
        return result;
    }

    result.success = true;
    return result;
}

bool WriteWorldRenderReceipt(
    const std::filesystem::path& path,
    const WorldRenderSnapshotResult& result,
    const std::string_view graphicsApi,
    std::string* outError)
{
    try
    {
        if (path.empty())
        {
            return true;
        }
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }

        json receipt = {
            {"format", "PrismRenderedWorldReceipt"},
            {"version", 1},
            {"success", result.success},
            {"graphicsApi", std::string(graphicsApi)},
            {"snapshotPath", result.snapshotPath.generic_string()},
            {"expectedWorldHash", result.expectedWorldHash},
            {"renderedWorldHash", result.renderedWorldHash},
            {"expectedAssetManifestHash", result.expectedAssetManifestHash},
            {"renderedAssetManifestHash", result.renderedAssetManifestHash},
            {"loadedAssetSourceCount", result.loadedAssetSourceCount},
            {"assetCacheHitCount", result.assetCacheHitCount},
            {"assetCacheMissCount", result.assetCacheMissCount},
            {"assetCachedBytes", result.assetCachedBytes},
            {"cookedAssetCount", result.cookedAssetCount},
            {"cookedAssetBytes", result.cookedAssetBytes},
            {"entityCount", result.entityCount},
            {"cameraCount", result.cameraCount},
            {"directionalLightCount", result.directionalLightCount},
            {"renderObjectCount", result.renderObjectCount},
            {"unresolvedAssets", SerializeUnresolvedAssets(result.unresolvedAssets)},
            {"assetDiagnostics", SerializeAssetDiagnostics(result.assetDiagnostics)}};
        if (!result.success)
        {
            receipt["error"] = {
                {"code", result.errorCode},
                {"message", result.errorMessage}};
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error("Could not open the rendered-world receipt for writing.");
        }
        output << receipt.dump(2) << '\n';
        if (!output)
        {
            throw std::runtime_error("Failed while writing the rendered-world receipt.");
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr) *outError = exception.what();
        return false;
    }
}
} // namespace Prism::Scene
