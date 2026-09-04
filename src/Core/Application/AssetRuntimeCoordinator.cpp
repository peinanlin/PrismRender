#include "Core/Application/AssetRuntimeCoordinator.h"

#include "Asset/AssetImportService.h"
#include "Asset/AssetRegistry.h"
#include "RHI/IGraphicsDevice.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace Prism::Core
{
AssetRuntimeCoordinator::AssetRuntimeCoordinator(
    std::filesystem::path projectRoot,
    std::filesystem::path manifestPath)
    : m_projectRoot(
          std::filesystem::absolute(std::move(projectRoot))
              .lexically_normal()),
      m_manifestPath(
          std::filesystem::absolute(
              manifestPath.is_absolute()
                  ? std::move(manifestPath)
                  : m_projectRoot / std::move(manifestPath))
              .lexically_normal()),
      m_registry(std::make_unique<Asset::AssetRegistry>())
{
}

AssetRuntimeCoordinator::~AssetRuntimeCoordinator()
{
    Shutdown();
}

Asset::AssetRegistry& AssetRuntimeCoordinator::GetRegistry() noexcept
{
    return *m_registry;
}

const Asset::AssetRegistry& AssetRuntimeCoordinator::GetRegistry() const noexcept
{
    return *m_registry;
}

const std::filesystem::path&
AssetRuntimeCoordinator::GetProjectRoot() const noexcept
{
    return m_projectRoot;
}

const std::filesystem::path&
AssetRuntimeCoordinator::GetManifestPath() const noexcept
{
    return m_manifestPath;
}

bool AssetRuntimeCoordinator::InitializeImportService(
    const bool enabled,
    std::string* const outError)
{
    m_importService.reset();
    if (!enabled)
    {
        return true;
    }

    auto service = std::make_unique<Asset::AssetImportService>(
        m_projectRoot,
        m_manifestPath);
    if (!service->Initialize(outError))
    {
        return false;
    }
    m_importService = std::move(service);
    return true;
}

bool AssetRuntimeCoordinator::IsImportAvailable() const noexcept
{
    return m_importService != nullptr;
}

Asset::AssetDatabase*
AssetRuntimeCoordinator::GetImportDatabase() noexcept
{
    return m_importService != nullptr
        ? &m_importService->GetDatabase()
        : nullptr;
}

const Asset::AssetDatabase*
AssetRuntimeCoordinator::GetImportDatabase() const noexcept
{
    return m_importService != nullptr
        ? &m_importService->GetDatabase()
        : nullptr;
}

Asset::AssetImportResult AssetRuntimeCoordinator::ImportAndReload(
    const std::filesystem::path& sourcePath,
    const bool reimport,
    RHI::IGraphicsDevice& device,
    const std::function<void()>& waitForGpu)
{
    Asset::AssetImportResult result{};
    result.reimported = reimport;
    result.sourcePath = sourcePath.generic_string();
    if (m_importService == nullptr)
    {
        result.errorCode = "asset_database_unavailable";
        result.errorMessage =
            "The editor Asset Database is unavailable.";
        return result;
    }
    if (m_streamingManager != nullptr)
    {
        result.errorCode = "asset_streaming_active";
        result.errorMessage =
            "Editor import is disabled while asynchronous Asset Streaming is active.";
        return result;
    }

    result = reimport
        ? m_importService->Reimport(sourcePath)
        : m_importService->Import(sourcePath);
    if (!result.success)
    {
        return result;
    }

    // The caller supplies the existing frame-context wait. The coordinator
    // owns the reload policy without taking ownership of the render backend.
    if (waitForGpu)
    {
        waitForGpu();
    }
    const Asset::AssetRuntimeLoadResult runtimeLoad =
        LoadRuntimeAssets(device);
    if (!runtimeLoad.success)
    {
        result.success = false;
        result.errorCode = runtimeLoad.errorCode.empty()
            ? "asset_runtime_reload_failed"
            : runtimeLoad.errorCode;
        result.errorMessage = runtimeLoad.errorMessage.empty()
            ? "The Asset Manifest was updated, but runtime assets could not be refreshed."
            : runtimeLoad.errorMessage;
    }
    return result;
}

Asset::AssetRuntimeLoadResult
AssetRuntimeCoordinator::LoadRuntimeAssets(
    RHI::IGraphicsDevice& device)
{
    return Asset::AssetRuntimeLoader::LoadManifest(
        m_projectRoot,
        m_manifestPath,
        device,
        *m_registry);
}

bool AssetRuntimeCoordinator::EnableStreaming(
    const Asset::AssetStreamingConfiguration& configuration,
    std::string* const outError)
{
    {
        std::lock_guard lock(m_bindingFeedbackMutex);
        if (!m_pendingBindingBatches.empty())
        {
            if (outError != nullptr)
            {
                *outError =
                    "Pending streaming bindings must be applied before reinitialization.";
            }
            return false;
        }
        m_nextBindingBatchId = 1;
        m_lastAppliedBindingSequence = 0;
    }
    auto manager =
        std::make_unique<Asset::AssetStreamingManager>(configuration);
    if (!manager->Initialize(
            m_projectRoot,
            m_manifestPath,
            outError))
    {
        return false;
    }
    m_streamingManager = std::move(manager);
    return true;
}

void AssetRuntimeCoordinator::DisableStreaming() noexcept
{
    // AssetStreamingManager destruction stops acceptance, wakes and joins its
    // IO worker before any registry or renderer resource can be released.
    m_streamingManager.reset();
    std::lock_guard lock(m_bindingFeedbackMutex);
    m_pendingBindingBatches.clear();
}

bool AssetRuntimeCoordinator::IsStreamingEnabled() const noexcept
{
    return m_streamingManager != nullptr;
}

bool AssetRuntimeCoordinator::RequestStreamingAsset(
    const std::string_view assetIdOrPath,
    const int priority,
    const bool pin,
    std::string* const outError)
{
    return m_streamingManager != nullptr
        && m_streamingManager->Request(
            assetIdOrPath,
            priority,
            pin,
            outError);
}

bool AssetRuntimeCoordinator::CancelStreamingRequest(
    const std::string_view assetIdOrPath,
    const bool unpin)
{
    return m_streamingManager != nullptr
        && m_streamingManager->Cancel(assetIdOrPath, unpin);
}

bool AssetRuntimeCoordinator::WaitForStreamingIo(
    const std::chrono::milliseconds timeout)
{
    return m_streamingManager != nullptr
        && m_streamingManager->WaitForIo(timeout);
}

std::size_t
AssetRuntimeCoordinator::ProcessStreamingAtRenderPreparation(
    RHI::IGraphicsDevice& device)
{
    return m_streamingManager != nullptr
        ? m_streamingManager->TickUploads(device, *m_registry)
        : 0;
}

std::size_t AssetRuntimeCoordinator::EvictStreamingToBudget()
{
    return m_streamingManager != nullptr
        ? m_streamingManager->EvictToBudget(*m_registry)
        : 0;
}

AssetRuntimeRenderWorkResult
AssetRuntimeCoordinator::ProcessStreamingRenderWork(
    RHI::IGraphicsDevice& device,
    const std::uint64_t logicalFrameId)
{
    if (logicalFrameId == 0)
    {
        throw std::invalid_argument(
            "Streaming render work requires a non-zero logical frame ID.");
    }
    if (m_streamingManager == nullptr)
    {
        return {logicalFrameId};
    }

    Asset::AssetStreamingRenderWork work =
        m_streamingManager->ProcessRenderWork(device);
    AssetRuntimeRenderWorkResult result{
        logicalFrameId,
        0,
        work.uploadedCount,
        work.evictedCount,
        work.bindingUpdates.size()};
    if (work.bindingUpdates.empty())
    {
        return result;
    }

    std::lock_guard lock(m_bindingFeedbackMutex);
    if (m_pendingBindingBatches.size()
        >= MaximumPendingBindingBatches)
    {
        throw std::runtime_error(
            "Streaming binding feedback exceeded its bounded capacity.");
    }
    if (m_nextBindingBatchId
        == std::numeric_limits<std::uint64_t>::max())
    {
        throw std::overflow_error(
            "Streaming binding batch capacity exhausted.");
    }
    result.batchId = m_nextBindingBatchId++;
    m_pendingBindingBatches.push_back({
        logicalFrameId,
        result.batchId,
        std::move(work.bindingUpdates)});
    return result;
}

AssetRuntimeBindingApplyResult
AssetRuntimeCoordinator::ApplyCompletedStreamingBindings()
{
    std::deque<PendingBindingBatch> batches;
    {
        std::lock_guard lock(m_bindingFeedbackMutex);
        batches.swap(m_pendingBindingBatches);
    }

    AssetRuntimeBindingApplyResult result{};
    for (const PendingBindingBatch& batch : batches)
    {
        for (const Asset::AssetStreamingBindingUpdate& update :
             batch.updates)
        {
            if (update.sequence <= m_lastAppliedBindingSequence)
            {
                throw std::logic_error(
                    "Streaming binding feedback arrived out of order.");
            }
            m_lastAppliedBindingSequence = update.sequence;
        }
        Asset::AssetStreamingManager::ApplyBindingUpdates(
            *m_registry,
            batch.updates);
        result.lastLogicalFrameId = batch.logicalFrameId;
        result.lastBatchId = batch.batchId;
        ++result.batchCount;
        result.bindingUpdateCount += batch.updates.size();
    }
    return result;
}

std::size_t
AssetRuntimeCoordinator::GetPendingStreamingBindingBatchCount()
    const noexcept
{
    std::lock_guard lock(m_bindingFeedbackMutex);
    return m_pendingBindingBatches.size();
}

Asset::AssetStreamingStatistics
AssetRuntimeCoordinator::GetStreamingStatistics() const
{
    return m_streamingManager != nullptr
        ? m_streamingManager->GetStatistics()
        : Asset::AssetStreamingStatistics{};
}

std::vector<Asset::AssetStreamingEntrySnapshot>
AssetRuntimeCoordinator::GetStreamingEntries() const
{
    return m_streamingManager != nullptr
        ? m_streamingManager->GetEntries()
        : std::vector<Asset::AssetStreamingEntrySnapshot>{};
}

std::optional<Asset::AssetStreamingEntrySnapshot>
AssetRuntimeCoordinator::FindStreamingEntry(
    const std::string_view assetIdOrPath) const
{
    return m_streamingManager != nullptr
        ? m_streamingManager->Find(assetIdOrPath)
        : std::nullopt;
}

bool AssetRuntimeCoordinator::WriteStreamingReport(
    const std::filesystem::path& path,
    const RHI::IGraphicsDevice& device,
    std::string* const outError) const
{
    if (m_streamingManager == nullptr)
    {
        if (outError != nullptr)
        {
            *outError = "Asset Streaming is not enabled.";
        }
        return false;
    }
    return m_streamingManager->WriteReport(
        path,
        device,
        outError);
}

Asset::AssetStreamingManager*
AssetRuntimeCoordinator::BorrowStreamingManager() noexcept
{
    return m_streamingManager.get();
}

void AssetRuntimeCoordinator::Shutdown() noexcept
{
    DisableStreaming();
    {
        std::lock_guard lock(m_bindingFeedbackMutex);
        m_pendingBindingBatches.clear();
    }
    m_importService.reset();
    m_registry.reset();
}
} // namespace Prism::Core
