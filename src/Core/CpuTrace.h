#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Prism::Core
{
    class CpuTrace
    {
    public:
        static void InitializeFromEnvironment();
        static void Initialize(
            const std::filesystem::path& outputPath,
            std::string_view correlationId);
        [[nodiscard]] static bool IsEnabled();
        [[nodiscard]] static std::uint64_t BeginSpan(
            std::string_view name,
            std::string_view category);
        static void EndSpan(std::uint64_t spanId);
        static bool Flush(
            bool success,
            int exitCode,
            std::string* outError = nullptr);
    };

    class CpuTraceSpan
    {
    public:
        CpuTraceSpan(
            std::string_view name,
            std::string_view category);
        ~CpuTraceSpan();

        CpuTraceSpan(const CpuTraceSpan&) = delete;
        CpuTraceSpan& operator=(const CpuTraceSpan&) = delete;
        CpuTraceSpan(CpuTraceSpan&& other) noexcept;
        CpuTraceSpan& operator=(CpuTraceSpan&& other) noexcept;

        void End();

    private:
        std::uint64_t m_spanId = 0;
    };
} // namespace Prism::Core
