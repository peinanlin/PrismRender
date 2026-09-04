#include "Engine/EntityId.h"

#include <array>
#include <charconv>
#include <iomanip>
#include <sstream>

namespace Prism::Engine
{
namespace
{
std::uint64_t Mix64(std::uint64_t value)
{
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

bool ParseHex(const std::string_view text, std::uint64_t& value)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return error == std::errc{} && end == text.data() + text.size();
}
} // namespace

bool EntityId::IsValid() const
{
    return high != 0 || low != 0;
}

std::string EntityId::ToString() const
{
    std::ostringstream output;
    output << std::hex << std::setfill('0')
           << std::setw(8) << static_cast<std::uint32_t>(high >> 32u) << '-'
           << std::setw(4) << static_cast<std::uint16_t>(high >> 16u) << '-'
           << std::setw(4) << static_cast<std::uint16_t>(high) << '-'
           << std::setw(4) << static_cast<std::uint16_t>(low >> 48u) << '-'
           << std::setw(12) << (low & 0x0000ffffffffffffull);
    return output.str();
}

std::optional<EntityId> EntityId::Parse(const std::string_view text)
{
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-')
    {
        return std::nullopt;
    }

    std::array<char, 32> compact{};
    std::size_t outputIndex = 0;
    for (const char character : text)
    {
        if (character != '-')
        {
            compact[outputIndex++] = character;
        }
    }

    EntityId result{};
    if (!ParseHex(std::string_view(compact.data(), 16), result.high)
        || !ParseHex(std::string_view(compact.data() + 16, 16), result.low))
    {
        return std::nullopt;
    }
    return result.IsValid() ? std::optional<EntityId>(result) : std::nullopt;
}

EntityIdGenerator::EntityIdGenerator(const std::uint64_t seed)
    : m_seed(seed)
{
}

EntityId EntityIdGenerator::Generate()
{
    ++m_counter;
    EntityId result{Mix64(m_seed ^ m_counter), Mix64(m_seed + m_counter)};
    result.high = (result.high & 0xffffffffffff0fffull) | 0x0000000000004000ull;
    result.low = (result.low & 0x3fffffffffffffffull) | 0x8000000000000000ull;
    return result;
}

void EntityIdGenerator::Reset(const std::uint64_t seed, const std::uint64_t counter)
{
    m_seed = seed;
    m_counter = counter;
}

std::uint64_t EntityIdGenerator::GetSeed() const { return m_seed; }
std::uint64_t EntityIdGenerator::GetCounter() const { return m_counter; }
} // namespace Prism::Engine
