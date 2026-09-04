#pragma once

#include "Engine/World.h"

#include <filesystem>
#include <string>

#include <json.hpp>

namespace Prism::Engine
{
class WorldSerializer
{
public:
    static constexpr std::uint32_t CurrentVersion = 2;

    [[nodiscard]] static nlohmann::json Serialize(const World& world);
    static void Deserialize(const nlohmann::json& document, World& world);
    static bool Save(
        const std::filesystem::path& path,
        const World& world,
        std::string* outError = nullptr);
    static bool Load(
        const std::filesystem::path& path,
        World& world,
        std::string* outError = nullptr);
    [[nodiscard]] static std::string ComputeHash(const nlohmann::json& document);
    [[nodiscard]] static std::string ComputeHash(const World& world);
};
} // namespace Prism::Engine
