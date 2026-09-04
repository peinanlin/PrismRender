#include "Core/Application/AssetRuntimeCoordinator.h"

#include "Asset/AssetRegistry.h"
#include "Asset/CookedAssetIO.h"
#include "Asset/MaterialAsset.h"
#include "Asset/TextureAsset.h"
#include "RHI/IGraphicsDevice.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>

namespace
{
void Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class TestTexture final : public Prism::RHI::ITexture
{
public:
    explicit TestTexture(Prism::RHI::TextureDescription description)
        : m_description(std::move(description))
    {
    }

    Prism::RHI::GraphicsApi GetGraphicsApi() const override
    {
        return Prism::RHI::GraphicsApi::Vulkan;
    }

    const Prism::RHI::TextureDescription&
    GetDescription() const override
    {
        return m_description;
    }

private:
    Prism::RHI::TextureDescription m_description;
};

class TestTextureView final : public Prism::RHI::ITextureView
{
public:
    TestTextureView(
        std::shared_ptr<Prism::RHI::ITexture> texture,
        Prism::RHI::TextureViewDescription description)
        : m_texture(std::move(texture)),
          m_description(std::move(description))
    {
    }

    Prism::RHI::GraphicsApi GetGraphicsApi() const override
    {
        return Prism::RHI::GraphicsApi::Vulkan;
    }

    const Prism::RHI::TextureViewDescription&
    GetDescription() const override
    {
        return m_description;
    }

    const Prism::RHI::ITexture* GetTexture() const override
    {
        return m_texture.get();
    }

private:
    std::shared_ptr<Prism::RHI::ITexture> m_texture;
    Prism::RHI::TextureViewDescription m_description;
};

class TestGraphicsDevice final : public Prism::RHI::IGraphicsDevice
{
public:
    Prism::RHI::GraphicsApi GetGraphicsApi() const override
    {
        return Prism::RHI::GraphicsApi::Vulkan;
    }

    Prism::RHI::ShaderBinaryFormat
    GetPreferredShaderBinaryFormat() const override
    {
        return Prism::RHI::ShaderBinaryFormat::SpirV;
    }

    const Prism::RHI::GraphicsDeviceCapabilities&
    GetCapabilities() const override
    {
        return m_capabilities;
    }

    Prism::RHI::DescriptorAllocatorStatistics
    GetDescriptorAllocatorStatistics() const override
    {
        return {};
    }

    Prism::RHI::UploadQueueStatistics
    GetUploadQueueStatistics() const override
    {
        return {};
    }

    Prism::RHI::UploadTicket GetPendingUploadTicket() const override
    {
        return {};
    }

    bool IsUploadComplete(Prism::RHI::UploadTicket) const override
    {
        return true;
    }

    Prism::RHI::ResourceRetirementStatistics
    GetResourceRetirementStatistics() const override
    {
        return {};
    }

    std::shared_ptr<Prism::RHI::IBuffer> CreateBuffer(
        const Prism::RHI::BufferDescription&,
        const void*) override
    {
        throw std::logic_error("The material-only fixture must not create buffers.");
    }

    std::shared_ptr<Prism::RHI::ITexture> CreateTexture(
        const Prism::RHI::TextureDescription& description,
        const Prism::RHI::TextureInitialData*) override
    {
        ++createdTextureCount;
        return std::make_shared<TestTexture>(description);
    }

    std::shared_ptr<Prism::RHI::ITransientTexturePool>
    CreateTransientTexturePool(
        const std::vector<Prism::RHI::TransientTextureRequest>&) override
    {
        return nullptr;
    }

    std::shared_ptr<Prism::RHI::ITransientBufferPool>
    CreateTransientBufferPool(
        const std::vector<Prism::RHI::TransientBufferRequest>&) override
    {
        return nullptr;
    }

    std::shared_ptr<Prism::RHI::ITextureView> CreateTextureView(
        std::shared_ptr<Prism::RHI::ITexture> texture,
        const Prism::RHI::TextureViewDescription& description) override
    {
        return std::make_shared<TestTextureView>(
            std::move(texture),
            description);
    }

    std::shared_ptr<Prism::RHI::ISampler> CreateSampler(
        const Prism::RHI::SamplerDescription&) override
    {
        return nullptr;
    }

    std::shared_ptr<Prism::RHI::IDescriptorSetLayout>
    CreateDescriptorSetLayout(
        const Prism::RHI::DescriptorSetLayoutDescription&) override
    {
        return nullptr;
    }

    std::shared_ptr<Prism::RHI::IDescriptorSet> CreateDescriptorSet(
        std::shared_ptr<Prism::RHI::IDescriptorSetLayout>) override
    {
        return nullptr;
    }

    std::shared_ptr<Prism::RHI::IGraphicsPipeline>
    CreateGraphicsPipeline(
        const Prism::RHI::GraphicsPipelineDescription&) override
    {
        return nullptr;
    }

    std::shared_ptr<Prism::RHI::IComputePipeline>
    CreateComputePipeline(
        const Prism::RHI::ComputePipelineDescription&) override
    {
        return nullptr;
    }

    Prism::RHI::AccelerationStructureBuildSizes
    QueryAccelerationStructureBuildSizes(
        const Prism::RHI::AccelerationStructureBuildDescription&) const override
    {
        return {};
    }

    std::shared_ptr<Prism::RHI::IRayTracingAccelerationStructure>
    CreateAccelerationStructure(
        const Prism::RHI::AccelerationStructureBuildRequest&) override
    {
        return nullptr;
    }

    std::size_t createdTextureCount = 0;

private:
    Prism::RHI::GraphicsDeviceCapabilities m_capabilities{};
};

struct TemporaryDirectory
{
    explicit TemporaryDirectory(std::filesystem::path value)
        : path(std::move(value))
    {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

struct StreamingFixture
{
    std::filesystem::path manifestPath;
    std::string materialId;
};

StreamingFixture CreateStreamingFixture(
    const std::filesystem::path& projectRoot)
{
    using nlohmann::json;
    const std::filesystem::path cookedDirectory =
        projectRoot / "automation/cache/cooked/test";
    std::filesystem::create_directories(cookedDirectory);

    json assets = json::array();
    std::array<std::string, 5> textureIds{};
    for (std::size_t index = 0; index < textureIds.size(); ++index)
    {
        textureIds[index] = "test-texture-" + std::to_string(index);
        Prism::Asset::TextureAsset texture;
        texture.SetName(textureIds[index]);
        texture.SetSolidColor({
            0.2f + static_cast<float>(index) * 0.1f,
            0.4f,
            0.6f,
            1.0f});
        const std::filesystem::path relativeCooked =
            std::filesystem::path("automation/cache/cooked/test")
            / (textureIds[index] + ".prismtex");
        std::string error;
        Expect(
            Prism::Asset::CookedAssetIO::WriteTexture(
                projectRoot / relativeCooked,
                texture,
                &error),
            "Could not create a Cooked Texture fixture.");
        assets.push_back({
            {"assetId", textureIds[index]},
            {"assetPath", "prism-asset://" + textureIds[index]},
            {"type", "texture"},
            {"name", textureIds[index]},
            {"sourcePath", "fixtures/source.png"},
            {"subresource", "texture/" + std::to_string(index)},
            {"contentHash", "test"},
            {"importRevision", 1},
            {"dependencies", json::array()},
            {"diagnostics", json::array()},
            {"metadata", {
                {"cooked", {
                    {"format", "PrismCookedTexture"},
                    {"version", Prism::Asset::CookedAssetIO::CurrentVersion},
                    {"path", relativeCooked.generic_string()}}}}}});
    }

    const std::string materialId = "test-material";
    Prism::Asset::MaterialAsset material;
    material.SetName(materialId);
    const std::filesystem::path relativeMaterial =
        "automation/cache/cooked/test/test-material.prismmat";
    std::string error;
    Expect(
        Prism::Asset::CookedAssetIO::WriteMaterial(
            projectRoot / relativeMaterial,
            material,
            textureIds,
            &error),
        "Could not create a Cooked Material fixture.");
    assets.push_back({
        {"assetId", materialId},
        {"assetPath", "prism-asset://" + materialId},
        {"type", "material"},
        {"name", materialId},
        {"sourcePath", "fixtures/material.json"},
        {"subresource", "material/0"},
        {"contentHash", "test"},
        {"importRevision", 1},
        {"dependencies", textureIds},
        {"diagnostics", json::array()},
        {"metadata", {
            {"cooked", {
                {"format", "PrismCookedMaterial"},
                {"version", Prism::Asset::CookedAssetIO::CurrentVersion},
                {"path", relativeMaterial.generic_string()}}}}}});

    const std::filesystem::path manifestPath =
        projectRoot / "automation/assets/AssetManifest.json";
    std::filesystem::create_directories(manifestPath.parent_path());
    std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
    output << json{
        {"format", "PrismAssetManifest"},
        {"version", 1},
        {"manifestRevision", 1},
        {"manifestHash", "test"},
        {"assets", std::move(assets)}}.dump(2) << '\n';
    Expect(static_cast<bool>(output), "Could not write the Asset Manifest fixture.");
    return {manifestPath, materialId};
}

void TestImportOwnershipAndReload()
{
    TemporaryDirectory temporary(
        std::filesystem::current_path()
        / "asset-runtime-coordinator-import-tests");
    const std::filesystem::path manifestPath =
        temporary.path / "automation/assets/AssetManifest.json";
    Prism::Core::AssetRuntimeCoordinator coordinator(
        temporary.path,
        manifestPath);
    std::string error;
    Expect(
        coordinator.InitializeImportService(true, &error),
        "Asset import service initialization failed.");
    Expect(
        coordinator.IsImportAvailable()
            && coordinator.GetImportDatabase() != nullptr,
        "The coordinator did not retain Asset Import ownership.");

    TestGraphicsDevice device;
    bool waitedForGpu = false;
    const Prism::Asset::AssetImportResult result =
        coordinator.ImportAndReload(
            std::filesystem::path(PRISM_RENDER_PROJECT_DIR)
                / "assets/scenes/DuckCM.png",
            false,
            device,
            [&]() { waitedForGpu = true; });
    Expect(result.success, "Coordinator import and runtime reload failed.");
    Expect(waitedForGpu, "Import reload skipped the existing GPU safety wait.");
    Expect(
        coordinator.GetRegistry().GetTextureAssetCount() > 0
            && device.createdTextureCount > 0,
        "Import reload did not publish the runtime Texture binding.");
    const std::vector<Prism::Asset::RuntimeAssetBindingChange>
        importChanges = coordinator.GetRegistry()
            .ConsumeRuntimeBindingChanges();
    Expect(
        !importChanges.empty()
            && std::ranges::all_of(
                importChanges,
                [](const Prism::Asset::RuntimeAssetBindingChange& change)
                {
                    return change.kind
                        == Prism::Asset::RuntimeAssetBindingChangeKind::Published;
                }),
        "Import/reload did not expose published runtime binding revisions.");
}

void TestStreamingSafePointCancellationAndShutdown()
{
    TemporaryDirectory temporary(
        std::filesystem::current_path()
        / "asset-runtime-coordinator-streaming-tests");
    const StreamingFixture fixture =
        CreateStreamingFixture(temporary.path);
    Prism::Asset::AssetStreamingConfiguration configuration{};
    configuration.maxUploadsPerTick = 8;
    configuration.residentBudgetBytes = 1;

    Prism::Core::AssetRuntimeCoordinator coordinator(
        temporary.path,
        fixture.manifestPath);
    std::string error;
    Expect(
        coordinator.EnableStreaming(configuration, &error),
        "Asset Streaming initialization failed.");
    Expect(
        coordinator.RequestStreamingAsset(
            fixture.materialId,
            10,
            true,
            &error),
        "Asset Streaming request failed.");
    Expect(
        coordinator.CancelStreamingRequest(
            fixture.materialId,
            true),
        "Asset Streaming cancellation failed.");
    for (const auto& entry : coordinator.GetStreamingEntries())
    {
        Expect(
            entry.referenceCount == 0 && !entry.pinned,
            "Cancellation did not release the dependency closure.");
    }

    Expect(
        coordinator.WaitForStreamingIo(std::chrono::seconds(5)),
        "Cancelled Asset Streaming IO did not become idle.");
    TestGraphicsDevice device;
    Expect(
        coordinator.ProcessStreamingAtRenderPreparation(device) == 0
            && coordinator.GetStreamingStatistics().completedUploadCount == 0,
        "Cancelled unreferenced IO work reached GPU submission.");

    Expect(
        coordinator.RequestStreamingAsset(
            fixture.materialId,
            10,
            false,
            &error),
        "Second Asset Streaming request failed.");
    Expect(
        coordinator.WaitForStreamingIo(std::chrono::seconds(5)),
        "Asset Streaming IO did not become idle.");
    const auto beforeUpload = coordinator.GetStreamingStatistics();
    Expect(
        beforeUpload.completedIoCount == 6
            && beforeUpload.completedUploadCount == 0,
        "The IO worker published GPU bindings before the render safe point.");
    Expect(
        !coordinator.GetRegistry()
             .FindMaterialByPath("prism-asset://test-material")
             .IsValid(),
        "The IO worker modified the runtime registry directly.");

    const Prism::Core::AssetRuntimeRenderWorkResult firstWork =
        coordinator.ProcessStreamingRenderWork(device, 1);
    const Prism::Core::AssetRuntimeRenderWorkResult secondWork =
        coordinator.ProcessStreamingRenderWork(device, 2);
    const auto afterUpload = coordinator.GetStreamingStatistics();
    Expect(
        firstWork.uploadedCount == 5
            && secondWork.uploadedCount == 1
            && afterUpload.completedUploadCount == 6,
        "Render-preparation upload completion did not stage the full dependency closure.");
    Expect(
        !coordinator.GetRegistry()
             .FindMaterialByPath("prism-asset://test-material")
             .IsValid()
            && coordinator.GetPendingStreamingBindingBatchCount() == 2,
        "The render lane published staged bindings directly into the main-thread registry.");
    const Prism::Core::AssetRuntimeBindingApplyResult appliedUploads =
        coordinator.ApplyCompletedStreamingBindings();
    Expect(
        appliedUploads.batchCount == 2
            && appliedUploads.bindingUpdateCount == 6
            && appliedUploads.lastLogicalFrameId == 2
            && coordinator.GetPendingStreamingBindingBatchCount() == 0,
        "Main-thread binding feedback did not preserve ordered render batches.");
    const Prism::Asset::MaterialHandle materialHandle =
        coordinator.GetRegistry().FindMaterialByPath(
            "prism-asset://test-material");
    Expect(
        materialHandle.IsValid()
            && coordinator.GetRegistry().GetRuntimeMaterial(materialHandle)
                != nullptr,
        "The completed streaming Material binding is unavailable.");
    const std::vector<Prism::Asset::RuntimeAssetBindingChange>
        uploadChanges = coordinator.GetRegistry()
            .ConsumeRuntimeBindingChanges();
    Expect(
        !uploadChanges.empty()
            && std::ranges::all_of(
                uploadChanges,
                [](const Prism::Asset::RuntimeAssetBindingChange& change)
                {
                    return change.kind
                        == Prism::Asset::RuntimeAssetBindingChangeKind::Published
                        && static_cast<bool>(change.revision);
                }),
        "Streaming upload completion did not expose binding revisions.");

    Expect(
        coordinator.CancelStreamingRequest(fixture.materialId),
        "Completed request release failed.");
    const Prism::Core::AssetRuntimeRenderWorkResult evictionWork =
        coordinator.ProcessStreamingRenderWork(device, 3);
    Expect(
        evictionWork.evictedCount > 0,
        "Unreferenced streaming assets were not staged for eviction at the render safe point.");
    Expect(
        coordinator.GetRegistry().GetRuntimeMaterial(materialHandle)
            != nullptr,
        "The render lane cleared a main-thread binding before feedback application.");
    const Prism::Core::AssetRuntimeBindingApplyResult appliedEviction =
        coordinator.ApplyCompletedStreamingBindings();
    Expect(
        appliedEviction.bindingUpdateCount > 0
            && appliedEviction.lastLogicalFrameId == 3,
        "Eviction binding feedback was not applied in frame order.");
    const std::vector<Prism::Asset::RuntimeAssetBindingChange>
        evictionChanges = coordinator.GetRegistry()
            .ConsumeRuntimeBindingChanges();
    Expect(
        !evictionChanges.empty()
            && std::ranges::any_of(
                evictionChanges,
                [](const Prism::Asset::RuntimeAssetBindingChange& change)
                {
                    return change.kind
                        == Prism::Asset::RuntimeAssetBindingChangeKind::Cleared;
                }),
        "Streaming eviction did not publish cleared binding revisions.");
    coordinator.Shutdown();
    coordinator.Shutdown();
    Expect(
        !coordinator.IsStreamingEnabled()
            && !coordinator.IsImportAvailable(),
        "Coordinator shutdown was not idempotent.");

    // Queue work and destroy immediately. Destruction must stop acceptance,
    // wake the worker and join it before the fixture directory is removed.
    {
        Prism::Core::AssetRuntimeCoordinator joiningCoordinator(
            temporary.path,
            fixture.manifestPath);
        Expect(
            joiningCoordinator.EnableStreaming(configuration, &error),
            "Join fixture streaming initialization failed.");
        Expect(
            joiningCoordinator.RequestStreamingAsset(
                fixture.materialId,
                1,
                false,
                &error),
            "Join fixture request failed.");
    }
}
} // namespace

int main()
{
    try
    {
        TestImportOwnershipAndReload();
        TestStreamingSafePointCancellationAndShutdown();
        std::cout << "AssetRuntimeCoordinator tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "AssetRuntimeCoordinator tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
