#pragma once

#include "Asset/AssetDatabase.h"
#include "Asset/AssetRuntimeLoader.h"
#include "Asset/AssetStreamingManager.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Prism::Asset
{
class AssetImportService;
class AssetRegistry;
}

namespace Prism::RHI
{
class IGraphicsDevice;
}

namespace Prism::Core
{
struct AssetRuntimeRenderWorkResult
{
    std::uint64_t logicalFrameId = 0;
    std::uint64_t batchId = 0;
    std::size_t uploadedCount = 0;
    std::size_t evictedCount = 0;
    std::size_t bindingUpdateCount = 0;
};

struct AssetRuntimeBindingApplyResult
{
    std::uint64_t lastLogicalFrameId = 0;
    std::uint64_t lastBatchId = 0;
    std::size_t batchCount = 0;
    std::size_t bindingUpdateCount = 0;
};

// Owns the process-level asset runtime services. CPU streaming workers only
// produce private completions. The render lane creates/uploads resources and
// queues immutable binding feedback; the main lane applies those revisions
// before publishing a later scene frame.
class AssetRuntimeCoordinator final
{
public:
    AssetRuntimeCoordinator(
        std::filesystem::path projectRoot,
        std::filesystem::path manifestPath);
    ~AssetRuntimeCoordinator();

    AssetRuntimeCoordinator(const AssetRuntimeCoordinator&) = delete;
    AssetRuntimeCoordinator& operator=(const AssetRuntimeCoordinator&) = delete;

    [[nodiscard]] Asset::AssetRegistry& GetRegistry() noexcept;
    [[nodiscard]] const Asset::AssetRegistry& GetRegistry() const noexcept;
    [[nodiscard]] const std::filesystem::path& GetProjectRoot() const noexcept;
    [[nodiscard]] const std::filesystem::path& GetManifestPath() const noexcept;

    bool InitializeImportService(
        bool enabled,
        std::string* outError = nullptr);
    [[nodiscard]] bool IsImportAvailable() const noexcept;
    [[nodiscard]] Asset::AssetDatabase* GetImportDatabase() noexcept;
    [[nodiscard]] const Asset::AssetDatabase* GetImportDatabase() const noexcept;
    [[nodiscard]] Asset::AssetImportResult ImportAndReload(
        const std::filesystem::path& sourcePath,
        bool reimport,
        RHI::IGraphicsDevice& device,
        const std::function<void()>& waitForGpu);

    [[nodiscard]] Asset::AssetRuntimeLoadResult LoadRuntimeAssets(
        RHI::IGraphicsDevice& device);

    bool EnableStreaming(
        const Asset::AssetStreamingConfiguration& configuration,
        std::string* outError = nullptr);
    void DisableStreaming() noexcept;
    [[nodiscard]] bool IsStreamingEnabled() const noexcept;
    bool RequestStreamingAsset(
        std::string_view assetIdOrPath,
        int priority = 0,
        bool pin = false,
        std::string* outError = nullptr);
    // Cancels this consumer's ownership of the request. Already-running file
    // reads may finish privately, but unreferenced pre-submit work cannot
    // publish GPU/runtime bindings.
    bool CancelStreamingRequest(
        std::string_view assetIdOrPath,
        bool unpin = false);
    bool WaitForStreamingIo(
        std::chrono::milliseconds timeout = std::chrono::seconds(5));
    [[nodiscard]] std::size_t ProcessStreamingAtRenderPreparation(
        RHI::IGraphicsDevice& device);
    [[nodiscard]] std::size_t EvictStreamingToBudget();
    [[nodiscard]] AssetRuntimeRenderWorkResult
        ProcessStreamingRenderWork(
            RHI::IGraphicsDevice& device,
            std::uint64_t logicalFrameId);
    [[nodiscard]] AssetRuntimeBindingApplyResult
        ApplyCompletedStreamingBindings();
    [[nodiscard]] std::size_t
        GetPendingStreamingBindingBatchCount() const noexcept;
    [[nodiscard]] Asset::AssetStreamingStatistics
        GetStreamingStatistics() const;
    [[nodiscard]] std::vector<Asset::AssetStreamingEntrySnapshot>
        GetStreamingEntries() const;
    [[nodiscard]] std::optional<Asset::AssetStreamingEntrySnapshot>
        FindStreamingEntry(std::string_view assetIdOrPath) const;
    bool WriteStreamingReport(
        const std::filesystem::path& path,
        const RHI::IGraphicsDevice& device,
        std::string* outError = nullptr) const;

    // SceneSession extraction bridge. Ownership
    // remains here; callers must not store or destroy the returned service.
    [[nodiscard]] Asset::AssetStreamingManager*
        BorrowStreamingManager() noexcept;

    void Shutdown() noexcept;

private:
    struct PendingBindingBatch
    {
        std::uint64_t logicalFrameId = 0;
        std::uint64_t batchId = 0;
        std::vector<Asset::AssetStreamingBindingUpdate> updates;
    };

    static constexpr std::size_t MaximumPendingBindingBatches = 4;

    std::filesystem::path m_projectRoot;
    std::filesystem::path m_manifestPath;
    std::unique_ptr<Asset::AssetRegistry> m_registry;
    std::unique_ptr<Asset::AssetImportService> m_importService;
    std::unique_ptr<Asset::AssetStreamingManager> m_streamingManager;
    mutable std::mutex m_bindingFeedbackMutex;
    std::deque<PendingBindingBatch> m_pendingBindingBatches;
    std::uint64_t m_nextBindingBatchId = 1;
    std::uint64_t m_lastAppliedBindingSequence = 0;
};
} // namespace Prism::Core
