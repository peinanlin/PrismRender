#pragma once

#include <DirectXMath.h>

#include <cstdint>

namespace Prism::Renderer
{
enum class FluidRenderMode : std::uint32_t
{
    Realistic,
    Toon,
    Particles,
    Depth,
    Thickness,
    Normals,
    Foam
};

enum class FluidSpawnLayout : std::uint32_t
{
    Block,
    DoubleDam
};

// Identifies the intentionally narrow demo pipeline selected by the scene
// catalog. Custom preserves the full laboratory UI for embedding elsewhere.
enum class FluidDemoPipeline : std::uint32_t
{
    Custom,
    PbfParticles,
    ScreenSpaceRealistic,
    ScreenSpaceCaustics,
    ScreenSpaceToonFoam
};

// Runtime-tunable PBF parameters. Settings that affect buffer capacity
// (particle count, domain, smoothing radius, bucket capacity, and compact
// neighbor capacity) trigger a resource rebuild when changed.
struct FluidSettings
{
    static constexpr std::uint32_t DefaultParticleCount =
        32u * 32u * 32u;

    bool enabled = false;
    bool paused = false;
    bool resetRequested = false;
    FluidDemoPipeline demoPipeline = FluidDemoPipeline::Custom;
    // Scene View owns a second SceneRenderer. Keep its independent fluid
    // simulation off unless the user explicitly asks for the costly preview.
    bool editorPreviewEnabled = false;
    std::uint32_t particleCount = DefaultParticleCount;
    std::uint32_t solverIterations = 4;
    std::uint32_t substepCount = 1;
    std::uint32_t maxParticlesPerCell = 64;
    std::uint32_t maxNeighborsPerParticle = 128;

    float timeScale = 1.0f;
    float maxFrameDeltaTime = 1.0f / 30.0f;
    float particleRadius = 0.045f;
    float smoothingRadius = 0.18f;

    float restDensity = 1000.0f;
    float particleMass = 0.5832f;
    float lambdaEpsilon = 100.0f;
    float maxPositionCorrection = 0.08f;

    float artificialPressureK = 0.001f;
    float artificialPressureQ = 0.3f;
    float artificialPressureN = 4.0f;
    float viscosity = 0.005f;

    float vorticity = 0.0f;
    float boundaryRestitution = 0.05f;
    // A particle trapped where the floor meets two side planes can turn PBF
    // projection corrections into a persistent upward corner jet. Retain only
    // this fraction of tangential velocity and cap the generated upward speed
    // while the particle is in three-plane contact.
    float boundaryCornerDamping = 0.15f;
    float boundaryCornerUpwardVelocityLimit = 0.35f;
    float maxVelocity = 20.0f;
    DirectX::XMFLOAT3 gravity{0.0f, -9.81f, 0.0f};
    DirectX::XMFLOAT3 externalAcceleration{};

    DirectX::XMFLOAT3 domainMin{-2.5f, 0.0f, -2.5f};
    DirectX::XMFLOAT3 domainMax{2.5f, 7.0f, 2.5f};
    FluidSpawnLayout spawnLayout = FluidSpawnLayout::DoubleDam;
    DirectX::XMFLOAT3 spawnMin{-2.10f, 1.15f, -0.72f};
    DirectX::XMFLOAT3 spawnMax{2.10f, 6.91f, 0.72f};

    FluidRenderMode renderMode = FluidRenderMode::Realistic;
    // Screen-space reconstruction needs overlapping splats. Keep this
    // independent from the physical collision radius used by the solver.
    float renderParticleRadiusScale = 2.0f;
    // A lone particle contributes roughly 0.156 rho0 with the default kernel.
    // The reference 0.20 cutoff therefore removes isolated spray while keeping
    // connected free-surface particles; the lab UI can still tune the ratio.
    float minimumRenderDensityRatio = 0.20f;
    // Dynamic spray rejection is independent from the density cutoff. A
    // grounded thin sheet can be under-dense and still form a coherent
    // surface, while a fast particle with very few PBF neighbours is rendered
    // as a distracting sphere splat.
    std::uint32_t minimumSplashNeighborCount = 10;
    float splashVelocityThreshold = 1.75f;
    float splashDensityRatioThreshold = 0.45f;
    // Optional additive-thickness fringe cutoff. Keep disabled by default:
    // the primary surface quality control is a physically consistent PBF LOD,
    // while a hard cutoff can erase thin sheets and wave crests.
    float surfaceCoverageThreshold = 0.0f;
    // A coarse-to-fine separable schedule removes both the large sphere domes
    // and their small residual normal ripples. The renderer derives per-pass
    // radii from this initial radius (15, 13, ... 1 for eight iterations).
    std::uint32_t bilateralIterations = 8;
    std::uint32_t bilateralRadius = 15;
    float bilateralSpatialSigma = 7.5f;
    // Linear view-space depth needs a broad range sigma to smooth the regular
    // particle layers. The validity mask, not a tiny sigma, protects edges.
    float bilateralDepthSigma = 1.0f;
    // Reconstruct normals across a wider pixel baseline than the local depth
    // derivative. This suppresses projected sphere-cap normals when the camera
    // moves close without increasing simulation particle count.
    std::uint32_t normalSmoothingRadius = 4;
    // Smooth only the reconstructed fluid silhouette. A majority filter at
    // the outer edge removes the last particle-sized scallops without
    // flattening waves in the interior of the water surface.
    std::uint32_t silhouetteSmoothingRadius = 3;

    DirectX::XMFLOAT3 waterColor{0.08f, 0.28f, 0.95f};
    DirectX::XMFLOAT3 absorption{2.40f, 1.25f, 0.06f};
    DirectX::XMFLOAT3 scattering{0.02f, 0.08f, 0.32f};
    float ior = 1.333f;
    float refractionScale = 0.025f;
    float reflectionStrength = 0.72f;
    // The thickness pass accumulates a physical sphere chord. Scale it down
    // after enlarging only the visual splat radius.
    float thicknessScale = 0.55f;

    bool toonEnabled = false;
    std::uint32_t toonBands = 3;
    float toonEdgeWidth = 1.5f;

    bool foamEnabled = false;
    float foamDensityThreshold = 0.50f;
    std::uint32_t foamErosionIterations = 1;

    // Receiver-space image-space refracted-photon gather. This is not the
    // reference renderer's light-space photon-buffer/additive-splat path.
    bool causticsEnabled = false;
    bool causticsDebugView = false;
    float causticsIntensity = 0.85f;
    float causticsRefractionScalePixels = 18.0f;
    float causticsDepthAttenuation = 0.12f;
    float causticsFocusStrength = 2.0f;
    float causticsFocusPower = 3.0f;
    std::uint32_t causticsBlurRadius = 4;
    float causticsBlurSigma = 2.0f;
};
} // namespace Prism::Renderer
