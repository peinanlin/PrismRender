#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Prism::Renderer
{
struct RenderSettings;
struct RendererStatistics;
}

namespace Prism::UI
{
class DebugPanel
{
public:
    struct DemoSceneOption
    {
        std::string displayName;
        std::string purpose;
    };

    struct FrameStats
    {
        std::uint32_t frameIndex = 0;
        std::uint32_t windowWidth = 0;
        std::uint32_t windowHeight = 0;
        std::uint32_t renderObjectCount = 0;
        std::uint32_t activePointLightCount = 0;
        std::uint32_t activeSpotLightCount = 0;
        std::uint32_t meshAssetCount = 0;
        std::uint32_t textureAssetCount = 0;
        std::uint32_t materialAssetCount = 0;
        std::string adapterName;
        std::string graphicsApiName;
        std::string sceneSourceLabel;
        std::string sceneLoadMessage;
        std::string environmentMapStatus;
        std::vector<std::string> sceneObjectSummaries;
        std::vector<DemoSceneOption> demoScenes;
        std::uint32_t activeDemoSceneIndex = 0;
        bool demoSceneSwitchingEnabled = false;
        bool gltfImportEnabled = false;
        bool usingFallbackScene = false;
        bool assetStreamingEnabled = false;
        std::uint32_t streamingQueuedCount = 0;
        std::uint32_t streamingResidentCount = 0;
        std::uint32_t streamingEvictedCount = 0;
        std::uint32_t streamingFailedCount = 0;
        std::uint64_t streamingResidentBytes = 0;
        std::uint64_t streamingBudgetBytes = 0;
        std::uint64_t streamingCompletedUploads = 0;
        std::uint64_t streamingEvictionCount = 0;
        DirectX::XMFLOAT3 cameraPosition{0.0f, 0.0f, 0.0f};
        float cameraPitch = 0.0f;
        float cameraYaw = 0.0f;
        float cameraFovYDegrees = 0.0f;
        float cameraNearPlane = 0.0f;
        float cameraFarPlane = 0.0f;
        float cameraMoveSpeed = 0.0f;
        bool cameraLookActive = false;
    };

    [[nodiscard]] std::optional<std::uint32_t> Draw(
        const FrameStats& stats,
        Renderer::RenderSettings& settings,
        const Renderer::RendererStatistics& rendererStatistics) const;
};
} // namespace Prism::UI
