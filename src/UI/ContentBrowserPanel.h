#pragma once

#include "Asset/AssetDatabase.h"
#include "Engine/EntityId.h"

#include <filesystem>
#include <functional>
#include <string>

namespace Prism::UI
{
struct EditorAssetActions
{
    const Asset::AssetDatabase* database = nullptr;
    std::function<Asset::AssetImportResult(
        const std::filesystem::path&)> importFile;
    std::function<Asset::AssetImportResult(
        const std::filesystem::path&)> reimportFile;
    std::function<Engine::EntityId(const Asset::AssetRecord&)>
        instantiateAsset;
};

struct AssetDragPayload
{
    static constexpr const char* TypeName =
        "PRISM_ASSET_PATH";
    char assetPath[128]{};
    std::uint32_t assetType = 0;
};

class ContentBrowserPanel
{
public:
    void Draw(const EditorAssetActions& actions);
    [[nodiscard]] const std::string&
        GetSelectedAssetId() const;

private:
    void SetImportStatus(
        const Asset::AssetImportResult& result);

    char m_search[128]{};
    int m_typeFilter = 0;
    std::string m_selectedAssetId;
    std::string m_status;
};
}
