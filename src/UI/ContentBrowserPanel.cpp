#include "UI/ContentBrowserPanel.h"

#include "Platform/FileDialog.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <optional>

namespace Prism::UI
{
namespace
{
std::optional<Asset::AssetType> SelectedType(
    const int filter)
{
    switch (filter)
    {
    case 1: return Asset::AssetType::Scene;
    case 2: return Asset::AssetType::Mesh;
    case 3: return Asset::AssetType::Material;
    case 4: return Asset::AssetType::Texture;
    default: return std::nullopt;
    }
}

const ImVec4& TypeColor(const Asset::AssetType type)
{
    static const ImVec4 Scene{0.42f, 0.75f, 1.0f, 1.0f};
    static const ImVec4 Mesh{0.45f, 0.86f, 0.55f, 1.0f};
    static const ImVec4 Material{0.92f, 0.65f, 0.34f, 1.0f};
    static const ImVec4 Texture{0.78f, 0.52f, 0.92f, 1.0f};
    static const ImVec4 Unknown{0.75f, 0.75f, 0.75f, 1.0f};
    switch (type)
    {
    case Asset::AssetType::Scene: return Scene;
    case Asset::AssetType::Mesh: return Mesh;
    case Asset::AssetType::Material: return Material;
    case Asset::AssetType::Texture: return Texture;
    default: return Unknown;
    }
}
}

void ContentBrowserPanel::Draw(
    const EditorAssetActions& actions)
{
    ImGui::Begin("Content Browser");

    if (ImGui::Button("Import..."))
    {
        const auto selected = Platform::OpenFileDialog(
            L"Import PrismRender Asset",
            {{L"Supported Assets",
              L"*.gltf;*.glb;*.png;*.jpg;*.jpeg;*.tga;*.bmp"},
             {L"glTF Scene", L"*.gltf;*.glb"},
             {L"Image", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp"}});
        if (selected.has_value() && actions.importFile)
        {
            SetImportStatus(actions.importFile(*selected));
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    ImGui::InputTextWithHint(
        "##AssetSearch", "Search assets...",
        m_search, sizeof(m_search));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(115.0f);
    static constexpr const char* TypeNames =
        "All\0Scene\0Mesh\0Material\0Texture\0";
    ImGui::Combo(
        "##AssetType", &m_typeFilter, TypeNames);

    const Asset::AssetDatabase* database =
        actions.database;
    if (database == nullptr)
    {
        ImGui::TextDisabled(
            "Asset Database is unavailable.");
        ImGui::End();
        return;
    }

    const std::vector<Asset::AssetRecord> records =
        database->List(SelectedType(m_typeFilter), m_search);
    if (ImGui::BeginTable(
            "AssetTable",
            4,
            ImGuiTableFlags_RowBg
                | ImGuiTableFlags_BordersInnerV
                | ImGuiTableFlags_Resizable
                | ImGuiTableFlags_ScrollY,
            ImVec2(0.0f, ImGui::GetContentRegionAvail().y
                - 82.0f)))
    {
        ImGui::TableSetupColumn(
            "Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            "Type", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableSetupColumn(
            "Source", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            "Revision", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableHeadersRow();

        for (const Asset::AssetRecord& record : records)
        {
            ImGui::PushID(record.assetId.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool selected =
                record.assetId == m_selectedAssetId;
            if (ImGui::Selectable(
                    record.name.c_str(),
                    selected,
                    ImGuiSelectableFlags_SpanAllColumns))
            {
                m_selectedAssetId = record.assetId;
            }
            if (ImGui::BeginDragDropSource())
            {
                AssetDragPayload payload{};
                strncpy_s(
                    payload.assetPath,
                    record.assetPath.c_str(),
                    _TRUNCATE);
                payload.assetType =
                    static_cast<std::uint32_t>(record.type);
                ImGui::SetDragDropPayload(
                    AssetDragPayload::TypeName,
                    &payload,
                    sizeof(payload));
                ImGui::TextUnformatted(record.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered()
                && ImGui::IsMouseDoubleClicked(
                    ImGuiMouseButton_Left)
                && actions.instantiateAsset
                && (record.type == Asset::AssetType::Scene
                    || record.type == Asset::AssetType::Mesh))
            {
                actions.instantiateAsset(record);
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(
                TypeColor(record.type),
                "%s",
                Asset::AssetDatabase::ToString(
                    record.type).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(
                record.sourcePath.c_str());
            ImGui::TableSetColumnIndex(3);
            if (!record.builtIn)
            {
                ImGui::Text("%llu",
                    static_cast<unsigned long long>(
                        record.importRevision));
            }
            else
            {
                ImGui::TextDisabled("Built-in");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    const Asset::AssetRecord* selectedRecord =
        database->FindById(m_selectedAssetId);
    if (selectedRecord != nullptr)
    {
        ImGui::Text("Selected: %s (%s)",
            selectedRecord->name.c_str(),
            Asset::AssetDatabase::ToString(
                selectedRecord->type).c_str());
        ImGui::SameLine();
        const bool canReimport =
            !selectedRecord->builtIn
            && actions.reimportFile;
        if (!canReimport) ImGui::BeginDisabled();
        if (ImGui::Button("Reimport"))
        {
            SetImportStatus(actions.reimportFile(
                selectedRecord->sourcePath));
        }
        if (!canReimport) ImGui::EndDisabled();
        ImGui::SameLine();
        const bool canInstantiate =
            actions.instantiateAsset
            && (selectedRecord->type
                    == Asset::AssetType::Scene
                || selectedRecord->type
                    == Asset::AssetType::Mesh);
        if (!canInstantiate) ImGui::BeginDisabled();
        if (ImGui::Button("Add to Scene"))
        {
            actions.instantiateAsset(*selectedRecord);
        }
        if (!canInstantiate) ImGui::EndDisabled();
    }
    else
    {
        ImGui::TextDisabled(
            "%zu assets", records.size());
    }

    if (!m_status.empty())
    {
        ImGui::TextWrapped("%s", m_status.c_str());
    }
    ImGui::End();
}

const std::string& ContentBrowserPanel::GetSelectedAssetId() const
{
    return m_selectedAssetId;
}

void ContentBrowserPanel::SetImportStatus(
    const Asset::AssetImportResult& result)
{
    if (result.success)
    {
        m_status = result.reimported
            ? "Reimported " : "Imported ";
        m_status += result.sourcePath + " ("
            + std::to_string(result.addedCount)
            + " added, "
            + std::to_string(result.updatedCount)
            + " updated).";
        m_selectedAssetId = result.sourceAssetId;
        return;
    }
    m_status = "Import failed [" + result.errorCode
        + "]: " + result.errorMessage;
}
}
