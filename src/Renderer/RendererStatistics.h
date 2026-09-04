#pragma once

#include "Renderer/Features/Fluid/FluidStatistics.h"
#include "Renderer/Features/Ocean/OceanStatistics.h"

#include <cstdint>

namespace Prism::Renderer
{
struct RendererStatistics
{
    FluidStatistics fluid{};
    OceanStatistics ocean{};
    std::uint32_t drawCalls = 0;
    std::uint32_t shadowDrawCalls = 0;
    std::uint32_t directionalShadowDrawCalls = 0;
    std::uint32_t localShadowDrawCalls = 0;
    std::uint32_t shadowLayersRendered = 0;
    std::uint32_t shadowLayersCached = 0;
    std::uint32_t shadowCasterCandidates = 0;
    std::uint32_t shadowCastersCulled = 0;
    std::uint32_t shadowInstancedDrawCalls = 0;
    std::uint32_t forwardDrawCalls = 0;
    std::uint32_t postProcessDrawCalls = 0;
    std::uint32_t renderedObjects = 0;
    std::uint32_t visibleObjects = 0;
    std::uint32_t culledObjects = 0;
    std::uint32_t instancedDrawCalls = 0;
    std::uint32_t instancedObjects = 0;
    std::uint32_t instanceBatchCount = 0;
    std::uint32_t gpuDrivenCandidateObjects = 0;
    std::uint32_t indirectDrawCalls = 0;
    std::uint32_t gpuVisibleObjects = 0;
    std::uint32_t gpuLodRejectedObjects = 0;
    std::uint32_t gpuFrustumCulledObjects = 0;
    std::uint32_t gpuOcclusionCulledObjects = 0;
    std::uint32_t gpuDisabledObjects = 0;
    bool gpuInstancingSupported = false;
    bool gpuDrivenSupported = false;
    bool gpuVisibilityStatisticsValid = false;
    std::uint32_t virtualTextureResidentPages = 0;
    std::uint32_t virtualTextureRequestedPages = 0;
    std::uint32_t virtualTexturePageHits = 0;
    std::uint32_t virtualTexturePageMisses = 0;
    std::uint32_t virtualTextureEvictions = 0;
    std::uint32_t cachedPsoCount = 0;
    float cullingCpuMs = 0.0f;
    float shadowPassCpuMs = 0.0f;
    float forwardPassCpuMs = 0.0f;
    float bloomPassCpuMs = 0.0f;
    float tonemapPassCpuMs = 0.0f;
    float renderGraphBuildCpuMs = 0.0f;
    float renderGraphExecuteCpuMs = 0.0f;
    float totalRendererCpuMs = 0.0f;
    float shadowPassGpuMs = 0.0f;
    float geometryPassGpuMs = 0.0f;
    float bloomPassGpuMs = 0.0f;
    float tonemapPassGpuMs = 0.0f;
    float totalRendererGpuMs = 0.0f;
};
} // namespace Prism::Renderer
