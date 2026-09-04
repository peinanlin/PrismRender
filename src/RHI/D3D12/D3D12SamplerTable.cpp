#include "RHI/D3D12/D3D12SamplerTable.h"

#include "Core/Assert.h"

#include <algorithm>

namespace Prism::RHI::D3D12
{
namespace
{
bool EqualSampler(const D3D12_SAMPLER_DESC& a, const D3D12_SAMPLER_DESC& b)
{
    // Compare fields, not struct padding. All fields affecting sampling belong
    // to the key, even when a particular filter/address mode ignores one.
    return a.Filter == b.Filter && a.AddressU == b.AddressU
        && a.AddressV == b.AddressV && a.AddressW == b.AddressW
        && a.MipLODBias == b.MipLODBias && a.MaxAnisotropy == b.MaxAnisotropy
        && a.ComparisonFunc == b.ComparisonFunc && a.MinLOD == b.MinLOD
        && a.MaxLOD == b.MaxLOD
        && std::equal(std::begin(a.BorderColor), std::end(a.BorderColor), std::begin(b.BorderColor));
}
}

D3D12SamplerTable::D3D12SamplerTable(
    D3D12Context& context, std::span<const D3D12_SAMPLER_DESC> descriptions)
    : m_context(context), m_descriptions(descriptions.begin(), descriptions.end())
{
    Core::Check(!descriptions.empty(), "Cannot allocate an empty D3D12 sampler table.");
    m_range = context.AllocateShaderVisibleSamplerRange(static_cast<std::uint32_t>(descriptions.size()));
    auto handle = m_range.cpuHandle;
    const auto stride = context.GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    for (const auto& description : descriptions)
    {
        context.GetDevice()->CreateSampler(&description, handle);
        handle.ptr += stride;
    }
}

D3D12SamplerTable::~D3D12SamplerTable()
{
    m_context.RetireShaderVisibleSamplerRange(m_range.index, static_cast<std::uint32_t>(m_descriptions.size()));
}

bool D3D12SamplerTable::Matches(
    const D3D12Context& context, std::span<const D3D12_SAMPLER_DESC> descriptions) const
{
    return &context == &m_context && descriptions.size() == m_descriptions.size()
        && std::equal(descriptions.begin(), descriptions.end(), m_descriptions.begin(), EqualSampler);
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12SamplerTable::GetGpuHandle() const { return m_range.gpuHandle; }

std::shared_ptr<const D3D12SamplerTable> D3D12SamplerTableCache::Acquire(
    D3D12Context& context, std::span<const D3D12_SAMPLER_DESC> descriptions)
{
    std::lock_guard lock(m_mutex);
    std::shared_ptr<const D3D12SamplerTable> match;
    std::erase_if(m_tables, [&](const auto& entry)
    {
        const auto table = entry.lock();
        if (!table) return true;
        if (!match && table->Matches(context, descriptions)) match = table;
        return false;
    });
    if (match) return match;
    auto table = std::make_shared<const D3D12SamplerTable>(context, descriptions);
    m_tables.push_back(table);
    return table;
}
} // namespace Prism::RHI::D3D12
