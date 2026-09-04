#pragma once

#include "Engine/EntityId.h"

#include <json.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace Prism::Asset
{
class AssetDatabase;
}

namespace Prism::Engine
{
class CommandProcessor;
struct ComponentDescriptor;
}

namespace Prism::UI
{
struct PropertyGridActions
{
    Engine::CommandProcessor* processor = nullptr;
    const Asset::AssetDatabase* assets = nullptr;
    std::function<void()> worldChanged;
};

class PropertyGrid
{
public:
    bool DrawComponent(
        Engine::EntityId entity,
        const Engine::ComponentDescriptor& descriptor,
        const PropertyGridActions& actions);
    [[nodiscard]] const std::string& GetStatus() const;

private:
    bool Submit(
        Engine::EntityId entity,
        const std::string& component,
        const std::string& property,
        const nlohmann::json& value,
        const PropertyGridActions& actions);
    void BeginTransaction(const PropertyGridActions& actions);
    void CommitTransaction(const PropertyGridActions& actions);
    std::string NextRequestId(const char* operation);

    bool m_transactionActive = false;
    std::uint64_t m_requestCounter = 0;
    std::string m_status;
};
}
