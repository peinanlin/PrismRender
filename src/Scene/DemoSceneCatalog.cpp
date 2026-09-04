#include "Scene/DemoSceneCatalog.h"

#include "Scene/DefaultSceneFactory.h"
#include "Scene/FeatureLabSceneFactory.h"
#include "Scene/FluidLabSceneFactory.h"
#include "Scene/LightingShowcaseSceneFactory.h"
#include "Scene/RenderScene.h"
#include "Scene/ShadowShowcaseSceneFactory.h"
#include "Scene/ShowcaseSceneFactory.h"
#include "Scene/TerrainOceanSceneFactory.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace Prism::Scene
{
namespace
{
constexpr DemoSceneCatalog::Descriptions SceneDescriptions{{
    {DemoSceneId::EditorPreview,
     "preview",
     "Editor Preview",
     "General editor preview and startup glTF scene."},
    {DemoSceneId::Showcase,
     "showcase",
     "Renderer Showcase",
     "Materials, instancing, post-processing, IBL and reflections."},
    {DemoSceneId::ReflectionLab,
     "reflections",
     "Reflection Lab",
     "IBL fallback, validated SSR and a mirror-camera planar reflection pass."},
    {DemoSceneId::ShadowLab,
     "shadows",
     "Shadow Lab",
     "Directional CSM, spot-light shadow arrays and point-light cube shadows."},
    {DemoSceneId::LightingLab,
     "lights",
     "Forward+ Lab",
     "Deferred and Forward+ share one clustered light list; legacy Forward keeps its four-light limit."},
    {DemoSceneId::GpuDrivenLab,
     "gpu-driven",
     "GPU Driven Stress Lab",
     "Frustum/Hi-Z culling, instancing, indirect draws and PSO cache under repeated geometry."},
    {DemoSceneId::PostProcessLab,
     "post-process",
     "Post Process Lab",
     "HDR, bloom, tonemapping, TAA and GTAO with controlled emissive intensity steps."},
    {DemoSceneId::RenderGraphLab,
     "render-graph",
     "RenderGraph Lab",
     "Graphics/compute scheduling, cross-queue synchronization, transient targets and GPU timeline."},
    {DemoSceneId::MaterialLab,
     "materials",
     "Material Lab",
     "Metallic/roughness grid with PBR, IBL, normal, AO and emissive feature switches."},
    {DemoSceneId::AssetStreamingLab,
     "streaming",
     "Asset Streaming Lab",
     "Cooked assets, asynchronous upload, residency budget and LRU eviction diagnostics."},
    {DemoSceneId::AtmosphereLab,
     "atmosphere",
     "Physical Atmosphere Lab",
     "Rayleigh/Mie scattering, transmittance and Sky-View LUTs under a low-angle sun."},
    {DemoSceneId::LargeWorldLab,
     "large-world",
     "Large World / RTE Lab",
     "Double-precision absolute transforms and camera-relative GPU coordinates at planetary scale."},
    {DemoSceneId::TerrainVirtualTextureLab,
     "terrain-vt",
     "Terrain & Virtual Texture Lab",
     "Real heightfield patches with GPU quadtree LOD, frustum/Hi-Z culling, indirect draws, and LRU virtual-texture pages."},
    {DemoSceneId::OceanLab,
     "ocean",
     "FFT Ocean Lab",
     "Phillips spectrum, two-dimensional inverse FFT, displacement, normal, and foam maps."},
    {DemoSceneId::WaveWorksLab,
     "waveworks-ocean",
     "WaveWorks Ocean Lab",
     "WaveWorks-inspired JONSWAP wind and swell, reference controls, foam, and adaptive-ocean migration.",
     EditorSceneViewUpdatePolicy::OnInteraction},
    {DemoSceneId::HpWaterOceanLab,
     "hpwater-ocean",
     "HPWater Ocean Optics Lab",
     "Large-area spectral ocean with local interactions and a clean-room HPWater-style optical pipeline.",
     EditorSceneViewUpdatePolicy::OnInteraction},
    {DemoSceneId::PbfLab,
     "pbf",
     "PBF Particle Demo",
     "GPU position-based fluid simulation with direct particle-mask visualization."},
    {DemoSceneId::FluidRenderLab,
     "fluid-render",
     "Screen-Space Fluid Demo",
     "GPU PBF plus realistic screen-space depth, thickness, filtering, normals, reflection, and refraction."},
    {DemoSceneId::FluidCausticsLab,
     "fluid-caustics",
     "Fluid Caustics Demo",
     "Realistic fluid with a receiver-space image-space refracted-photon gather over a high-contrast receiver."},
    {DemoSceneId::FluidToonLab,
     "fluid-toon",
     "Toon Fluid and Foam Demo",
     "GPU PBF with quantized toon water, outlines, and density-driven foam."},
}};

std::string Normalize(std::string_view value)
{
    std::string normalized(value);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return normalized;
}
} // namespace

const DemoSceneCatalog::Descriptions&
DemoSceneCatalog::GetDescriptions()
{
    return SceneDescriptions;
}

const DemoSceneDescription& DemoSceneCatalog::GetDescription(
    const DemoSceneId id)
{
    const std::size_t index = static_cast<std::size_t>(id);
    if (index >= SceneDescriptions.size())
    {
        throw std::out_of_range("Unknown demo scene identifier.");
    }
    return SceneDescriptions[index];
}

DemoSceneId DemoSceneCatalog::Parse(
    const std::string_view key,
    const DemoSceneId fallback)
{
    return TryParse(key).value_or(fallback);
}

std::optional<DemoSceneId> DemoSceneCatalog::TryParse(
    const std::string_view key)
{
    if (key.empty())
    {
        return std::nullopt;
    }
    const std::string normalized = Normalize(key);
    for (const DemoSceneDescription& description : SceneDescriptions)
    {
        if (normalized == description.key
            || normalized == Normalize(description.displayName))
        {
            return description.id;
        }
    }
    if (normalized == "shadow" || normalized == "shadowlab")
    {
        return DemoSceneId::ShadowLab;
    }
    if (normalized == "reflection" || normalized == "reflectionlab")
    {
        return DemoSceneId::ReflectionLab;
    }
    if (normalized == "lighting" || normalized == "lightinglab"
        || normalized == "multi-light")
    {
        return DemoSceneId::LightingLab;
    }
    if (normalized == "forward+" || normalized == "forwardplus")
    {
        return DemoSceneId::LightingLab;
    }
    if (normalized == "gpu" || normalized == "gpudriven")
    {
        return DemoSceneId::GpuDrivenLab;
    }
    if (normalized == "sky" || normalized == "sky-atmosphere"
        || normalized == "atmospherelab")
    {
        return DemoSceneId::AtmosphereLab;
    }
    if (normalized == "largeworld" || normalized == "rte"
        || normalized == "relative-to-eye")
    {
        return DemoSceneId::LargeWorldLab;
    }
    if (normalized == "terrain" || normalized == "terrainvt"
        || normalized == "virtual-terrain")
    {
        return DemoSceneId::TerrainVirtualTextureLab;
    }
    if (normalized == "fft-ocean" || normalized == "fftocean"
        || normalized == "sea")
    {
        return DemoSceneId::OceanLab;
    }
    if (normalized == "waveworks" || normalized == "waveworks-lab"
        || normalized == "spectral-ocean" || normalized == "jonswap-ocean")
    {
        return DemoSceneId::WaveWorksLab;
    }
    if (normalized == "hpwater" || normalized == "hp-water"
        || normalized == "hpwater-lab" || normalized == "hp-water-ocean")
    {
        return DemoSceneId::HpWaterOceanLab;
    }
    if (normalized == "position-based-fluids")
    {
        return DemoSceneId::PbfLab;
    }
    if (normalized == "fluid" || normalized == "fluids"
        || normalized == "fluid-lab")
    {
        return DemoSceneId::FluidRenderLab;
    }
    return std::nullopt;
}

bool DemoSceneCatalog::IsFeatureDemo(const DemoSceneId id)
{
    return id != DemoSceneId::EditorPreview;
}

bool DemoSceneCatalog::IsFluidDemo(const DemoSceneId id)
{
    return id == DemoSceneId::PbfLab
        || id == DemoSceneId::FluidRenderLab
        || id == DemoSceneId::FluidCausticsLab
        || id == DemoSceneId::FluidToonLab;
}

DemoSceneBuildResult DemoSceneCatalog::Populate(
    const DemoSceneId id,
    Asset::AssetRegistry& assetRegistry,
    RHI::IGraphicsDevice& device,
    RenderScene& scene,
    const float cameraAspectRatio)
{
    switch (id)
    {
    case DemoSceneId::EditorPreview:
        DefaultSceneFactory::PopulateEditorPreviewScene(
            assetRegistry,
            device,
            scene);
        DefaultSceneFactory::ConfigureEditorPreviewWorld(
            scene,
            cameraAspectRatio);
        return {
            static_cast<std::uint32_t>(
                scene.GetRenderObjects().size()),
            scene.GetActivePointLightCount(),
            scene.GetActiveSpotLightCount(),
            "Built-in editor preview scene is active."};

    case DemoSceneId::Showcase:
    {
        const ShowcaseSceneSummary summary =
            ShowcaseSceneFactory::Populate(
                assetRegistry,
                device,
                scene);
        ShowcaseSceneFactory::ConfigureWorld(
            scene,
            cameraAspectRatio);
        return {
            summary.renderObjectCount,
            summary.pointLightCount,
            summary.spotLightCount,
            "Showcase scene active: "
                + std::to_string(summary.renderObjectCount)
                + " objects, "
                + std::to_string(summary.materialSampleCount)
                + " material samples, "
                + std::to_string(summary.instanceObjectCount)
                + " repeated instances and "
                + std::to_string(summary.pointLightCount)
                + " point lights."};
    }

    case DemoSceneId::ReflectionLab:
    {
        const ShowcaseSceneSummary summary =
            ShowcaseSceneFactory::Populate(
                assetRegistry,
                device,
                scene);
        ShowcaseSceneFactory::ConfigureWorld(
            scene,
            cameraAspectRatio);
        return {
            summary.renderObjectCount,
            summary.pointLightCount,
            summary.spotLightCount,
            "Reflection Lab active: IBL fallback plus SSR and a planar mirror-camera pass on the floor."};
    }

    case DemoSceneId::ShadowLab:
    {
        const ShadowShowcaseSceneSummary summary =
            ShadowShowcaseSceneFactory::Populate(
                assetRegistry,
                device,
                scene);
        ShadowShowcaseSceneFactory::ConfigureWorld(
            scene,
            cameraAspectRatio);
        return {
            summary.renderObjectCount,
            summary.pointLightCount,
            summary.spotLightCount,
            "Shadow Lab active: directional CSM subjects plus "
                + std::to_string(summary.spotLightCount)
                + " shadowed spot lights and "
                + std::to_string(summary.pointLightCount)
                + " point lights."};
    }

    case DemoSceneId::LightingLab:
    {
        const LightingShowcaseSceneSummary summary =
            LightingShowcaseSceneFactory::Populate(
                assetRegistry,
                device,
                scene);
        LightingShowcaseSceneFactory::ConfigureWorld(
            scene,
            cameraAspectRatio);
        return {
            summary.renderObjectCount,
            summary.pointLightCount,
            0,
            "Forward+ Lab active: "
                + std::to_string(summary.pointLightCount)
                + " point lights and "
                + std::to_string(summary.materialSubjectCount)
                + " PBR subjects. Deferred and Forward+ use the same cluster lists; "
                  "disable Forward+ to inspect the four-light compatibility path."};
    }

    case DemoSceneId::TerrainVirtualTextureLab:
    {
        TerrainOceanSceneFactory::ConfigureTerrainWorld(
            scene, cameraAspectRatio);
        const TerrainOceanSceneSummary summary =
            TerrainOceanSceneFactory::PopulateTerrain(
                assetRegistry, device, scene);
        return {
            summary.renderObjectCount,
            0u,
            0u,
            "Terrain & Virtual Texture Lab active: "
                + std::to_string(summary.patchCount)
                + " heightfield quadtree patches through level "
                + std::to_string(summary.maxQuadtreeLevel)
                + ". Scene View uses the same editor navigation controls as the other Labs; Final Output and GPU culling use the independent Game Camera."};
    }

    case DemoSceneId::OceanLab:
    case DemoSceneId::WaveWorksLab:
    case DemoSceneId::HpWaterOceanLab:
    {
        const bool waveWorksReference = id == DemoSceneId::WaveWorksLab;
        const bool hpWaterReference = id == DemoSceneId::HpWaterOceanLab;
        const OceanLabSceneKind kind = hpWaterReference
            ? OceanLabSceneKind::HpWaterReference
            : (waveWorksReference
                ? OceanLabSceneKind::WaveWorksReference
                : OceanLabSceneKind::LegacyFft);
        const TerrainOceanSceneSummary summary =
            TerrainOceanSceneFactory::PopulateOcean(
                assetRegistry,
                device,
                scene,
                kind);
        TerrainOceanSceneFactory::ConfigureOceanWorld(
            scene,
            cameraAspectRatio,
            kind);
        return {
            summary.renderObjectCount,
            0u,
            0u,
            hpWaterReference
                ? "HPWater Ocean Optics Lab active: WaveWorks-reference spectral geometry with the project-owned HPWater optical model."
                : waveWorksReference
                ? "WaveWorks Ocean Lab active: reference JONSWAP wind and swell parameters on the native spectral migration path."
                : "FFT Ocean Lab active: Phillips spectrum, 14 butterfly stages, displacement, normals, and foam."};
    }

    case DemoSceneId::PbfLab:
    case DemoSceneId::FluidRenderLab:
    case DemoSceneId::FluidCausticsLab:
    case DemoSceneId::FluidToonLab:
    {
        FluidLabSceneKind kind = FluidLabSceneKind::ScreenSpace;
        const char* summaryText =
            "Screen-Space Fluid Demo active: GPU PBF plus realistic surface reconstruction, reflection, and refraction.";
        if (id == DemoSceneId::PbfLab)
        {
            kind = FluidLabSceneKind::PbfParticles;
            summaryText =
                "PBF Particle Demo active: GPU constraint projection and direct particle-mask visualization only.";
        }
        else if (id == DemoSceneId::FluidCausticsLab)
        {
            kind = FluidLabSceneKind::Caustics;
            summaryText =
                "Fluid Caustics Demo active: realistic screen-space water and refractive caustics over a high-contrast receiver.";
        }
        else if (id == DemoSceneId::FluidToonLab)
        {
            kind = FluidLabSceneKind::ToonFoam;
            summaryText =
                "Toon Fluid Demo active: quantized screen-space water, outlines, and density-driven foam.";
        }
        const FluidLabSceneSummary summary =
            FluidLabSceneFactory::Populate(
                assetRegistry,
                device,
                scene,
                kind);
        FluidLabSceneFactory::ConfigureWorld(
            scene,
            cameraAspectRatio,
            kind);
        return {
            summary.renderObjectCount,
            scene.GetActivePointLightCount(),
            scene.GetActiveSpotLightCount(),
            summaryText};
    }

    case DemoSceneId::GpuDrivenLab:
    case DemoSceneId::PostProcessLab:
    case DemoSceneId::RenderGraphLab:
    case DemoSceneId::MaterialLab:
    case DemoSceneId::AssetStreamingLab:
    case DemoSceneId::AtmosphereLab:
    case DemoSceneId::LargeWorldLab:
    {
        FeatureLabSceneKind kind = FeatureLabSceneKind::GpuDriven;
        const char* label = "GPU Driven Stress Lab";
        if (id == DemoSceneId::PostProcessLab)
        {
            kind = FeatureLabSceneKind::PostProcess;
            label = "Post Process Lab";
        }
        else if (id == DemoSceneId::RenderGraphLab)
        {
            kind = FeatureLabSceneKind::RenderGraph;
            label = "RenderGraph Lab";
        }
        else if (id == DemoSceneId::MaterialLab)
        {
            kind = FeatureLabSceneKind::Material;
            label = "Material Lab";
        }
        else if (id == DemoSceneId::AssetStreamingLab)
        {
            kind = FeatureLabSceneKind::AssetStreaming;
            label = "Asset Streaming Lab";
        }
        else if (id == DemoSceneId::AtmosphereLab)
        {
            kind = FeatureLabSceneKind::Atmosphere;
            label = "Physical Atmosphere Lab";
        }
        else if (id == DemoSceneId::LargeWorldLab)
        {
            kind = FeatureLabSceneKind::LargeWorld;
            label = "Large World / RTE Lab";
        }
        const FeatureLabSceneSummary summary =
            FeatureLabSceneFactory::Populate(
                kind,
                assetRegistry,
                device,
                scene);
        FeatureLabSceneFactory::ConfigureWorld(
            kind,
            scene,
            cameraAspectRatio);
        return {
            summary.renderObjectCount,
            summary.pointLightCount,
            0u,
            std::string(label) + " active: "
                + std::to_string(summary.subjectCount)
                + " controlled subjects and "
                + std::to_string(summary.pointLightCount)
                + " point lights."};
    }

    case DemoSceneId::Count:
        break;
    }
    throw std::invalid_argument("Cannot populate an unknown demo scene.");
}
} // namespace Prism::Scene
