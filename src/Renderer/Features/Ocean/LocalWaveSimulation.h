#pragma once

#include "Renderer/Features/Ocean/OceanSettings.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <vector>

namespace Prism::Renderer
{
struct LocalWaveDisturbance
{
    DirectX::XMFLOAT2 position{};
    float radius = 1.0f;
    float strength = 0.0f;
    DirectX::XMFLOAT2 velocityDirection{};
};

struct LocalWaveSample
{
    float height = 0.0f;
    float velocity = 0.0f;
    DirectX::XMFLOAT2 gradient{};
    DirectX::XMFLOAT4 slopeMoments{};
    float folding = 0.0f;
    float foam = 0.0f;
};

// Deterministic CPU oracle and bounded fallback for the interactive wave
// layer. The GPU implementation can consume the same settings/disturbance
// contract without changing callers or the sampling convention.
class LocalWaveSimulation
{
public:
    [[nodiscard]] bool Configure(
        const OceanLocalWaveSettings& settings) noexcept;
    void Reset() noexcept;
    void AddDisturbances(
        std::span<const LocalWaveDisturbance> disturbances) noexcept;
    void Advance(
        float deltaSeconds,
        const OceanLocalWaveSettings& settings) noexcept;
    // GPU production only needs the finalized disturbance batch. This keeps
    // the deterministic emitter contract without redundantly advancing the
    // CPU oracle's full height/velocity/foam grids.
    void AdvanceEmitters(
        float deltaSeconds,
        const OceanLocalWaveSettings& settings) noexcept;
    [[nodiscard]] LocalWaveSample Sample(
        float worldX,
        float worldZ) const noexcept;
    [[nodiscard]] std::span<const LocalWaveDisturbance>
        LastFrameDisturbances() const noexcept
    {
        return m_lastFrameDisturbances;
    }

    [[nodiscard]] std::uint32_t GridSize() const noexcept
    {
        return m_gridSize;
    }
    [[nodiscard]] float DomainSizeMeters() const noexcept
    {
        return m_domainSizeMeters;
    }
    [[nodiscard]] float AllocatedMegabytes() const noexcept;
    [[nodiscard]] std::uint64_t StateVersion() const noexcept
    {
        return m_stateVersion;
    }
    void ResetEmitters() noexcept;
    [[nodiscard]] std::uint64_t EmitterSequence() const noexcept
    {
        return m_emitterSequence;
    }

private:
    [[nodiscard]] std::size_t Index(
        std::uint32_t x,
        std::uint32_t y) const noexcept;
    [[nodiscard]] bool WorldToGrid(
        float worldX,
        float worldZ,
        float& x,
        float& y) const noexcept;
    [[nodiscard]] float Read(
        const std::vector<float>& values,
        int x,
        int y) const noexcept;
    void Inject(
        const LocalWaveDisturbance& disturbance) noexcept;
    void AdvanceDemoEmitters(
        float deltaSeconds,
        const OceanLocalWaveSettings& settings) noexcept;
    [[nodiscard]] static std::uint32_t EmitterHash(
        std::uint32_t value) noexcept;
    void AdvanceSubstep(
        float deltaSeconds,
        const OceanLocalWaveSettings& settings) noexcept;

    std::uint32_t m_gridSize = 0u;
    float m_domainSizeMeters = 0.0f;
    DirectX::XMFLOAT2 m_domainCenter{};
    std::vector<float> m_height;
    std::vector<float> m_velocity;
    std::vector<float> m_nextHeight;
    std::vector<float> m_nextVelocity;
    std::vector<float> m_foam;
    std::vector<float> m_nextFoam;
    std::vector<LocalWaveDisturbance> m_pendingDisturbances;
    std::vector<LocalWaveDisturbance> m_lastFrameDisturbances;
    float m_emitterTimeSeconds = 0.0f;
    float m_rainAccumulator = 0.0f;
    std::uint32_t m_emitterSeed = 0u;
    std::uint64_t m_emitterSequence = 0u;
    std::uint64_t m_stateVersion = 0u;
};
} // namespace Prism::Renderer
