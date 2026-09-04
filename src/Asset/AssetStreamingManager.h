#pragma once

#include "Asset/AssetDatabase.h"
#include "Asset/CookedAssetIO.h"
#include "RHI/DeviceCapabilities.h"

#include <chrono>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Prism::RHI
{
class IGraphicsDevice;
}

namespace Prism::Asset
{
class AssetRegistry;
class Material;
class MaterialAsset;
class Mesh;
class MeshAsset;
class Texture;
class TextureAsset;

enum class AssetResidencyState
{
    Unloaded,
    Queued,
    LoadingIo,
    WaitingForDependencies,
    ReadyForUpload,
    Uploading,
    Resident,
    Evicted,
    Failed
};

struct AssetStreamingConfiguration
{
    std::uint64_t residentBudgetBytes = 512ull * 1024ull * 1024ull;
    std::uint32_t maxUploadsPerTick = 4;
};

struct AssetStreamingEntrySnapshot
{
    std::string assetId;
    std::string assetPath;
    AssetType type = AssetType::Unknown;
    AssetResidencyState state = AssetResidencyState::Unloaded;
    int priority = 0;
    std::uint32_t referenceCount = 0;
    bool pinned = false;
    std::uint64_t residentBytes = 0;
    std::uint64_t lastTouchedSerial = 0;
    std::uint64_t uploadTicket = 0;
    std::string errorCode;
    std::string errorMessage;
};

struct AssetStreamingStatistics
{
    std::size_t assetCount = 0;
    std::size_t queuedCount = 0;
    std::size_t loadingCount = 0;
    std::size_t readyCount = 0;
    std::size_t uploadingCount = 0;
    std::size_t residentCount = 0;
    std::size_t evictedCount = 0;
    std::size_t failedCount = 0;
    std::uint64_t residentBytes = 0;
    std::uint64_t residentBudgetBytes = 0;
    std::uint64_t ioBytesRead = 0;
    std::uint64_t completedIoCount = 0;
    std::uint64_t completedUploadCount = 0;
    std::uint64_t evictionCount = 0;
};

struct AssetStreamingSceneInstance
{
    std::string name;
    std::string meshAssetId;
    std::string meshAssetPath;
    std::string materialAssetId;
    std::string materialAssetPath;
    std::array<float, 16> worldMatrix{};
};

enum class AssetStreamingBindingUpdateKind : std::uint8_t
{
    Publish,
    Clear
};

// GPU resources are created on the render execution lane, but runtime binding
// publication remains a main-thread operation. Keeping the update immutable
// lets an older RenderFramePacket retain its previous resource lease while the
// next packet observes the newly published registry revision.
struct AssetStreamingBindingUpdate
{
    std::uint64_t sequence = 0;
    std::uint64_t generation = 0;
    AssetStreamingBindingUpdateKind kind =
        AssetStreamingBindingUpdateKind::Publish;
    AssetType type = AssetType::Unknown;
    std::string assetPath;
    std::shared_ptr<MeshAsset> meshAsset;
    std::shared_ptr<TextureAsset> textureAsset;
    std::shared_ptr<MaterialAsset> materialAsset;
    std::array<std::string, 5> materialTexturePaths{};
    std::shared_ptr<Mesh> mesh;
    std::shared_ptr<Texture> texture;
    std::shared_ptr<Material> material;
};

struct AssetStreamingRenderWork
{
    std::size_t uploadedCount = 0;
    std::size_t evictedCount = 0;
    std::vector<AssetStreamingBindingUpdate> bindingUpdates;
};

class AssetStreamingManager
{
public:
    explicit AssetStreamingManager(
        AssetStreamingConfiguration configuration = {});
    ~AssetStreamingManager();

    AssetStreamingManager(const AssetStreamingManager&) = delete;
    AssetStreamingManager& operator=(const AssetStreamingManager&) = delete;

    bool Initialize(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& manifestPath = {},
        std::string* outError = nullptr);
    bool Request(
        std::string_view assetIdOrPath,
        int priority = 0,
        bool pin = false,
        std::string* outError = nullptr);
    bool Release(std::string_view assetIdOrPath, bool unpin = false);
    // Releases this request and invalidates unreferenced CPU-side work that
    // has not reached GPU submission. Resident or already-uploading resources
    // retain their existing retirement semantics.
    bool Cancel(std::string_view assetIdOrPath, bool unpin = false);
    bool Touch(std::string_view assetIdOrPath);

    std::size_t PumpIoCompletions();
    std::size_t TickUploads(
        RHI::IGraphicsDevice& device,
        AssetRegistry& registry);
    std::size_t EvictToBudget(AssetRegistry& registry);
    [[nodiscard]] AssetStreamingRenderWork ProcessRenderWork(
        RHI::IGraphicsDevice& device);
    static void ApplyBindingUpdates(
        AssetRegistry& registry,
        const std::vector<AssetStreamingBindingUpdate>& updates);
    bool WaitForIo(
        std::chrono::milliseconds timeout =
            std::chrono::seconds(5));

    [[nodiscard]] AssetStreamingStatistics GetStatistics() const;
    [[nodiscard]] std::vector<AssetStreamingEntrySnapshot>
        GetEntries() const;
    [[nodiscard]] std::optional<AssetStreamingEntrySnapshot>
        Find(std::string_view assetIdOrPath) const;
    bool GetResidentSceneInstances(
        std::string_view assetIdOrPath,
        std::vector<AssetStreamingSceneInstance>&
            outInstances,
        std::string* outError = nullptr) const;
    bool WriteReport(
        const std::filesystem::path& path,
        const RHI::IGraphicsDevice& device,
        std::string* outError = nullptr) const;

    [[nodiscard]] static std::string_view ToString(
        AssetResidencyState state);

private:
    using Payload = std::variant<
        std::monostate,
        std::shared_ptr<MeshAsset>,
        std::shared_ptr<TextureAsset>,
        CookedMaterialData>;
    using RuntimePayload = std::variant<
        std::monostate,
        std::shared_ptr<Mesh>,
        std::shared_ptr<Texture>,
        std::shared_ptr<Material>>;

    struct Entry
    {
        AssetRecord record;
        std::filesystem::path cookedPath;
        AssetResidencyState state =
            AssetResidencyState::Unloaded;
        Payload payload;
        RuntimePayload runtimePayload;
        int priority = 0;
        std::uint32_t referenceCount = 0;
        std::uint32_t pinReferenceCount = 0;
        std::uint64_t generation = 0;
        std::uint64_t residentBytes = 0;
        std::uint64_t lastTouchedSerial = 0;
        RHI::UploadTicket uploadTicket;
        std::string errorCode;
        std::string errorMessage;
    };

    struct IoJob
    {
        std::string assetId;
        AssetType type = AssetType::Unknown;
        std::filesystem::path path;
        int priority = 0;
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
    };

    struct IoJobCompare
    {
        bool operator()(const IoJob& left, const IoJob& right) const;
    };

    struct IoCompletion
    {
        std::string assetId;
        std::uint64_t generation = 0;
        Payload payload;
        std::uint64_t bytesRead = 0;
        std::string errorCode;
        std::string errorMessage;
    };

    bool RequestLocked(
        const std::string& assetId,
        int priority,
        bool pin,
        std::vector<std::string>& dependencyStack,
        std::string* outError);
    void ReleaseLocked(
        const std::string& assetId,
        bool unpin,
        std::vector<std::string>& dependencyStack);
    void CancelLocked(
        const std::string& assetId,
        bool unpin,
        std::vector<std::string>& dependencyStack);
    bool DependenciesResidentLocked(const Entry& entry) const;
    Entry* FindEntryLocked(std::string_view assetIdOrPath);
    const Entry* FindEntryLocked(
        std::string_view assetIdOrPath) const;
    void WorkerMain();
    void StopWorker();
    bool UploadEntry(
        Entry& entry,
        RHI::IGraphicsDevice& device,
        std::string& error);
    bool EvictEntry(Entry& entry);
    [[nodiscard]] AssetStreamingRenderWork TickUploadsStaged(
        RHI::IGraphicsDevice& device);
    [[nodiscard]] AssetStreamingRenderWork EvictToBudgetStaged();
    void AppendBindingUpdate(
        Entry& entry,
        AssetStreamingBindingUpdateKind kind,
        std::vector<AssetStreamingBindingUpdate>& updates);
    AssetStreamingEntrySnapshot Snapshot(const Entry& entry) const;

    AssetStreamingConfiguration m_configuration;
    std::filesystem::path m_projectRoot;
    std::string m_manifestHash;
    std::unordered_map<std::string, Entry> m_entries;
    std::unordered_map<std::string, std::string> m_pathToId;
    std::priority_queue<
        IoJob,
        std::vector<IoJob>,
        IoJobCompare> m_jobs;
    std::queue<IoCompletion> m_completions;
    mutable std::mutex m_mutex;
    std::condition_variable m_workAvailable;
    std::condition_variable m_ioIdle;
    std::thread m_worker;
    bool m_stopRequested = false;
    std::size_t m_activeIoJobs = 0;
    std::uint64_t m_sequence = 0;
    std::uint64_t m_touchSerial = 0;
    std::uint64_t m_nextBindingUpdateSequence = 1;
    AssetStreamingStatistics m_statistics;
};
} // namespace Prism::Asset
