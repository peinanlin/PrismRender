#include "Asset/AssetStreamingManager.h"

#include "Asset/AssetRegistry.h"
#include "Asset/Material.h"
#include "Asset/MaterialAsset.h"
#include "Asset/Mesh.h"
#include "Asset/MeshAsset.h"
#include "Asset/Texture.h"
#include "Asset/TextureAsset.h"
#include "Core/CpuTrace.h"
#include "RHI/IGraphicsDevice.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <ranges>
#include <set>
#include <stdexcept>
#include <type_traits>

#include <json.hpp>

namespace Prism::Asset
{
namespace
{
std::string Lowercase(std::string value)
{
    for (char& character : value)
    {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

bool IsPathWithin(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate)
{
    auto rootIterator = root.begin();
    auto candidateIterator = candidate.begin();
    for (; rootIterator != root.end();
         ++rootIterator, ++candidateIterator)
    {
        if (candidateIterator == candidate.end()
            || Lowercase(rootIterator->string())
                != Lowercase(candidateIterator->string()))
        {
            return false;
        }
    }
    return true;
}

std::string_view ExpectedCookedFormat(const AssetType type)
{
    switch (type)
    {
    case AssetType::Mesh: return "PrismCookedMesh";
    case AssetType::Texture: return "PrismCookedTexture";
    case AssetType::Material: return "PrismCookedMaterial";
    default: return {};
    }
}

Material::Parameters CreateMaterialParameters(
    const MaterialAsset& asset)
{
    Material::Parameters parameters{};
    parameters.albedoColor = asset.GetAlbedoColor();
    parameters.specularColor = asset.GetSpecularColor();
    parameters.emissiveColor = asset.GetEmissiveColor();
    parameters.metallic = asset.GetMetallic();
    parameters.roughness = asset.GetRoughness();
    parameters.shininess = asset.GetShininess();
    parameters.occlusionStrength = asset.GetOcclusionStrength();
    parameters.normalScale = asset.GetNormalScale();
    parameters.emissiveStrength = asset.GetEmissiveStrength();
    parameters.alphaCutoff = asset.GetAlphaCutoff();
    parameters.alphaMode = asset.GetAlphaMode();
    parameters.doubleSided = asset.GetDoubleSided();
    parameters.useAlbedoTexture = asset.GetUseAlbedoTexture();
    parameters.useMetallicRoughnessTexture =
        asset.GetUseMetallicRoughnessTexture();
    parameters.useNormalTexture = asset.GetUseNormalTexture();
    parameters.useOcclusionTexture =
        asset.GetUseOcclusionTexture();
    parameters.useEmissiveTexture =
        asset.GetUseEmissiveTexture();
    return parameters;
}

template<typename PayloadT>
std::uint64_t EstimatePayloadBytes(const PayloadT& payload)
{
    if (const auto* mesh =
            std::get_if<std::shared_ptr<MeshAsset>>(&payload))
    {
        if (*mesh == nullptr)
        {
            return 0;
        }
        return (*mesh)->GetVertices().size() * sizeof(MeshVertex)
            + (*mesh)->GetIndices().size() * sizeof(std::uint16_t);
    }
    if (const auto* texture =
            std::get_if<std::shared_ptr<TextureAsset>>(&payload))
    {
        if (*texture == nullptr)
        {
            return 0;
        }
        return (*texture)->HasImageData()
            ? (*texture)->GetImageData().size()
            : 4u;
    }
    return 0;
}
} // namespace

AssetStreamingManager::AssetStreamingManager(
    AssetStreamingConfiguration configuration)
    : m_configuration(configuration)
{
    m_statistics.residentBudgetBytes =
        configuration.residentBudgetBytes;
}

AssetStreamingManager::~AssetStreamingManager()
{
    StopWorker();
}

bool AssetStreamingManager::IoJobCompare::operator()(
    const IoJob& left,
    const IoJob& right) const
{
    if (left.priority != right.priority)
    {
        return left.priority < right.priority;
    }
    return left.sequence > right.sequence;
}

bool AssetStreamingManager::Initialize(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& manifestPath,
    std::string* outError)
{
    StopWorker();

    AssetDatabase database(projectRoot, manifestPath);
    std::string error;
    if (!database.Load(&error))
    {
        if (outError != nullptr)
        {
            *outError = error;
        }
        return false;
    }

    const std::filesystem::path normalizedRoot =
        std::filesystem::absolute(projectRoot).lexically_normal();
    std::unordered_map<std::string, Entry> entries;
    std::unordered_map<std::string, std::string> paths;
    for (const AssetRecord& record : database.List())
    {
        if (record.builtIn)
        {
            continue;
        }

        Entry entry{};
        entry.record = record;
        if (record.type != AssetType::Scene)
        {
            const nlohmann::json cooked = record.metadata.value(
                "cooked",
                nlohmann::json::object());
            if (cooked.value("format", std::string{})
                    != ExpectedCookedFormat(record.type)
                || !CookedAssetIO::IsSupportedVersion(
                    cooked.value("version", 0u))
                || cooked.value("path", std::string{}).empty())
            {
                continue;
            }
            entry.cookedPath = std::filesystem::absolute(
                normalizedRoot
                / cooked.at("path").get<std::string>())
                                   .lexically_normal();
            if (!IsPathWithin(normalizedRoot, entry.cookedPath))
            {
                if (outError != nullptr)
                {
                    *outError =
                        "A Cooked Asset path escapes the project root: "
                        + record.assetPath;
                }
                return false;
            }
        }
        paths.emplace(record.assetPath, record.assetId);
        entries.emplace(record.assetId, std::move(entry));
    }

    {
        std::scoped_lock lock(m_mutex);
        m_projectRoot = normalizedRoot;
        m_manifestHash = database.ComputeManifestHash();
        m_entries = std::move(entries);
        m_pathToId = std::move(paths);
        m_jobs = {};
        m_completions = {};
        m_stopRequested = false;
        m_activeIoJobs = 0;
        m_sequence = 0;
        m_touchSerial = 0;
        m_nextBindingUpdateSequence = 1;
        m_statistics = {};
        m_statistics.assetCount = m_entries.size();
        m_statistics.residentBudgetBytes =
            m_configuration.residentBudgetBytes;
    }
    m_worker = std::thread(
        &AssetStreamingManager::WorkerMain,
        this);
    return true;
}

bool AssetStreamingManager::Request(
    const std::string_view assetIdOrPath,
    const int priority,
    const bool pin,
    std::string* outError)
{
    std::scoped_lock lock(m_mutex);
    Entry* entry = FindEntryLocked(assetIdOrPath);
    if (entry == nullptr)
    {
        if (outError != nullptr)
        {
            *outError =
                "The requested Asset ID or path is not in the streaming manifest.";
        }
        return false;
    }
    std::vector<std::string> dependencyStack;
    return RequestLocked(
        entry->record.assetId,
        priority,
        pin,
        dependencyStack,
        outError);
}

bool AssetStreamingManager::RequestLocked(
    const std::string& assetId,
    const int priority,
    const bool pin,
    std::vector<std::string>& dependencyStack,
    std::string* outError)
{
    if (std::ranges::find(dependencyStack, assetId)
        != dependencyStack.end())
    {
        if (outError != nullptr)
        {
            *outError =
                "The Asset Manifest contains a dependency cycle.";
        }
        return false;
    }
    const auto iterator = m_entries.find(assetId);
    if (iterator == m_entries.end())
    {
        if (outError != nullptr)
        {
            *outError =
                "An Asset dependency is missing from the streaming manifest: "
                + assetId;
        }
        return false;
    }

    Entry& entry = iterator->second;
    ++entry.referenceCount;
    entry.priority = std::max(entry.priority, priority);
    if (pin)
    {
        ++entry.pinReferenceCount;
    }
    entry.lastTouchedSerial = ++m_touchSerial;

    dependencyStack.push_back(assetId);
    for (const std::string& dependency :
         entry.record.dependencies)
    {
        if (!RequestLocked(
                dependency,
                priority + 1,
                pin,
                dependencyStack,
                outError))
        {
            dependencyStack.pop_back();
            return false;
        }
    }
    dependencyStack.pop_back();

    if (entry.state == AssetResidencyState::Unloaded
        || entry.state == AssetResidencyState::Evicted
        || entry.state == AssetResidencyState::Failed)
    {
        entry.errorCode.clear();
        entry.errorMessage.clear();
        entry.uploadTicket = {};
        ++entry.generation;
        if (entry.record.type == AssetType::Scene)
        {
            entry.state =
                AssetResidencyState::WaitingForDependencies;
        }
        else
        {
            entry.state = AssetResidencyState::Queued;
            m_jobs.push({
                entry.record.assetId,
                entry.record.type,
                entry.cookedPath,
                entry.priority,
                entry.generation,
                ++m_sequence});
            m_workAvailable.notify_one();
        }
    }
    return true;
}

bool AssetStreamingManager::Release(
    const std::string_view assetIdOrPath,
    const bool unpin)
{
    std::scoped_lock lock(m_mutex);
    const Entry* entry = FindEntryLocked(assetIdOrPath);
    if (entry == nullptr)
    {
        return false;
    }
    std::vector<std::string> dependencyStack;
    ReleaseLocked(
        entry->record.assetId,
        unpin,
        dependencyStack);
    return true;
}

bool AssetStreamingManager::Cancel(
    const std::string_view assetIdOrPath,
    const bool unpin)
{
    std::scoped_lock lock(m_mutex);
    const Entry* entry = FindEntryLocked(assetIdOrPath);
    if (entry == nullptr)
    {
        return false;
    }
    std::vector<std::string> dependencyStack;
    CancelLocked(
        entry->record.assetId,
        unpin,
        dependencyStack);
    return true;
}

void AssetStreamingManager::ReleaseLocked(
    const std::string& assetId,
    const bool unpin,
    std::vector<std::string>& dependencyStack)
{
    if (std::ranges::find(dependencyStack, assetId)
        != dependencyStack.end())
    {
        return;
    }
    const auto iterator = m_entries.find(assetId);
    if (iterator == m_entries.end())
    {
        return;
    }

    Entry& entry = iterator->second;
    const bool releasedReference =
        entry.referenceCount > 0;
    if (releasedReference)
    {
        --entry.referenceCount;
    }
    if (unpin && entry.pinReferenceCount > 0)
    {
        --entry.pinReferenceCount;
    }
    if (!releasedReference && !unpin)
    {
        return;
    }

    dependencyStack.push_back(assetId);
    for (const std::string& dependency :
         entry.record.dependencies)
    {
        ReleaseLocked(
            dependency,
            unpin,
            dependencyStack);
    }
    dependencyStack.pop_back();
}

void AssetStreamingManager::CancelLocked(
    const std::string& assetId,
    const bool unpin,
    std::vector<std::string>& dependencyStack)
{
    if (std::ranges::find(dependencyStack, assetId)
        != dependencyStack.end())
    {
        return;
    }
    const auto iterator = m_entries.find(assetId);
    if (iterator == m_entries.end())
    {
        return;
    }

    Entry& entry = iterator->second;
    const bool releasedReference = entry.referenceCount > 0;
    if (releasedReference)
    {
        --entry.referenceCount;
    }
    if (unpin && entry.pinReferenceCount > 0)
    {
        --entry.pinReferenceCount;
    }
    if (!releasedReference && !unpin)
    {
        return;
    }

    dependencyStack.push_back(assetId);
    for (const std::string& dependency : entry.record.dependencies)
    {
        CancelLocked(
            dependency,
            unpin,
            dependencyStack);
    }
    dependencyStack.pop_back();

    if (entry.referenceCount != 0
        || entry.pinReferenceCount != 0)
    {
        return;
    }
    switch (entry.state)
    {
    case AssetResidencyState::Queued:
    case AssetResidencyState::LoadingIo:
    case AssetResidencyState::WaitingForDependencies:
    case AssetResidencyState::ReadyForUpload:
    case AssetResidencyState::Failed:
        // A queued/active job may still finish its file read. Advancing the
        // generation makes that completion stale, so it cannot publish a
        // payload or reach the render-preparation upload list.
        ++entry.generation;
        entry.payload = std::monostate{};
        entry.residentBytes = 0;
        entry.uploadTicket = {};
        entry.errorCode.clear();
        entry.errorMessage.clear();
        entry.state = AssetResidencyState::Unloaded;
        break;
    case AssetResidencyState::Unloaded:
    case AssetResidencyState::Uploading:
    case AssetResidencyState::Resident:
    case AssetResidencyState::Evicted:
        break;
    }
}

bool AssetStreamingManager::Touch(
    const std::string_view assetIdOrPath)
{
    std::scoped_lock lock(m_mutex);
    Entry* entry = FindEntryLocked(assetIdOrPath);
    if (entry == nullptr)
    {
        return false;
    }
    entry->lastTouchedSerial = ++m_touchSerial;
    return true;
}

std::size_t AssetStreamingManager::PumpIoCompletions()
{
    std::scoped_lock lock(m_mutex);
    std::size_t completedCount = 0;
    while (!m_completions.empty())
    {
        IoCompletion completion =
            std::move(m_completions.front());
        m_completions.pop();
        const auto iterator =
            m_entries.find(completion.assetId);
        if (iterator == m_entries.end()
            || iterator->second.generation
                != completion.generation)
        {
            continue;
        }
        Entry& entry = iterator->second;
        if (!completion.errorCode.empty())
        {
            entry.state = AssetResidencyState::Failed;
            entry.errorCode =
                std::move(completion.errorCode);
            entry.errorMessage =
                std::move(completion.errorMessage);
        }
        else
        {
            entry.payload = std::move(completion.payload);
            entry.residentBytes =
                EstimatePayloadBytes(entry.payload);
            entry.state =
                entry.record.type == AssetType::Material
                && !DependenciesResidentLocked(entry)
                ? AssetResidencyState::WaitingForDependencies
                : AssetResidencyState::ReadyForUpload;
            m_statistics.ioBytesRead +=
                completion.bytesRead;
            ++m_statistics.completedIoCount;
        }
        ++completedCount;
    }
    return completedCount;
}

std::size_t AssetStreamingManager::TickUploads(
    RHI::IGraphicsDevice& device,
    AssetRegistry& registry)
{
    AssetStreamingRenderWork work = TickUploadsStaged(device);
    ApplyBindingUpdates(registry, work.bindingUpdates);
    return work.uploadedCount;
}

AssetStreamingRenderWork
AssetStreamingManager::TickUploadsStaged(
    RHI::IGraphicsDevice& device)
{
    PumpIoCompletions();
    std::scoped_lock lock(m_mutex);
    AssetStreamingRenderWork work{};

    for (auto& [assetId, entry] : m_entries)
    {
        (void)assetId;
        if (entry.state == AssetResidencyState::Uploading
            && device.IsUploadComplete(
                entry.uploadTicket))
        {
            entry.state = AssetResidencyState::Resident;
            entry.uploadTicket = {};
            m_statistics.residentBytes +=
                entry.residentBytes;
            ++m_statistics.completedUploadCount;
            AppendBindingUpdate(
                entry,
                AssetStreamingBindingUpdateKind::Publish,
                work.bindingUpdates);
        }
        else if (entry.state
                     == AssetResidencyState::WaitingForDependencies
                 && DependenciesResidentLocked(entry))
        {
            entry.state = entry.record.type
                    == AssetType::Scene
                ? AssetResidencyState::Resident
                : AssetResidencyState::ReadyForUpload;
        }
    }

    std::vector<Entry*> ready;
    for (auto& [assetId, entry] : m_entries)
    {
        (void)assetId;
        if (entry.state == AssetResidencyState::ReadyForUpload)
        {
            ready.push_back(&entry);
        }
    }
    std::ranges::sort(
        ready,
        [](const Entry* left, const Entry* right)
        {
            return left->priority > right->priority;
        });

    std::size_t uploadedCount = 0;
    for (Entry* entry : ready)
    {
        if (uploadedCount
            >= m_configuration.maxUploadsPerTick)
        {
            break;
        }
        std::string error;
        const RHI::UploadQueueStatistics before =
            device.GetUploadQueueStatistics();
        if (!UploadEntry(*entry, device, error))
        {
            entry->state = AssetResidencyState::Failed;
            entry->uploadTicket = {};
            entry->errorCode = "asset_upload_failed";
            entry->errorMessage = std::move(error);
            continue;
        }
        const RHI::UploadQueueStatistics after =
            device.GetUploadQueueStatistics();
        if (after.pendingOperationCount > before.pendingOperationCount)
        {
            entry->uploadTicket =
                device.GetPendingUploadTicket();
            if (!entry->uploadTicket.IsValid())
            {
                entry->state =
                    AssetResidencyState::Failed;
                entry->errorCode =
                    "upload_ticket_missing";
                entry->errorMessage =
                    "The RHI queued upload work without issuing an Upload Ticket.";
                continue;
            }
            entry->state = AssetResidencyState::Uploading;
        }
        else
        {
            entry->state = AssetResidencyState::Resident;
            m_statistics.residentBytes +=
                entry->residentBytes;
            ++m_statistics.completedUploadCount;
            AppendBindingUpdate(
                *entry,
                AssetStreamingBindingUpdateKind::Publish,
                work.bindingUpdates);
        }
        ++uploadedCount;
    }
    work.uploadedCount = uploadedCount;
    return work;
}

std::size_t AssetStreamingManager::EvictToBudget(
    AssetRegistry& registry)
{
    AssetStreamingRenderWork work = EvictToBudgetStaged();
    ApplyBindingUpdates(registry, work.bindingUpdates);
    return work.evictedCount;
}

AssetStreamingRenderWork
AssetStreamingManager::EvictToBudgetStaged()
{
    std::scoped_lock lock(m_mutex);
    AssetStreamingRenderWork work{};
    if (m_statistics.residentBytes
        <= m_configuration.residentBudgetBytes)
    {
        return work;
    }

    std::vector<Entry*> candidates;
    for (auto& [assetId, entry] : m_entries)
    {
        (void)assetId;
        if (entry.state == AssetResidencyState::Resident
            && entry.referenceCount == 0
            && entry.pinReferenceCount == 0
            && entry.record.type != AssetType::Scene)
        {
            candidates.push_back(&entry);
        }
    }
    std::ranges::sort(
        candidates,
        [](const Entry* left, const Entry* right)
        {
            if (left->record.type != right->record.type)
            {
                return left->record.type
                    == AssetType::Material;
            }
            return left->lastTouchedSerial
                < right->lastTouchedSerial;
        });

    std::size_t evictedCount = 0;
    for (Entry* entry : candidates)
    {
        if (m_statistics.residentBytes
            <= m_configuration.residentBudgetBytes)
        {
            break;
        }
        if (EvictEntry(*entry))
        {
            AppendBindingUpdate(
                *entry,
                AssetStreamingBindingUpdateKind::Clear,
                work.bindingUpdates);
            m_statistics.residentBytes -=
                std::min(
                    m_statistics.residentBytes,
                    entry->residentBytes);
            entry->payload = std::monostate{};
            entry->state = AssetResidencyState::Evicted;
            ++m_statistics.evictionCount;
            ++evictedCount;
        }
    }
    work.evictedCount = evictedCount;
    return work;
}

AssetStreamingRenderWork AssetStreamingManager::ProcessRenderWork(
    RHI::IGraphicsDevice& device)
{
    AssetStreamingRenderWork result = TickUploadsStaged(device);
    AssetStreamingRenderWork eviction = EvictToBudgetStaged();
    result.evictedCount = eviction.evictedCount;
    result.bindingUpdates.insert(
        result.bindingUpdates.end(),
        std::make_move_iterator(eviction.bindingUpdates.begin()),
        std::make_move_iterator(eviction.bindingUpdates.end()));
    return result;
}

void AssetStreamingManager::ApplyBindingUpdates(
    AssetRegistry& registry,
    const std::vector<AssetStreamingBindingUpdate>& updates)
{
    std::uint64_t previousSequence = 0;
    for (const AssetStreamingBindingUpdate& update : updates)
    {
        if (update.sequence == 0
            || (previousSequence != 0
                && update.sequence <= previousSequence))
        {
            throw std::invalid_argument(
                "Streaming binding updates must be ordered and non-zero.");
        }
        previousSequence = update.sequence;

        if (update.type == AssetType::Mesh)
        {
            MeshHandle handle = registry.FindMeshByPath(update.assetPath);
            if (!handle.IsValid() && update.meshAsset != nullptr)
            {
                handle = registry.RegisterMeshAsset(
                    update.assetPath,
                    update.meshAsset);
            }
            registry.SetRuntimeMesh(
                handle,
                update.kind == AssetStreamingBindingUpdateKind::Publish
                    ? update.mesh
                    : nullptr);
        }
        else if (update.type == AssetType::Texture)
        {
            TextureHandle handle =
                registry.FindTextureByPath(update.assetPath);
            if (!handle.IsValid() && update.textureAsset != nullptr)
            {
                handle = registry.RegisterTextureAsset(
                    update.assetPath,
                    update.textureAsset);
            }
            registry.SetRuntimeTexture(
                handle,
                update.kind == AssetStreamingBindingUpdateKind::Publish
                    ? update.texture
                    : nullptr);
        }
        else if (update.type == AssetType::Material)
        {
            MaterialHandle handle =
                registry.FindMaterialByPath(update.assetPath);
            if (!handle.IsValid() && update.materialAsset != nullptr)
            {
                handle = registry.RegisterMaterialAsset(
                    update.assetPath,
                    update.materialAsset);
            }
            if (update.kind == AssetStreamingBindingUpdateKind::Publish
                && update.materialAsset != nullptr)
            {
                std::array<TextureHandle, 5> textureHandles{};
                for (std::size_t index = 0;
                     index < textureHandles.size();
                     ++index)
                {
                    if (!update.materialTexturePaths[index].empty())
                    {
                        textureHandles[index] =
                            registry.FindTextureByPath(
                                update.materialTexturePaths[index]);
                    }
                }
                update.materialAsset->SetBaseColorTexture(textureHandles[0]);
                update.materialAsset->SetMetallicRoughnessTexture(textureHandles[1]);
                update.materialAsset->SetNormalTexture(textureHandles[2]);
                update.materialAsset->SetOcclusionTexture(textureHandles[3]);
                update.materialAsset->SetEmissiveTexture(textureHandles[4]);
            }
            registry.SetRuntimeMaterial(
                handle,
                update.kind == AssetStreamingBindingUpdateKind::Publish
                    ? update.material
                    : nullptr);
        }
    }
}

bool AssetStreamingManager::WaitForIo(
    const std::chrono::milliseconds timeout)
{
    std::unique_lock lock(m_mutex);
    const bool idle = m_ioIdle.wait_for(
        lock,
        timeout,
        [&]()
        {
            return m_jobs.empty()
                && m_activeIoJobs == 0;
        });
    lock.unlock();
    PumpIoCompletions();
    return idle;
}

AssetStreamingStatistics
AssetStreamingManager::GetStatistics() const
{
    std::scoped_lock lock(m_mutex);
    AssetStreamingStatistics statistics = m_statistics;
    for (const auto& [assetId, entry] : m_entries)
    {
        (void)assetId;
        switch (entry.state)
        {
        case AssetResidencyState::Queued:
            ++statistics.queuedCount;
            break;
        case AssetResidencyState::LoadingIo:
            ++statistics.loadingCount;
            break;
        case AssetResidencyState::WaitingForDependencies:
        case AssetResidencyState::ReadyForUpload:
            ++statistics.readyCount;
            break;
        case AssetResidencyState::Uploading:
            ++statistics.uploadingCount;
            break;
        case AssetResidencyState::Resident:
            ++statistics.residentCount;
            break;
        case AssetResidencyState::Evicted:
            ++statistics.evictedCount;
            break;
        case AssetResidencyState::Failed:
            ++statistics.failedCount;
            break;
        default:
            break;
        }
    }
    return statistics;
}

std::vector<AssetStreamingEntrySnapshot>
AssetStreamingManager::GetEntries() const
{
    std::scoped_lock lock(m_mutex);
    std::vector<AssetStreamingEntrySnapshot> snapshots;
    snapshots.reserve(m_entries.size());
    for (const auto& [assetId, entry] : m_entries)
    {
        (void)assetId;
        snapshots.push_back(Snapshot(entry));
    }
    std::ranges::sort(
        snapshots,
        [](const auto& left, const auto& right)
        {
            return left.assetPath < right.assetPath;
        });
    return snapshots;
}

std::optional<AssetStreamingEntrySnapshot>
AssetStreamingManager::Find(
    const std::string_view assetIdOrPath) const
{
    std::scoped_lock lock(m_mutex);
    const Entry* entry = FindEntryLocked(assetIdOrPath);
    return entry != nullptr
        ? std::optional(Snapshot(*entry))
        : std::nullopt;
}

bool AssetStreamingManager::GetResidentSceneInstances(
    const std::string_view assetIdOrPath,
    std::vector<AssetStreamingSceneInstance>&
        outInstances,
    std::string* outError) const
{
    outInstances.clear();
    std::scoped_lock lock(m_mutex);
    const Entry* scene = FindEntryLocked(assetIdOrPath);
    if (scene == nullptr
        || scene->record.type != AssetType::Scene)
    {
        if (outError != nullptr)
        {
            *outError =
                "The requested streaming asset is not a Scene.";
        }
        return false;
    }
    if (scene->state != AssetResidencyState::Resident)
    {
        return false;
    }

    const nlohmann::json instances =
        scene->record.metadata.value(
            "instances",
            nlohmann::json::array());
    if (!instances.is_array()
        || instances.empty())
    {
        if (outError != nullptr)
        {
            *outError =
                "The resident Scene does not contain streaming instance metadata. Reimport the source asset.";
        }
        return false;
    }

    try
    {
        outInstances.reserve(instances.size());
        for (const nlohmann::json& serialized :
             instances)
        {
            AssetStreamingSceneInstance instance{};
            instance.name =
                serialized.value(
                    "name",
                    std::string("StreamedObject"));
            instance.meshAssetId =
                serialized.at("meshAssetId")
                    .get<std::string>();
            instance.materialAssetId =
                serialized.at("materialAssetId")
                    .get<std::string>();
            const auto mesh =
                m_entries.find(instance.meshAssetId);
            const auto material =
                m_entries.find(instance.materialAssetId);
            if (mesh == m_entries.end()
                || material == m_entries.end()
                || mesh->second.state
                    != AssetResidencyState::Resident
                || material->second.state
                    != AssetResidencyState::Resident)
            {
                outInstances.clear();
                return false;
            }
            instance.meshAssetPath =
                mesh->second.record.assetPath;
            instance.materialAssetPath =
                material->second.record.assetPath;
            const std::vector<float> matrix =
                serialized.at("worldMatrix")
                    .get<std::vector<float>>();
            if (matrix.size()
                != instance.worldMatrix.size())
            {
                throw std::runtime_error(
                    "A streamed Scene instance world matrix must contain 16 floats.");
            }
            std::ranges::copy(
                matrix,
                instance.worldMatrix.begin());
            outInstances.push_back(
                std::move(instance));
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        outInstances.clear();
        if (outError != nullptr)
        {
            *outError = exception.what();
        }
        return false;
    }
}

bool AssetStreamingManager::WriteReport(
    const std::filesystem::path& path,
    const RHI::IGraphicsDevice& device,
    std::string* outError) const
{
    if (path.empty())
    {
        if (outError != nullptr)
        {
            *outError =
                "Asset streaming report path is empty.";
        }
        return false;
    }

    try
    {
        const std::vector<AssetStreamingEntrySnapshot>
            entries = GetEntries();
        const AssetStreamingStatistics statistics =
            GetStatistics();
        const RHI::UploadQueueStatistics uploadStatistics =
            device.GetUploadQueueStatistics();

        nlohmann::json serializedEntries =
            nlohmann::json::array();
        for (const AssetStreamingEntrySnapshot& entry :
             entries)
        {
            serializedEntries.push_back({
                {"assetId", entry.assetId},
                {"assetPath", entry.assetPath},
                {"type",
                 AssetDatabase::ToString(entry.type)},
                {"state", ToString(entry.state)},
                {"priority", entry.priority},
                {"referenceCount", entry.referenceCount},
                {"pinned", entry.pinned},
                {"residentBytes", entry.residentBytes},
                {"lastTouchedSerial",
                 entry.lastTouchedSerial},
                {"uploadTicket", entry.uploadTicket},
                {"errorCode", entry.errorCode},
                {"errorMessage", entry.errorMessage}});
        }

        const nlohmann::json report = {
            {"format", "PrismAssetStreamingReport"},
            {"version", 1},
            {"statistics", {
                {"assetCount", statistics.assetCount},
                {"queuedCount", statistics.queuedCount},
                {"loadingCount", statistics.loadingCount},
                {"readyCount", statistics.readyCount},
                {"uploadingCount",
                 statistics.uploadingCount},
                {"residentCount",
                 statistics.residentCount},
                {"evictedCount", statistics.evictedCount},
                {"failedCount", statistics.failedCount},
                {"residentBytes",
                 statistics.residentBytes},
                {"residentBudgetBytes",
                 statistics.residentBudgetBytes},
                {"ioBytesRead",
                 statistics.ioBytesRead},
                {"completedIoCount",
                 statistics.completedIoCount},
                {"completedUploadCount",
                 statistics.completedUploadCount},
                {"evictionCount",
                 statistics.evictionCount}}},
            {"uploadQueue", {
                {"pendingOperationCount",
                 uploadStatistics.pendingOperationCount},
                {"pendingBytes",
                 uploadStatistics.pendingBytes},
                {"submittedBatchCount",
                 uploadStatistics.submittedBatchCount},
                {"uploadedBytes",
                 uploadStatistics.uploadedBytes},
                {"synchronousFlushCount",
                 uploadStatistics.synchronousFlushCount},
                {"maximumBatchBytes",
                 uploadStatistics.maximumBatchBytes},
                {"pendingTicket",
                 uploadStatistics.pendingTicket},
                {"lastSubmittedTicket",
                 uploadStatistics.lastSubmittedTicket},
                {"completedTicket",
                 uploadStatistics.completedTicket},
                {"outstandingBatchCount",
                 uploadStatistics.outstandingBatchCount},
                {"stagingPageCount",
                 uploadStatistics.stagingPageCount},
                {"stagingCapacityBytes",
                 uploadStatistics.stagingCapacityBytes},
                {"stagingHighWatermarkBytes",
                 uploadStatistics.stagingHighWatermarkBytes}}},
            {"entries", std::move(serializedEntries)}};

        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(
                path.parent_path());
        }
        std::ofstream stream(
            path,
            std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            throw std::runtime_error(
                "Could not open report file.");
        }
        stream << report.dump(2) << '\n';
        if (!stream)
        {
            throw std::runtime_error(
                "Could not write report file.");
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr)
        {
            *outError = exception.what();
        }
        return false;
    }
}

std::string_view AssetStreamingManager::ToString(
    const AssetResidencyState state)
{
    switch (state)
    {
    case AssetResidencyState::Unloaded: return "unloaded";
    case AssetResidencyState::Queued: return "queued";
    case AssetResidencyState::LoadingIo: return "loading_io";
    case AssetResidencyState::WaitingForDependencies:
        return "waiting_for_dependencies";
    case AssetResidencyState::ReadyForUpload:
        return "ready_for_upload";
    case AssetResidencyState::Uploading: return "uploading";
    case AssetResidencyState::Resident: return "resident";
    case AssetResidencyState::Evicted: return "evicted";
    case AssetResidencyState::Failed: return "failed";
    }
    return "unknown";
}

bool AssetStreamingManager::DependenciesResidentLocked(
    const Entry& entry) const
{
    return std::ranges::all_of(
        entry.record.dependencies,
        [&](const std::string& dependencyId)
        {
            const auto iterator =
                m_entries.find(dependencyId);
            return iterator != m_entries.end()
                && iterator->second.state
                    == AssetResidencyState::Resident;
        });
}

AssetStreamingManager::Entry*
AssetStreamingManager::FindEntryLocked(
    const std::string_view assetIdOrPath)
{
    if (const auto iterator =
            m_entries.find(std::string(assetIdOrPath));
        iterator != m_entries.end())
    {
        return &iterator->second;
    }
    const auto pathIterator =
        m_pathToId.find(std::string(assetIdOrPath));
    if (pathIterator == m_pathToId.end())
    {
        return nullptr;
    }
    return &m_entries.at(pathIterator->second);
}

const AssetStreamingManager::Entry*
AssetStreamingManager::FindEntryLocked(
    const std::string_view assetIdOrPath) const
{
    if (const auto iterator =
            m_entries.find(std::string(assetIdOrPath));
        iterator != m_entries.end())
    {
        return &iterator->second;
    }
    const auto pathIterator =
        m_pathToId.find(std::string(assetIdOrPath));
    if (pathIterator == m_pathToId.end())
    {
        return nullptr;
    }
    return &m_entries.at(pathIterator->second);
}

void AssetStreamingManager::WorkerMain()
{
    for (;;)
    {
        IoJob job;
        {
            std::unique_lock lock(m_mutex);
            m_workAvailable.wait(
                lock,
                [&]()
                {
                    return m_stopRequested
                        || !m_jobs.empty();
                });
            if (m_stopRequested && m_jobs.empty())
            {
                return;
            }
            job = m_jobs.top();
            m_jobs.pop();
            ++m_activeIoJobs;
            if (auto iterator = m_entries.find(job.assetId);
                iterator != m_entries.end()
                && iterator->second.generation == job.generation)
            {
                iterator->second.state =
                    AssetResidencyState::LoadingIo;
            }
        }

        Core::CpuTraceSpan ioSpan(
            "AssetStreamingIo",
            "asset");
        IoCompletion completion{};
        completion.assetId = job.assetId;
        completion.generation = job.generation;
        std::string error;
        if (!std::filesystem::is_regular_file(job.path))
        {
            completion.errorCode = "cooked_asset_missing";
            completion.errorMessage =
                "The requested Cooked Asset is missing: "
                + job.path.generic_string();
        }
        else
        {
            completion.bytesRead =
                std::filesystem::file_size(job.path);
            bool success = false;
            if (job.type == AssetType::Mesh)
            {
                std::shared_ptr<MeshAsset> mesh;
                success = CookedAssetIO::ReadMesh(
                    job.path,
                    mesh,
                    &error);
                completion.payload = std::move(mesh);
            }
            else if (job.type == AssetType::Texture)
            {
                std::shared_ptr<TextureAsset> texture;
                success = CookedAssetIO::ReadTexture(
                    job.path,
                    texture,
                    &error);
                completion.payload = std::move(texture);
            }
            else if (job.type == AssetType::Material)
            {
                CookedMaterialData material;
                success = CookedAssetIO::ReadMaterial(
                    job.path,
                    material,
                    &error);
                completion.payload = std::move(material);
            }
            if (!success)
            {
                completion.errorCode =
                    "cooked_asset_read_failed";
                completion.errorMessage = std::move(error);
            }
        }

        {
            std::scoped_lock lock(m_mutex);
            m_completions.push(std::move(completion));
            --m_activeIoJobs;
            if (m_jobs.empty() && m_activeIoJobs == 0)
            {
                m_ioIdle.notify_all();
            }
        }
    }
}

void AssetStreamingManager::StopWorker()
{
    {
        std::scoped_lock lock(m_mutex);
        m_stopRequested = true;
        while (!m_jobs.empty())
        {
            m_jobs.pop();
        }
    }
    m_workAvailable.notify_all();
    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

bool AssetStreamingManager::UploadEntry(
    Entry& entry,
    RHI::IGraphicsDevice& device,
    std::string& error)
{
    try
    {
        if (entry.record.type == AssetType::Mesh)
        {
            const auto& asset =
                std::get<std::shared_ptr<MeshAsset>>(
                    entry.payload);
            entry.runtimePayload =
                Mesh::CreateFromAsset(device, *asset);
            return true;
        }
        if (entry.record.type == AssetType::Texture)
        {
            const auto& asset =
                std::get<std::shared_ptr<TextureAsset>>(
                    entry.payload);
            auto texture = std::make_shared<Texture>();
            if (asset->HasImageData())
            {
                texture->InitializeRgba8(
                    device,
                    asset->GetWidth(),
                    asset->GetHeight(),
                    asset->GetImageData().data());
            }
            else
            {
                texture->InitializeSolidColor(
                    device,
                    asset->GetSolidColor());
            }
            entry.runtimePayload = std::move(texture);
            return true;
        }
        if (entry.record.type == AssetType::Material)
        {
            CookedMaterialData& data =
                std::get<CookedMaterialData>(entry.payload);
            std::array<std::shared_ptr<Texture>, 5> textures{};
            for (std::size_t index = 0;
                 index < data.textureAssetIds.size();
                 ++index)
            {
                const auto dependency =
                    m_entries.find(
                        data.textureAssetIds[index]);
                if (dependency == m_entries.end())
                {
                    error =
                        "A Material Texture dependency is missing.";
                    return false;
                }
                const auto* texture =
                    std::get_if<std::shared_ptr<Texture>>(
                        &dependency->second.runtimePayload);
                textures[index] = texture != nullptr
                    ? *texture
                    : nullptr;
                if (textures[index] == nullptr)
                {
                    error =
                        "A Material Texture dependency is not resident.";
                    return false;
                }
            }
            auto material = std::make_shared<Material>();
            material->Initialize(
                CreateMaterialParameters(*data.material),
                textures[0],
                textures[1],
                textures[2],
                textures[3],
                textures[4]);
            entry.runtimePayload = std::move(material);
            return true;
        }
        return entry.record.type == AssetType::Scene;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool AssetStreamingManager::EvictEntry(Entry& entry)
{
    if (entry.record.type == AssetType::Material)
    {
        const auto* runtime =
            std::get_if<std::shared_ptr<Material>>(
                &entry.runtimePayload);
        if (runtime != nullptr && *runtime != nullptr
            && runtime->use_count() > 2)
        {
            return false;
        }
        return true;
    }
    if (entry.record.type == AssetType::Mesh)
    {
        const auto* runtime = std::get_if<std::shared_ptr<Mesh>>(
            &entry.runtimePayload);
        if (runtime != nullptr && *runtime != nullptr
            && runtime->use_count() > 2)
        {
            return false;
        }
        return true;
    }
    if (entry.record.type == AssetType::Texture)
    {
        const auto* runtime =
            std::get_if<std::shared_ptr<Texture>>(
                &entry.runtimePayload);
        if (runtime != nullptr && *runtime != nullptr
            && runtime->use_count() > 2)
        {
            return false;
        }
        return true;
    }
    return false;
}

void AssetStreamingManager::AppendBindingUpdate(
    Entry& entry,
    const AssetStreamingBindingUpdateKind kind,
    std::vector<AssetStreamingBindingUpdate>& updates)
{
    if (entry.record.type == AssetType::Scene)
    {
        return;
    }
    if (m_nextBindingUpdateSequence
        == std::numeric_limits<std::uint64_t>::max())
    {
        throw std::overflow_error(
            "Streaming binding update sequence capacity exhausted.");
    }

    AssetStreamingBindingUpdate update{};
    update.sequence = m_nextBindingUpdateSequence++;
    update.generation = entry.generation;
    update.kind = kind;
    update.type = entry.record.type;
    update.assetPath = entry.record.assetPath;
    if (entry.record.type == AssetType::Mesh)
    {
        update.meshAsset =
            std::get<std::shared_ptr<MeshAsset>>(entry.payload);
        if (const auto* runtime =
                std::get_if<std::shared_ptr<Mesh>>(
                    &entry.runtimePayload))
        {
            update.mesh = *runtime;
        }
    }
    else if (entry.record.type == AssetType::Texture)
    {
        update.textureAsset =
            std::get<std::shared_ptr<TextureAsset>>(entry.payload);
        if (const auto* runtime =
                std::get_if<std::shared_ptr<Texture>>(
                    &entry.runtimePayload))
        {
            update.texture = *runtime;
        }
    }
    else if (entry.record.type == AssetType::Material)
    {
        const CookedMaterialData& materialData =
            std::get<CookedMaterialData>(entry.payload);
        update.materialAsset = materialData.material;
        for (std::size_t index = 0;
             index < materialData.textureAssetIds.size();
             ++index)
        {
            const auto dependency = m_entries.find(
                materialData.textureAssetIds[index]);
            if (dependency != m_entries.end())
            {
                update.materialTexturePaths[index] =
                    dependency->second.record.assetPath;
            }
        }
        if (const auto* runtime =
                std::get_if<std::shared_ptr<Material>>(
                    &entry.runtimePayload))
        {
            update.material = *runtime;
        }
    }
    updates.push_back(std::move(update));
    if (kind == AssetStreamingBindingUpdateKind::Clear)
    {
        entry.runtimePayload = std::monostate{};
    }
}

AssetStreamingEntrySnapshot
AssetStreamingManager::Snapshot(const Entry& entry) const
{
    return {
        entry.record.assetId,
        entry.record.assetPath,
        entry.record.type,
        entry.state,
        entry.priority,
        entry.referenceCount,
        entry.pinReferenceCount > 0,
        entry.residentBytes,
        entry.lastTouchedSerial,
        entry.uploadTicket.value,
        entry.errorCode,
        entry.errorMessage};
}
} // namespace Prism::Asset
