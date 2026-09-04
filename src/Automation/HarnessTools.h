#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

#include <json.hpp>

namespace Prism::Engine
{
class CommandProcessor;
}

namespace Prism::Automation
{
class HarnessTools
{
public:
    HarnessTools(std::filesystem::path projectRoot, std::filesystem::path executableDirectory);

    [[nodiscard]] bool CanHandle(std::string_view command) const;
    [[nodiscard]] nlohmann::json Execute(
        const nlohmann::json& request,
        const Engine::CommandProcessor& processor);
    void AugmentEngineDescription(nlohmann::json& result) const;

private:
    struct RenderWorldInput
    {
        bool enabled = false;
        std::string source = "startupScene";
        std::filesystem::path snapshotPath;
        std::string worldHash;
        std::filesystem::path assetManifestPath;
        std::string assetManifestHash;
    };

    [[nodiscard]] nlohmann::json ListAssets(const nlohmann::json& arguments) const;
    [[nodiscard]] nlohmann::json DescribeAsset(const nlohmann::json& arguments) const;
    [[nodiscard]] nlohmann::json DescribeAssetCache(const nlohmann::json& arguments) const;
    [[nodiscard]] nlohmann::json GarbageCollectAssetCache(
        const nlohmann::json& arguments) const;
    [[nodiscard]] nlohmann::json InspectCrashReport(const nlohmann::json& arguments) const;
    [[nodiscard]] nlohmann::json SymbolizeCrash(
        const nlohmann::json& arguments) const;
    [[nodiscard]] nlohmann::json ImportAsset(const nlohmann::json& arguments) const;
    [[nodiscard]] nlohmann::json ReimportAsset(const nlohmann::json& arguments) const;
    [[nodiscard]] nlohmann::json PlanAssetStreaming(
        const nlohmann::json& arguments) const;
    [[nodiscard]] nlohmann::json ValidateAssetStreaming(
        const nlohmann::json& arguments,
        std::string_view requestId) const;
    [[nodiscard]] nlohmann::json CompileShader(const nlohmann::json& arguments) const;
    [[nodiscard]] nlohmann::json CompareGoldenImages(const nlohmann::json& arguments) const;
    [[nodiscard]] nlohmann::json CaptureRender(
        const nlohmann::json& arguments,
        const Engine::CommandProcessor& processor,
        std::string_view requestId,
        const RenderWorldInput* preparedWorld = nullptr) const;
    [[nodiscard]] nlohmann::json CompareGraphicsApis(
        const nlohmann::json& arguments,
        const Engine::CommandProcessor& processor,
        std::string_view requestId) const;
    [[nodiscard]] nlohmann::json DescribeRenderGraph(
        const nlohmann::json& arguments,
        const Engine::CommandProcessor& processor,
        std::string_view requestId) const;
    [[nodiscard]] nlohmann::json MeasurePerformance(
        const nlohmann::json& arguments,
        const Engine::CommandProcessor& processor,
        std::string_view requestId) const;
    [[nodiscard]] nlohmann::json CompareQueueModes(
        const nlohmann::json& arguments,
        const Engine::CommandProcessor& processor,
        std::string_view requestId) const;
    [[nodiscard]] RenderWorldInput PrepareRenderWorld(
        const nlohmann::json& arguments,
        const Engine::CommandProcessor& processor,
        std::string_view requestId) const;
    [[nodiscard]] std::filesystem::path ResolveProjectPath(const std::filesystem::path& path) const;
    [[nodiscard]] std::filesystem::path ResolveGeneratedPath(const std::filesystem::path& path) const;
    std::filesystem::path m_projectRoot;
    std::filesystem::path m_executableDirectory;
    std::unordered_map<std::string, nlohmann::json> m_requestCache;
};
} // namespace Prism::Automation
