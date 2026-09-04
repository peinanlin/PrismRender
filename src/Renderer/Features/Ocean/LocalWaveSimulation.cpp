#include "Renderer/Features/Ocean/LocalWaveSimulation.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Prism::Renderer
{
namespace
{
constexpr std::array<std::uint32_t, 5> SupportedGridSizes{
    128u, 256u, 512u, 1024u, 2048u};
constexpr float LocalWaveVelocityDamping = 0.6f;
constexpr float ReferenceSimulationRate = 60.0f;
constexpr float SpatialDissipationPerStep = 0.25f;
// The WaveWorks reference application injects a 10 x 10 grid-space block at
// the moving boat position every frame.  Preserve that footprint here and let
// the local wave equation generate the divergent wake from the moving source.
constexpr std::uint32_t ReferenceHullSamplesPerAxis = 10u;
constexpr float ReferenceOrbitLongitudinalRadiusFraction = 0.20f;
constexpr float ReferenceOrbitLateralRadiusFraction = 0.125f;
constexpr float ReferenceHullHalfLengthCells = 4.5f;
constexpr float ReferenceHullMinimumHalfWidthCells = 0.6f;
constexpr float ReferenceHullMaximumHalfWidthCells = 2.4f;
// One physical hull source is represented by 100 overlapping samples. Scale
// each sample so their combined energy remains comparable to the original
// compact reference block instead of growing with sampling density.
constexpr float ReferenceHullSampleStrengthScale = 0.25f;

std::uint32_t ClampGridSize(const std::uint32_t requested) noexcept
{
    for (const std::uint32_t size : SupportedGridSizes)
    {
        if (requested <= size)
        {
            return size;
        }
    }
    return SupportedGridSizes.back();
}
} // namespace

bool LocalWaveSimulation::Configure(
    const OceanLocalWaveSettings& settings) noexcept
{
    const std::uint32_t gridSize = ClampGridSize(settings.gridSize);
    const float domainSize = std::isfinite(settings.domainSizeMeters)
        ? std::max(settings.domainSizeMeters, 1.0f)
        : 200.0f;
    if (gridSize == m_gridSize
        && std::abs(domainSize - m_domainSizeMeters) < 1.0e-5f
        && m_domainCenter.x == settings.domainCenter.x
        && m_domainCenter.y == settings.domainCenter.y)
    {
        return false;
    }
    m_gridSize = gridSize;
    m_domainSizeMeters = domainSize;
    m_domainCenter = settings.domainCenter;
    const std::size_t count = static_cast<std::size_t>(gridSize) * gridSize;
    m_height.assign(count, 0.0f);
    m_velocity.assign(count, 0.0f);
    m_nextHeight.assign(count, 0.0f);
    m_nextVelocity.assign(count, 0.0f);
    m_foam.assign(count, 0.0f);
    m_nextFoam.assign(count, 0.0f);
    m_pendingDisturbances.clear();
    ++m_stateVersion;
    return true;
}

void LocalWaveSimulation::Reset() noexcept
{
    std::fill(m_height.begin(), m_height.end(), 0.0f);
    std::fill(m_velocity.begin(), m_velocity.end(), 0.0f);
    std::fill(m_nextHeight.begin(), m_nextHeight.end(), 0.0f);
    std::fill(m_nextVelocity.begin(), m_nextVelocity.end(), 0.0f);
    std::fill(m_foam.begin(), m_foam.end(), 0.0f);
    std::fill(m_nextFoam.begin(), m_nextFoam.end(), 0.0f);
    m_pendingDisturbances.clear();
    m_lastFrameDisturbances.clear();
    ResetEmitters();
    ++m_stateVersion;
}

void LocalWaveSimulation::ResetEmitters() noexcept
{
    m_emitterTimeSeconds = 0.0f;
    m_rainAccumulator = 0.0f;
    m_emitterSeed = 0u;
    m_emitterSequence = 0u;
}

void LocalWaveSimulation::AddDisturbances(
    const std::span<const LocalWaveDisturbance> disturbances) noexcept
{
    for (const LocalWaveDisturbance& disturbance : disturbances)
    {
        if (std::isfinite(disturbance.position.x)
            && std::isfinite(disturbance.position.y)
            && std::isfinite(disturbance.radius)
            && std::isfinite(disturbance.strength)
            && disturbance.radius > 0.0f)
        {
            m_pendingDisturbances.push_back(disturbance);
        }
    }
}

void LocalWaveSimulation::Advance(
    float deltaSeconds,
    const OceanLocalWaveSettings& settings) noexcept
{
    (void)Configure(settings);
    AdvanceEmitters(deltaSeconds, settings);
    if (!settings.enabled || m_gridSize == 0u
        || !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f)
    {
        return;
    }
    for (const LocalWaveDisturbance& disturbance : m_lastFrameDisturbances)
    {
        Inject(disturbance);
    }
    constexpr float StableStep = 1.0f / 60.0f;
    constexpr std::uint32_t MaximumSubsteps = 8u;
    const float boundedDelta = std::min(deltaSeconds,
        StableStep * static_cast<float>(MaximumSubsteps));
    std::uint32_t substeps = static_cast<std::uint32_t>(
        std::ceil(boundedDelta / StableStep));
    substeps = std::clamp(substeps, 1u, MaximumSubsteps);
    const float step = boundedDelta / static_cast<float>(substeps);
    for (std::uint32_t index = 0u; index < substeps; ++index)
    {
        AdvanceSubstep(step, settings);
    }
}

void LocalWaveSimulation::AdvanceEmitters(
    const float deltaSeconds,
    const OceanLocalWaveSettings& settings) noexcept
{
    if (!settings.enabled || !std::isfinite(deltaSeconds)
        || deltaSeconds <= 0.0f)
    {
        m_pendingDisturbances.clear();
        m_lastFrameDisturbances.clear();
        return;
    }
    if (settings.demoEmittersEnabled)
    {
        AdvanceDemoEmitters(deltaSeconds, settings);
    }
    // Publish only after both external and deterministic demo disturbances
    // have been finalized. CPU validation and the GPU upload consume this
    // exact snapshot; only the CPU oracle proceeds to integrate its grids.
    m_lastFrameDisturbances = m_pendingDisturbances;
    m_pendingDisturbances.clear();
}

std::uint32_t LocalWaveSimulation::EmitterHash(
    const std::uint32_t value) noexcept
{
    std::uint32_t hash = value + 0x9e3779b9u;
    hash ^= hash >> 16u;
    hash *= 0x7feb352du;
    hash ^= hash >> 15u;
    hash *= 0x846ca68bu;
    return hash ^ (hash >> 16u);
}

void LocalWaveSimulation::AdvanceDemoEmitters(
    const float deltaSeconds,
    const OceanLocalWaveSettings& settings) noexcept
{
    const float safeDelta = std::clamp(
        std::isfinite(deltaSeconds) ? deltaSeconds : 0.0f,
        0.0f,
        0.25f);
    if (m_emitterSequence == 0u
        || m_emitterSeed != settings.emitterSeed)
    {
        ResetEmitters();
        m_emitterSeed = settings.emitterSeed;
    }
    m_emitterTimeSeconds += safeDelta;

    if (settings.rainEmitterEnabled)
    {
        m_rainAccumulator += safeDelta
            * std::max(settings.rainRatePerSecond, 0.0f);
        constexpr float RainInterval = 0.2f;
        while (m_rainAccumulator >= RainInterval)
        {
            m_rainAccumulator -= RainInterval;
            const std::uint32_t sequence =
                static_cast<std::uint32_t>(m_emitterSequence++);
            const std::uint32_t randomX = EmitterHash(
                m_emitterSeed ^ (sequence * 0x45d9f3bu));
            const std::uint32_t randomZ = EmitterHash(
                m_emitterSeed ^ (sequence * 0x119de1f3u));
            const float halfDomain =
                std::max(settings.domainSizeMeters, 1.0f) * 0.45f;
            const auto normalized = [](const std::uint32_t value)
            {
                return static_cast<float>(value & 0xffffu)
                    / 65535.0f * 2.0f - 1.0f;
            };
            m_pendingDisturbances.push_back({
                {normalized(randomX) * halfDomain,
                 normalized(randomZ) * halfDomain},
                std::max(settings.domainSizeMeters, 1.0f) * 0.012f,
                0.12f,
                {0.0f, 0.0f}});
        }
    }

    if (settings.wakeEmitterEnabled)
    {
        const float domain = std::max(settings.domainSizeMeters, 1.0f);
        const float gridSize = static_cast<float>(
            std::max(settings.gridSize, 1u));
        const float cellSize = domain / gridSize;
        const float requestedSpeed = std::max(
            settings.wakeSpeedMetersPerSecond, 0.0f);
        if (requestedSpeed <= 0.0001f)
        {
            return;
        }

        // The invisible hull loops from the foreground to the background and
        // back again. The major axis follows the reference camera's forward
        // direction (+X); the minor axis keeps both wake arms visible instead
        // of repeatedly crossing the same line.
        const float longitudinalRadius = domain
            * ReferenceOrbitLongitudinalRadiusFraction;
        const float lateralRadius = domain
            * ReferenceOrbitLateralRadiusFraction;
        const float meanRadius = std::max(
            0.5f * (longitudinalRadius + lateralRadius), cellSize);
        const float angularSpeed = requestedSpeed / meanRadius;
        const float phase = m_emitterTimeSeconds * angularSpeed;
        const float phaseSin = std::sin(phase);
        const float phaseCos = std::cos(phase);
        const DirectX::XMFLOAT2 boatPosition{
            settings.domainCenter.x - longitudinalRadius * phaseCos,
            settings.domainCenter.y + lateralRadius * phaseSin};
        const float velocityWorldX = longitudinalRadius
            * phaseSin * angularSpeed;
        const float velocityWorldZ = lateralRadius
            * phaseCos * angularSpeed;
        const float velocityLengthWorld = std::max(
            std::hypot(velocityWorldX, velocityWorldZ), 0.0001f);
        const DirectX::XMFLOAT2 direction{
            velocityWorldX / velocityLengthWorld,
            velocityWorldZ / velocityLengthWorld};
        const DirectX::XMFLOAT2 right{-direction.y, direction.x};
        const float velocityLengthGrid = velocityLengthWorld
            / std::max(cellSize, 0.001f);
        const float strength = 0.13f * velocityLengthGrid
            * safeDelta * ReferenceHullSampleStrengthScale;
        const float radius = std::max(cellSize * 0.75f, 0.20f);
        for (std::uint32_t row = 0u;
             row < ReferenceHullSamplesPerAxis; ++row)
        {
            const float longitudinalUnit =
                (static_cast<float>(row) + 0.5f)
                    / static_cast<float>(ReferenceHullSamplesPerAxis)
                    * 2.0f
                - 1.0f;
            const float hullOutline = std::sqrt(std::max(
                1.0f - longitudinalUnit * longitudinalUnit, 0.0f));
            const float halfWidthCells = std::lerp(
                ReferenceHullMinimumHalfWidthCells,
                ReferenceHullMaximumHalfWidthCells,
                hullOutline);
            const float longitudinalOffset = longitudinalUnit
                * ReferenceHullHalfLengthCells * cellSize;
            for (std::uint32_t column = 0u;
                 column < ReferenceHullSamplesPerAxis; ++column)
            {
                const float lateralUnit =
                    (static_cast<float>(column) + 0.5f)
                        / static_cast<float>(ReferenceHullSamplesPerAxis)
                        * 2.0f
                    - 1.0f;
                const float lateralOffset = lateralUnit
                    * halfWidthCells * cellSize;
                m_pendingDisturbances.push_back({
                    {boatPosition.x
                         + direction.x * longitudinalOffset
                         + right.x * lateralOffset,
                     boatPosition.y
                         + direction.y * longitudinalOffset
                         + right.y * lateralOffset},
                    radius,
                    strength,
                    direction});
            }
        }
        m_emitterSequence += ReferenceHullSamplesPerAxis
            * ReferenceHullSamplesPerAxis;
    }
}

void LocalWaveSimulation::AdvanceSubstep(
    const float deltaSeconds,
    const OceanLocalWaveSettings& settings) noexcept
{
    // Foam spatial dissipation is not water-velocity damping. Keep the wave
    // solver's numerical damping independent from the foam control.
    constexpr float damping = LocalWaveVelocityDamping;
    const float waveSpeed = 6.0f * std::max(settings.amplitudeMultiplier, 0.0f);
    for (std::uint32_t y = 0u; y < m_gridSize; ++y)
    {
        for (std::uint32_t x = 0u; x < m_gridSize; ++x)
        {
            const std::size_t index = Index(x, y);
            const float center = m_height[index];
            const float laplacian = Read(m_height, static_cast<int>(x) - 1,
                    static_cast<int>(y))
                + Read(m_height, static_cast<int>(x) + 1,
                    static_cast<int>(y))
                + Read(m_height, static_cast<int>(x),
                    static_cast<int>(y) - 1)
                + Read(m_height, static_cast<int>(x),
                    static_cast<int>(y) + 1)
                - 4.0f * center;
            float velocity = m_velocity[index]
                + laplacian * waveSpeed * deltaSeconds;
            velocity *= std::exp(-damping * deltaSeconds);
            float height = center
                + velocity * deltaSeconds
                    * std::max(settings.amplitudeMultiplier, 0.0f);
            const float edge = std::min({
                static_cast<float>(x),
                static_cast<float>(y),
                static_cast<float>(m_gridSize - 1u - x),
                static_cast<float>(m_gridSize - 1u - y)})
                / std::max(0.12f * static_cast<float>(m_gridSize), 1.0f);
            const float boundaryFade = std::clamp(edge, 0.0f, 1.0f);
            height *= boundaryFade;
            velocity *= boundaryFade;
            const float folding = std::max(-laplacian, 0.0f)
                * std::max(settings.lateralMultiplier, 0.0f) * 12.0f;
            const float crestHeight = std::clamp(
                (height - 0.01f) / 0.15f, 0.0f, 1.0f);
            const float risingCrest = std::clamp(
                (velocity + 0.02f) / 0.12f, 0.0f, 1.0f);
            const float generationCandidate = std::clamp(
                (folding - settings.foam.generationThreshold)
                    / std::max(1.0f - settings.foam.generationThreshold,
                        0.001f),
                0.0f,
                1.0f);
            const float generated = generationCandidate
                * crestHeight * risingCrest
                * std::max(settings.foam.generationAmount, 0.0f);
            const float neighborFoam = 0.25f * (
                Read(m_foam, static_cast<int>(x) - 1,
                    static_cast<int>(y))
                + Read(m_foam, static_cast<int>(x) + 1,
                    static_cast<int>(y))
                + Read(m_foam, static_cast<int>(x),
                    static_cast<int>(y) - 1)
                + Read(m_foam, static_cast<int>(x),
                    static_cast<int>(y) + 1));
            const float frameScale = deltaSeconds * ReferenceSimulationRate;
            const float spatialBlend = 1.0f - std::pow(
                std::max(1.0f
                        - std::clamp(settings.foam.dissipationSpeed,
                              0.0f, 1.0f)
                            * SpatialDissipationPerStep,
                    0.001f),
                frameScale);
            const float dissipated = std::lerp(
                m_foam[index], neighborFoam,
                std::clamp(spatialBlend, 0.0f, 1.0f));
            const float temporalRetention = std::pow(
                std::clamp(settings.foam.falloffSpeed, 0.0001f, 1.0f),
                deltaSeconds);
            m_nextFoam[index] = std::clamp(
                dissipated * temporalRetention
                    + generated * frameScale,
                0.0f,
                1.0f);
            m_nextHeight[index] = std::isfinite(height) ? height : 0.0f;
            m_nextVelocity[index] = std::isfinite(velocity)
                ? velocity : 0.0f;
        }
    }
    m_height.swap(m_nextHeight);
    m_velocity.swap(m_nextVelocity);
    m_foam.swap(m_nextFoam);
}

void LocalWaveSimulation::Inject(
    const LocalWaveDisturbance& disturbance) noexcept
{
    float gridX = 0.0f;
    float gridY = 0.0f;
    if (!WorldToGrid(disturbance.position.x, disturbance.position.y,
        gridX, gridY))
    {
        return;
    }
    const float cellSize = m_domainSizeMeters
        / static_cast<float>(std::max(m_gridSize - 1u, 1u));
    const int radiusCells = static_cast<int>(std::ceil(
        disturbance.radius / std::max(cellSize, 0.001f)));
    const int centerX = static_cast<int>(std::round(gridX));
    const int centerY = static_cast<int>(std::round(gridY));
    for (int y = centerY - radiusCells; y <= centerY + radiusCells; ++y)
    {
        for (int x = centerX - radiusCells; x <= centerX + radiusCells; ++x)
        {
            if (x < 0 || y < 0 || x >= static_cast<int>(m_gridSize)
                || y >= static_cast<int>(m_gridSize))
            {
                continue;
            }
            const float distance = std::hypot(
                (static_cast<float>(x) - gridX) * cellSize,
                (static_cast<float>(y) - gridY) * cellSize);
            const float normalizedRadius = distance
                / std::max(disturbance.radius, 0.001f);
            const float radiusSquared = normalizedRadius * normalizedRadius;
            // Match the positive disturbance amount consumed by the GPU and
            // by the WaveWorks reference sample. The wave equation creates
            // the following trough; pre-building a negative annulus causes
            // the dense moving-hull block to self-cancel.
            const float falloff = std::exp(-2.0f * radiusSquared);
            const std::size_t index = Index(
                static_cast<std::uint32_t>(x),
                static_cast<std::uint32_t>(y));
            m_height[index] += disturbance.strength * falloff;
            m_velocity[index] += disturbance.strength * falloff * 0.25f;
        }
    }
}

LocalWaveSample LocalWaveSimulation::Sample(
    const float worldX,
    const float worldZ) const noexcept
{
    LocalWaveSample sample{};
    float gridX = 0.0f;
    float gridY = 0.0f;
    if (!WorldToGrid(worldX, worldZ, gridX, gridY))
    {
        return sample;
    }
    const int x = static_cast<int>(std::round(gridX));
    const int y = static_cast<int>(std::round(gridY));
    sample.height = Read(m_height, x, y);
    sample.velocity = Read(m_velocity, x, y);
    const float cellSize = m_domainSizeMeters
        / static_cast<float>(std::max(m_gridSize - 1u, 1u));
    sample.gradient = {
        (Read(m_height, x + 1, y) - Read(m_height, x - 1, y))
            / std::max(2.0f * cellSize, 0.001f),
        (Read(m_height, x, y + 1) - Read(m_height, x, y - 1))
            / std::max(2.0f * cellSize, 0.001f)};
    sample.slopeMoments = {
        sample.gradient.x,
        sample.gradient.y,
        sample.gradient.x * sample.gradient.x,
        sample.gradient.y * sample.gradient.y};
    sample.folding = std::clamp(
        std::abs(sample.gradient.x) + std::abs(sample.gradient.y),
        0.0f,
        1.0f);
    sample.foam = Read(m_foam, x, y);
    return sample;
}

std::size_t LocalWaveSimulation::Index(
    const std::uint32_t x,
    const std::uint32_t y) const noexcept
{
    return static_cast<std::size_t>(y) * m_gridSize + x;
}

bool LocalWaveSimulation::WorldToGrid(
    const float worldX,
    const float worldZ,
    float& x,
    float& y) const noexcept
{
    if (m_gridSize == 0u || m_domainSizeMeters <= 0.0f)
    {
        return false;
    }
    const float half = m_domainSizeMeters * 0.5f;
    const float localX = worldX - m_domainCenter.x;
    const float localY = worldZ - m_domainCenter.y;
    if (localX < -half || localX > half || localY < -half || localY > half)
    {
        return false;
    }
    x = (localX / m_domainSizeMeters + 0.5f)
        * static_cast<float>(m_gridSize - 1u);
    y = (localY / m_domainSizeMeters + 0.5f)
        * static_cast<float>(m_gridSize - 1u);
    return true;
}

float LocalWaveSimulation::Read(
    const std::vector<float>& values,
    const int x,
    const int y) const noexcept
{
    if (values.empty())
    {
        return 0.0f;
    }
    const int clampedX = std::clamp(x, 0, static_cast<int>(m_gridSize) - 1);
    const int clampedY = std::clamp(y, 0, static_cast<int>(m_gridSize) - 1);
    return values[Index(
        static_cast<std::uint32_t>(clampedX),
        static_cast<std::uint32_t>(clampedY))];
}

float LocalWaveSimulation::AllocatedMegabytes() const noexcept
{
    const std::size_t bytes = (m_height.size() + m_velocity.size()
        + m_nextHeight.size() + m_nextVelocity.size() + m_foam.size()
        + m_nextFoam.size())
        * sizeof(float);
    return static_cast<float>(bytes) / (1024.0f * 1024.0f);
}
} // namespace Prism::Renderer
