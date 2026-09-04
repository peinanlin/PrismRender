#pragma once

#include "Core/Environment.h"

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace Prism::Renderer
{
enum class RenderCaptureStage
{
    Tonemap,
    Shadow,
    GBuffer0,
    GBuffer1,
    GBuffer2,
    GBuffer3,
    WaterDepth,
    WaterMask,
    WaterGBuffer0,
    WaterGBuffer1,
    WaterGBuffer2,
    WaterRefraction,
    WaterComposite,
    Hdr,
    Bloom
};

inline std::string ReadRenderEnvironmentVariable(const char* name)
{
    return Core::ReadEnvironmentVariableValue(name);
}

inline RenderCaptureStage ReadRenderCaptureStage()
{
    const std::string value = ReadRenderEnvironmentVariable("PRISM_RENDER_CAPTURE_STAGE");
    if (value == "shadow") return RenderCaptureStage::Shadow;
    if (value == "gbuffer0") return RenderCaptureStage::GBuffer0;
    if (value == "gbuffer1") return RenderCaptureStage::GBuffer1;
    if (value == "gbuffer2") return RenderCaptureStage::GBuffer2;
    if (value == "gbuffer3") return RenderCaptureStage::GBuffer3;
    if (value == "water-depth") return RenderCaptureStage::WaterDepth;
    if (value == "water-mask") return RenderCaptureStage::WaterMask;
    if (value == "water-gbuffer0") return RenderCaptureStage::WaterGBuffer0;
    if (value == "water-gbuffer1") return RenderCaptureStage::WaterGBuffer1;
    if (value == "water-gbuffer2") return RenderCaptureStage::WaterGBuffer2;
    if (value == "water-refraction") return RenderCaptureStage::WaterRefraction;
    if (value == "water-composite") return RenderCaptureStage::WaterComposite;
    if (value == "hdr") return RenderCaptureStage::Hdr;
    if (value == "bloom") return RenderCaptureStage::Bloom;
    return RenderCaptureStage::Tonemap;
}

inline bool IsDeterministicRenderCaptureEnabled()
{
    return ReadRenderEnvironmentVariable("PRISM_RENDER_DETERMINISTIC") == "1";
}

inline std::uint32_t ReadRenderWindowDimension(
    const char* name,
    const std::uint32_t fallback)
{
    const std::string value = ReadRenderEnvironmentVariable(name);
    if (value.empty())
    {
        return fallback;
    }

    std::uint32_t dimension = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), dimension);
    if (error != std::errc{} || end != value.data() + value.size()
        || dimension < 64 || dimension > 8192)
    {
        throw std::invalid_argument(
            std::string(name) + " must be an integer between 64 and 8192.");
    }
    return dimension;
}

inline std::uint32_t ReadAsyncComputeWorkloadMultiplier()
{
    const std::string value = ReadRenderEnvironmentVariable(
        "PRISM_RENDER_ASYNC_WORKLOAD_MULTIPLIER");
    if (value.empty())
    {
        return 1;
    }

    std::uint32_t multiplier = 0;
    const auto [end, error] = std::from_chars(
        value.data(),
        value.data() + value.size(),
        multiplier);
    if (error != std::errc{}
        || end != value.data() + value.size()
        || multiplier < 1
        || multiplier > 64)
    {
        throw std::invalid_argument(
            "PRISM_RENDER_ASYNC_WORKLOAD_MULTIPLIER must be an integer between 1 and 64.");
    }
    return multiplier;
}

inline bool IsGBufferCaptureStage(const RenderCaptureStage stage)
{
    return stage >= RenderCaptureStage::GBuffer0 && stage <= RenderCaptureStage::GBuffer3;
}

inline bool IsWaterCaptureStage(const RenderCaptureStage stage)
{
    return stage >= RenderCaptureStage::WaterDepth
        && stage <= RenderCaptureStage::WaterComposite;
}

inline bool IsLinearDataCaptureStage(const RenderCaptureStage stage)
{
    return IsGBufferCaptureStage(stage)
        || (stage >= RenderCaptureStage::WaterDepth
            && stage <= RenderCaptureStage::WaterRefraction);
}
} // namespace Prism::Renderer
