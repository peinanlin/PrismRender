#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Prism::Engine
{
struct EntityId
{
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    [[nodiscard]] bool IsValid() const;
    [[nodiscard]] std::string ToString() const;
    static std::optional<EntityId> Parse(std::string_view text);

    auto operator<=>(const EntityId&) const = default;
};

class EntityIdGenerator
{
public:
    explicit EntityIdGenerator(std::uint64_t seed = 0x505249534d454e47ull);

    [[nodiscard]] EntityId Generate();
    void Reset(std::uint64_t seed, std::uint64_t counter = 0);
    [[nodiscard]] std::uint64_t GetSeed() const;
    [[nodiscard]] std::uint64_t GetCounter() const;

private:
    std::uint64_t m_seed = 0;
    std::uint64_t m_counter = 0;
};
} // namespace Prism::Engine
