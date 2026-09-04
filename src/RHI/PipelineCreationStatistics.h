#pragma once

#include <atomic>
#include <cstdint>

namespace Prism::RHI
{
struct PipelineCreationStatistics
{
    std::uint64_t graphics = 0;
    std::uint64_t compute = 0;
};

// Lifetime counters, not cache occupancy. Relaxed atomics also cover creation
// from independent recording contexts; they do not synchronize GPU work.
class PipelineCreationCounters
{
public:
    void RecordGraphics() noexcept { m_graphics.fetch_add(1, std::memory_order_relaxed); }
    void RecordCompute() noexcept { m_compute.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] PipelineCreationStatistics Read() const noexcept
    {
        return {m_graphics.load(std::memory_order_relaxed), m_compute.load(std::memory_order_relaxed)};
    }
private:
    std::atomic<std::uint64_t> m_graphics{0};
    std::atomic<std::uint64_t> m_compute{0};
};
} // namespace Prism::RHI
