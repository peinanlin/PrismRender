#pragma once

#include "Core/Profiling/FrameProfilerSnapshot.h"

#include <cstdint>
#include <filesystem>
#include <fstream>

#include <json.hpp>

namespace Prism::Core
{
// Opt-in streaming export. It keeps no frame history and never waits for GPU
// work; every record describes an already completed FrameProfiler snapshot.
class FrameProfilerReport
{
public:
    FrameProfilerReport(
        const std::filesystem::path& path,
        const nlohmann::json& metadata);

    void WriteCompleted(const FrameProfilerSnapshot& snapshot);
    void Finish(std::uint64_t expectedFrameCount);

private:
    void Write(const nlohmann::json& value);

    std::ofstream m_output;
    std::uint64_t m_completedFrameCount = 0;
    bool m_finished = false;
};
} // namespace Prism::Core
