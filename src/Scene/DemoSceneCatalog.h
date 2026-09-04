#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Prism::Asset
{
class AssetRegistry;
}

namespace Prism::RHI
{
class IGraphicsDevice;
}

namespace Prism::Scene
{
class RenderScene;

enum class DemoSceneId : std::uint32_t
{
    EditorPreview = 0,
    Showcase,
    ReflectionLab,
    ShadowLab,
    LightingLab,
    GpuDrivenLab,
    PostProcessLab,
    RenderGraphLab,
    MaterialLab,
    AssetStreamingLab,
    AtmosphereLab,
    LargeWorldLab,
    TerrainVirtualTextureLab,
    OceanLab,
    WaveWorksLab,
    HpWaterOceanLab,
    PbfLab,
    FluidRenderLab,
    FluidCausticsLab,
    FluidToonLab,
    Count,
    // Source-compatible alias for integrations that used the original
    // all-in-one Fluid Lab identifier. The `fluid` scene key also resolves
    // to the realistic screen-space demo.
    FluidLab = FluidRenderLab
};

enum class EditorSceneViewUpdatePolicy : std::uint8_t
{
    Continuous,
    OnInteraction
};

struct DemoSceneDescription
{
    DemoSceneId id = DemoSceneId::EditorPreview;
    std::string_view key;
    std::string_view displayName;
    std::string_view purpose;
    // Expensive presentation-focused labs may keep the independent editor
    // view as an on-demand snapshot while Game continues to render every
    // frame. Other scenes retain the normal continuous editor viewport.
    EditorSceneViewUpdatePolicy editorSceneViewUpdatePolicy =
        EditorSceneViewUpdatePolicy::Continuous;
};

struct DemoSceneBuildResult
{
    std::uint32_t renderObjectCount = 0;
    std::uint32_t pointLightCount = 0;
    std::uint32_t spotLightCount = 0;
    std::string summary;
};

class DemoSceneCatalog
{
public:
    using Descriptions = std::array<
        DemoSceneDescription,
        static_cast<std::size_t>(DemoSceneId::Count)>;

    [[nodiscard]] static const Descriptions& GetDescriptions();
    [[nodiscard]] static const DemoSceneDescription& GetDescription(
        DemoSceneId id);
    [[nodiscard]] static DemoSceneId Parse(
        std::string_view key,
        DemoSceneId fallback = DemoSceneId::EditorPreview);
    [[nodiscard]] static std::optional<DemoSceneId> TryParse(
        std::string_view key);
    [[nodiscard]] static bool IsFeatureDemo(DemoSceneId id);
    [[nodiscard]] static bool IsFluidDemo(DemoSceneId id);

    static DemoSceneBuildResult Populate(
        DemoSceneId id,
        Asset::AssetRegistry& assetRegistry,
        RHI::IGraphicsDevice& device,
        RenderScene& scene,
        float cameraAspectRatio);
};
} // namespace Prism::Scene
