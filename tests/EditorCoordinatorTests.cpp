#include "Asset/AssetRegistry.h"
#include "Engine/CommandSystem.h"
#include "Platform/FileDialog.h"
#include "Scene/SceneSession.h"
#include "UI/EditorCoordinator.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
void Expect(const bool condition, const char* const message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

nlohmann::json Request(
    std::string requestId,
    std::string command,
    nlohmann::json arguments = nlohmann::json::object())
{
    return {
        {"requestId", std::move(requestId)},
        {"command", std::move(command)},
        {"arguments", std::move(arguments)}};
}

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        m_path = std::filesystem::temp_directory_path()
            / ("prism-editor-coordinator-"
               + std::to_string(suffix));
        std::filesystem::create_directories(m_path);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    [[nodiscard]] const std::filesystem::path& Get() const noexcept
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};
} // namespace

int main()
{
    try
    {
        using Prism::UI::EditorNavigationTarget;
        using Prism::UI::EditorCoordinator;
        const auto fileDialogEntry =
            &Prism::Platform::OpenFileDialog;
        Expect(
            fileDialogEntry != nullptr,
            "Editor file-dialog boundary was not linked.");
        Expect(
            EditorCoordinator::SelectNavigationTarget(
                false, true, false, false, false)
                == EditorNavigationTarget::Game,
            "Active Game camera did not retain navigation priority.");
        Expect(
            EditorCoordinator::SelectNavigationTarget(
                true, false, false, true, true)
                == EditorNavigationTarget::Scene,
            "Active Scene camera did not retain navigation priority.");
        Expect(
            EditorCoordinator::SelectNavigationTarget(
                false, false, false, true, false)
                == EditorNavigationTarget::Game,
            "Hovered Game viewport did not receive navigation.");
        Expect(
            EditorCoordinator::SelectNavigationTarget(
                false, false, false, false, true)
                == EditorNavigationTarget::Scene,
            "Hovered Scene viewport did not receive navigation.");
        Expect(
            EditorCoordinator::SelectNavigationTarget(
                false, false, true, true, true)
                == EditorNavigationTarget::None,
            "An active gizmo did not suppress camera navigation.");

        TemporaryDirectory temporary;
        Prism::Asset::AssetRegistry assets;
        Prism::Scene::SceneSession session(temporary.Get(), assets);
        session.InitializeWorldFromRenderScene();
        const std::filesystem::path document =
            temporary.Get() / "EditorWorld.prismworld.json";
        Prism::UI::EditorSessionActionBundle actions =
            EditorCoordinator::BuildSessionActions(
                session,
                assets,
                document,
                {});
        Expect(
            actions.document.path == document.string()
                && actions.document.save
                && actions.document.load,
            "Editor document actions were not bound to the SceneSession.");
        Expect(
            actions.commands.processor
                    == &session.GetCommandProcessor()
                && actions.commands.runtimeAssets == &assets
                && actions.commands.worldChanged,
            "Editor command actions did not expose the narrow session contract.");
        actions.commands.worldChanged();
        Expect(
            session.IsWorldDirty(),
            "Editor mutation did not mark the SceneSession dirty.");
        session.SynchronizePendingWorldChanges();
        const std::string beforeEditHash =
            actions.commands.processor->ComputeStateHash();
        Expect(
            actions.commands.processor
                ->Execute(Request(
                    "editor-begin",
                    "transaction.begin"))
                .value("success", false)
                && actions.commands.processor
                    ->Execute(Request(
                        "editor-create",
                        "entity.create",
                        {{"name", "EditorCoordinatorEntity"}}))
                    .value("success", false)
                && actions.commands.processor
                    ->Execute(Request(
                        "editor-commit",
                        "transaction.commit"))
                    .value("success", false),
            "Editor transaction action failed its edit smoke test.");
        const std::string editedHash =
            actions.commands.processor->ComputeStateHash();
        Expect(
            editedHash != beforeEditHash
                && actions.commands.processor
                    ->Execute(Request(
                        "editor-undo",
                        "transaction.undo"))
                    .value("success", false)
                && actions.commands.processor->ComputeStateHash()
                    == beforeEditHash,
            "Editor undo action did not restore the prior World state.");
        Expect(
            actions.commands.processor
                ->Execute(Request(
                    "editor-redo",
                    "transaction.redo"))
                .value("success", false)
                && actions.commands.processor->ComputeStateHash()
                    == editedHash,
            "Editor redo action did not restore the edit.");
        std::string message;
        Expect(
            actions.document.save(message)
                && std::filesystem::is_regular_file(document),
            "Editor save action failed its smoke test.");
        Expect(
            actions.document.load(message),
            "Editor load action failed its smoke test.");

        std::unique_ptr<EditorCoordinator> coordinator =
            EditorCoordinator::Create(
                Prism::RHI::GraphicsApi::Vulkan);
        Expect(
            coordinator != nullptr,
            "EditorCoordinator did not assemble the Vulkan UI session.");
        coordinator->Shutdown();
        coordinator->Shutdown();

        std::cout << "EditorCoordinator tests passed.\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "EditorCoordinator tests failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
