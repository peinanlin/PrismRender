#pragma once

#include "RHI/D3D12/D3D12Context.h"

#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace Prism::RHI::D3D12
{
// Published tables are immutable; the last owner retires the range through the
// existing frame-fence allocator, including tables replaced during a frame.
class D3D12SamplerTable final
{
public:
    D3D12SamplerTable(D3D12Context& context, std::span<const D3D12_SAMPLER_DESC> descriptions);
    ~D3D12SamplerTable();
    D3D12SamplerTable(const D3D12SamplerTable&) = delete;
    D3D12SamplerTable& operator=(const D3D12SamplerTable&) = delete;

    bool Matches(const D3D12Context& context, std::span<const D3D12_SAMPLER_DESC> descriptions) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle() const;

private:
    D3D12Context& m_context;
    std::vector<D3D12_SAMPLER_DESC> m_descriptions;
    D3D12Context::ShaderVisibleDescriptor m_range{};
};

// One cache per layout. Weak entries do not prolong GPU resource lifetimes.
class D3D12SamplerTableCache final
{
public:
    std::shared_ptr<const D3D12SamplerTable> Acquire(
        D3D12Context& context, std::span<const D3D12_SAMPLER_DESC> descriptions);

private:
    std::mutex m_mutex;
    std::vector<std::weak_ptr<const D3D12SamplerTable>> m_tables;
};
} // namespace Prism::RHI::D3D12
