#include "UI/PropertyGrid.h"

#include "Asset/AssetDatabase.h"
#include "Engine/CommandSystem.h"
#include "Engine/Reflection.h"
#include "Engine/World.h"
#include "UI/ContentBrowserPanel.h"

#include <DirectXMath.h>
#include <imgui.h>

#include <array>
#include <cstring>
#include <optional>

namespace Prism::UI
{
namespace
{
using json = nlohmann::json;

std::string DisplayName(const Engine::PropertyDescriptor& property)
{
    return property.displayName.empty()
        ? property.name
        : property.displayName;
}

std::optional<Asset::AssetType> AcceptedAssetType(
    const Engine::PropertyDescriptor& property)
{
    return Asset::AssetDatabase::ParseAssetType(
        property.acceptedAssetType);
}
}

bool PropertyGrid::DrawComponent(
    const Engine::EntityId entity,
    const Engine::ComponentDescriptor& descriptor,
    const PropertyGridActions& actions)
{
    if (actions.processor == nullptr)
    {
        return false;
    }
    json values = actions.processor->GetComponentProperties(
        entity, descriptor.name);
    if (!values.is_object())
    {
        return false;
    }

    bool anyChanged = false;
    std::string currentCategory;
    for (const Engine::PropertyDescriptor& property : descriptor.properties)
    {
        if (!values.contains(property.name)) continue;
        if (!property.category.empty()
            && property.category != currentCategory)
        {
            currentCategory = property.category;
            ImGui::SeparatorText(currentCategory.c_str());
        }

        ImGui::PushID(property.name.c_str());
        bool changed = false;
        bool continuous = false;
        json updated = values.at(property.name);
        const std::string label = DisplayName(property);

        switch (property.type)
        {
        case Engine::PropertyType::String:
        {
            std::array<char, 256> buffer{};
            const std::string value = updated.get<std::string>();
            strncpy_s(
                buffer.data(),
                buffer.size(),
                value.c_str(),
                _TRUNCATE);
            changed = ImGui::InputText(label.c_str(), buffer.data(), buffer.size());
            continuous = true;
            if (changed) updated = std::string(buffer.data());
            break;
        }
        case Engine::PropertyType::Float:
        {
            float value = updated.get<float>();
            if (property.editorHint == Engine::PropertyEditorHint::AngleRadians)
            {
                value = DirectX::XMConvertToDegrees(value);
                changed = ImGui::DragFloat(
                    label.c_str(), &value, property.step,
                    property.hasRange
                        ? DirectX::XMConvertToDegrees(property.minimum) : 0.0f,
                    property.hasRange
                        ? DirectX::XMConvertToDegrees(property.maximum) : 0.0f,
                    "%.2f deg");
                if (changed) updated = DirectX::XMConvertToRadians(value);
            }
            else
            {
                changed = ImGui::DragFloat(
                    label.c_str(), &value, property.step,
                    property.hasRange ? property.minimum : 0.0f,
                    property.hasRange ? property.maximum : 0.0f);
                if (changed) updated = value;
            }
            continuous = true;
            break;
        }
        case Engine::PropertyType::Float3:
        {
            float value[3] = {
                updated.at(0).get<float>(), updated.at(1).get<float>(),
                updated.at(2).get<float>()};
            if (property.editorHint == Engine::PropertyEditorHint::Color)
            {
                changed = ImGui::ColorEdit3(label.c_str(), value);
            }
            else if (property.editorHint == Engine::PropertyEditorHint::AngleRadians)
            {
                for (float& component : value)
                    component = DirectX::XMConvertToDegrees(component);
                changed = ImGui::DragFloat3(
                    label.c_str(), value, property.step, 0.0f, 0.0f, "%.2f deg");
                if (changed)
                {
                    for (float& component : value)
                        component = DirectX::XMConvertToRadians(component);
                }
            }
            else
            {
                changed = ImGui::DragFloat3(
                    label.c_str(), value, property.step,
                    property.hasRange ? property.minimum : 0.0f,
                    property.hasRange ? property.maximum : 0.0f);
            }
            continuous = true;
            if (changed) updated = json::array({value[0], value[1], value[2]});
            break;
        }
        case Engine::PropertyType::Double3:
        {
            double value[3] = {
                updated.at(0).get<double>(),
                updated.at(1).get<double>(),
                updated.at(2).get<double>()};
            const double speed =
                static_cast<double>(property.step);
            changed = ImGui::DragScalarN(
                label.c_str(),
                ImGuiDataType_Double,
                value,
                3,
                static_cast<float>(speed),
                nullptr,
                nullptr,
                "%.6f");
            continuous = true;
            if (changed)
            {
                updated = json::array({
                    value[0],
                    value[1],
                    value[2]});
            }
            break;
        }
        case Engine::PropertyType::Float4:
        {
            float value[4] = {
                updated.at(0).get<float>(), updated.at(1).get<float>(),
                updated.at(2).get<float>(), updated.at(3).get<float>()};
            changed = property.editorHint == Engine::PropertyEditorHint::Color
                ? ImGui::ColorEdit4(label.c_str(), value)
                : ImGui::DragFloat4(label.c_str(), value, property.step);
            continuous = true;
            if (changed)
                updated = json::array({value[0], value[1], value[2], value[3]});
            break;
        }
        case Engine::PropertyType::Boolean:
        {
            bool value = updated.get<bool>();
            changed = ImGui::Checkbox(label.c_str(), &value);
            if (changed) updated = value;
            break;
        }
        case Engine::PropertyType::EntityReference:
        {
            const std::string current = updated.is_null()
                ? std::string{} : updated.get<std::string>();
            const std::optional<Engine::EntityId> parsed =
                Engine::EntityId::Parse(current);
            const Engine::EntityRecord* currentEntity = parsed.has_value()
                ? actions.processor->GetWorld().FindEntity(*parsed) : nullptr;
            const char* preview = currentEntity != nullptr
                ? currentEntity->name.value.c_str() : "None";
            if (ImGui::BeginCombo(label.c_str(), preview))
            {
                if (ImGui::Selectable("None", current.empty()))
                {
                    updated = nullptr;
                    changed = true;
                }
                for (const auto& [candidateId, candidate] :
                     actions.processor->GetWorld().GetEntities())
                {
                    if (candidateId == entity) continue;
                    if (ImGui::Selectable(
                            candidate.name.value.c_str(),
                            candidateId.ToString() == current))
                    {
                        updated = candidateId.ToString();
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case Engine::PropertyType::AssetPath:
        {
            const std::string current = updated.get<std::string>();
            std::string preview = current.empty() ? "None" : current;
            const std::optional<Asset::AssetType> accepted =
                AcceptedAssetType(property);
            if (actions.assets != nullptr)
            {
                if (const Asset::AssetRecord* record =
                        actions.assets->FindByPath(current))
                    preview = record->name;
            }
            if (ImGui::BeginCombo(label.c_str(), preview.c_str()))
            {
                if (ImGui::Selectable("None", current.empty()))
                {
                    updated = std::string{};
                    changed = true;
                }
                if (actions.assets != nullptr)
                {
                    for (const Asset::AssetRecord& record :
                         actions.assets->List(accepted))
                    {
                        if (ImGui::Selectable(record.name.c_str(), record.assetPath == current))
                        {
                            updated = record.assetPath;
                            changed = true;
                        }
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetDragPayload::TypeName))
                {
                    const auto* asset = static_cast<const AssetDragPayload*>(payload->Data);
                    const Asset::AssetType type = static_cast<Asset::AssetType>(asset->assetType);
                    if (!accepted.has_value() || *accepted == type)
                    {
                        updated = std::string(asset->assetPath);
                        changed = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            break;
        }
        }

        if (continuous && ImGui::IsItemActivated()) BeginTransaction(actions);
        if (changed)
        {
            anyChanged |= Submit(
                entity, descriptor.name, property.name, updated, actions);
        }
        if (continuous && ImGui::IsItemDeactivatedAfterEdit()) CommitTransaction(actions);
        ImGui::PopID();
    }
    return anyChanged;
}

const std::string& PropertyGrid::GetStatus() const { return m_status; }

bool PropertyGrid::Submit(
    const Engine::EntityId entity,
    const std::string& component,
    const std::string& property,
    const nlohmann::json& value,
    const PropertyGridActions& actions)
{
    const nlohmann::json result = actions.processor->Execute({
        {"requestId", NextRequestId("property")},
        {"command", "component.set"},
        {"arguments", {{"entity", entity.ToString()},
                       {"component", component},
                       {"properties", {{property, value}}}}}});
    if (!result.value("success", false))
    {
        m_status = result.at("error").value(
            "message", std::string("Property update failed."));
        return false;
    }
    m_status.clear();
    if (actions.worldChanged) actions.worldChanged();
    return true;
}

void PropertyGrid::BeginTransaction(const PropertyGridActions& actions)
{
    if (m_transactionActive || actions.processor == nullptr) return;
    const nlohmann::json result = actions.processor->Execute({
        {"requestId", NextRequestId("begin")},
        {"command", "transaction.begin"},
        {"arguments", nlohmann::json::object()}});
    m_transactionActive = result.value("success", false);
}

void PropertyGrid::CommitTransaction(const PropertyGridActions& actions)
{
    if (!m_transactionActive || actions.processor == nullptr) return;
    const nlohmann::json result = actions.processor->Execute({
        {"requestId", NextRequestId("commit")},
        {"command", "transaction.commit"},
        {"arguments", nlohmann::json::object()}});
    m_transactionActive = false;
    if (!result.value("success", false))
        m_status = result.at("error").value(
            "message", std::string("Could not commit property edit."));
}

std::string PropertyGrid::NextRequestId(const char* operation)
{
    return "editor-property-" + std::string(operation)
        + '-' + std::to_string(++m_requestCounter);
}
}
