#pragma once

#include "RHI/GraphicsApi.h"

#include <cstdint>

namespace Prism::Renderer
{
struct OceanStatistics
{
    RHI::GraphicsApi graphicsApi = RHI::GraphicsApi::Direct3D12;
    float spectrumMilliseconds = 0.0f;
    float fftMilliseconds = 0.0f;
    float mapMilliseconds = 0.0f;
    float foamMilliseconds = 0.0f;
    float mipMilliseconds = 0.0f;
    float localWaveMilliseconds = 0.0f;
    float surfaceMilliseconds = 0.0f;
    float waterVisibilityMilliseconds = 0.0f;
    float waterRefractionMilliseconds = 0.0f;
    float waterCompositeMilliseconds = 0.0f;
    float waterCausticsMilliseconds = 0.0f;
    float waterVolumetricsMilliseconds = 0.0f;
    float waterVolumetricReconstructionMilliseconds = 0.0f;
    float rendererGpuMilliseconds = 0.0f;
    float geometryMilliseconds = 0.0f;
    float gpuTotalMilliseconds = 0.0f;
    float cpuTotalMilliseconds = 0.0f;
    float renderLatencyFrames = 0.0f;
    float readbackLatencyFrames = 0.0f;
    float allocatedMegabytes = 0.0f;
    float waterAllocatedMegabytes = 0.0f;
    float waterPixelCoverage = 0.0f;
    float conservativeMaxDisplacement = 0.0f;
    std::uint32_t dispatchCount = 0u;
    std::uint32_t cascadeResolution = 0u;
    std::uint32_t localWaveGridSize = 0u;
    std::uint32_t outputWidth = 0u;
    std::uint32_t outputHeight = 0u;
    std::uint32_t activeRenderedViews = 0u;
    std::uint32_t waterVisibilityDrawCount = 0u;
    std::uint32_t waterOpticsDispatchCount = 0u;
    std::uint32_t waterRefractionEffectiveSamples = 0u;
    std::uint32_t waterRefractionWidth = 0u;
    std::uint32_t waterRefractionHeight = 0u;
    std::uint32_t waterVolumetricWidth = 0u;
    std::uint32_t waterVolumetricHeight = 0u;
    std::uint32_t waterOpticsQuality = 0u;
    std::uint64_t waterMediumVersion = 0u;
    bool waterCameraUnderwater = false;
    bool waterMediumFallback = true;
    bool waterVolumetricHistoryValid = false;
    bool waterVolumetricsActive = false;
    bool waterCoverageAvailable = false;
    std::uint32_t quality = 0u;
    std::uint64_t localWaveResourceGeneration = 0u;
    float localWaveAllocatedMegabytes = 0.0f;
    std::uint32_t visibleGeometryNodes = 0u;
    std::uint32_t geometryNodes = 0u;
    std::uint32_t geometryInstanceCount = 0u;
    std::uint32_t geometryRefinementIterations = 0u;
    std::uint32_t geometryMaxLod = 0u;
    float geometryMaxTessellationFactor = 1.0f;
    std::uint64_t publishedVersion = 0u;
    std::uint64_t requestedSettingsVersion = 0u;
    std::uint64_t activeSettingsVersion = 0u;
    std::uint64_t waterOpticsHistoryVersion = 0u;
    std::uint64_t waterOpticsSurfaceHistoryVersion = 0u;
    std::uint32_t pendingDirtyScopes = 0u;
    std::uint32_t queryPending = 0u;
    std::uint32_t queryCompleted = 0u;
    std::uint64_t queryExpired = 0u;
    std::uint64_t queryGpuBatches = 0u;
    std::uint64_t queryReadbackCopies = 0u;
    bool queryResourceAllocated = false;
    bool gpuTimersAvailable = false;
    bool cpuTimersAvailable = false;
    bool tessellationActive = false;
    bool localWaveSharedTime = true;
    bool localWaveEmitterActive = false;
    bool localWaveSimulationActive = false;
    bool waterOpticsActive = false;
    bool waterOpticsHistoryValid = false;
    bool waterRefractionRayMarchActive = false;
};
} // namespace Prism::Renderer
