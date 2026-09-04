#include "Engine/CommandSystem.h"

#include "Engine/WorldSerializer.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace Prism::Engine
{
namespace
{
using json = nlohmann::json;

class CommandError final : public std::runtime_error
{
public:
    CommandError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), m_code(std::move(code))
    {
    }

    [[nodiscard]] const std::string& GetCode() const { return m_code; }

private:
    std::string m_code;
};

Float3 ReadFloat3(const json& value, const std::string_view field)
{
    if (!value.is_array() || value.size() != 3)
    {
        throw CommandError("invalid_property", std::string(field) + " must contain three numbers.");
    }
    return {value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
}

Core::Double3 ReadDouble3(
    const json& value,
    const std::string_view field)
{
    if (!value.is_array() || value.size() != 3)
    {
        throw CommandError(
            "invalid_property",
            std::string(field)
                + " must contain three numbers.");
    }
    return {
        value.at(0).get<double>(),
        value.at(1).get<double>(),
        value.at(2).get<double>()};
}

Float4 ReadFloat4(const json& value, const std::string_view field)
{
    if (!value.is_array() || value.size() != 4)
    {
        throw CommandError("invalid_property", std::string(field) + " must contain four numbers.");
    }
    return {
        value.at(0).get<float>(), value.at(1).get<float>(),
        value.at(2).get<float>(), value.at(3).get<float>()};
}

json ToJson(const Float3& value)
{
    return json::array({value.x, value.y, value.z});
}

json ToJson(const Core::Double3& value)
{
    return json::array({value.x, value.y, value.z});
}

json ToJson(const Float4& value)
{
    return json::array({value.x, value.y, value.z, value.w});
}

std::string Lowercase(std::string value)
{
    std::ranges::transform(value, value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}
} // namespace

CommandProcessor::CommandProcessor(std::filesystem::path projectRoot)
    : m_projectRoot(std::filesystem::absolute(std::move(projectRoot)).lexically_normal())
{
    m_initialState = CaptureState();
}

nlohmann::json CommandProcessor::Execute(const nlohmann::json& request)
{
    std::string requestId;
    std::string command;
    try
    {
        if (!request.is_object())
        {
            throw CommandError("invalid_request", "A command request must be a JSON object.");
        }
        requestId = request.value("requestId", std::string{});
        command = request.value("command", std::string{});
        if (requestId.empty() || command.empty())
        {
            throw CommandError("invalid_request", "requestId and command are required.");
        }
        if (const auto cached = m_requestCache.find(requestId); cached != m_requestCache.end())
        {
            json result = cached->second;
            result["idempotentReplay"] = true;
            return result;
        }

        json result = ExecuteUncached(requestId, command, request.value("arguments", json::object()));
        m_requestCache.emplace(requestId, result);
        return result;
    }
    catch (const CommandError& error)
    {
        json result = MakeFailure(requestId, command, error.GetCode(), error.what());
        if (!requestId.empty()) m_requestCache.emplace(requestId, result);
        return result;
    }
    catch (const std::exception& exception)
    {
        json result = MakeFailure(requestId, command, "command_failed", exception.what());
        if (!requestId.empty()) m_requestCache.emplace(requestId, result);
        return result;
    }
}

nlohmann::json CommandProcessor::ExecuteUncached(
    const std::string& requestId,
    const std::string& command,
    const nlohmann::json& arguments)
{
    if (command == "transaction.begin")
    {
        if (m_activeTransaction.has_value())
        {
            throw CommandError("transaction_active", "A transaction is already active.");
        }
        m_sceneChangeTracker.BeginCommitScope();
        m_activeTransaction = ActiveTransaction{
            m_nextTransactionId++,
            CaptureState(),
            {},
            SceneChangeCategory::None};
        return MakeSuccess(requestId, command, m_activeTransaction->id);
    }
    if (command == "transaction.commit")
    {
        if (!m_activeTransaction.has_value())
        {
            throw CommandError("no_transaction", "There is no active transaction to commit.");
        }
        const std::uint64_t transactionId = m_activeTransaction->id;
        if (!m_activeTransaction->requests.empty())
        {
            AppendJournalEntry({
                transactionId,
                std::move(m_activeTransaction->requests),
                std::move(m_activeTransaction->beforeState),
                CaptureState(),
                m_activeTransaction->sceneChanges});
        }
        (void)m_sceneChangeTracker.CommitScope();
        m_activeTransaction.reset();
        return MakeSuccess(requestId, command, transactionId);
    }
    if (command == "transaction.rollback")
    {
        if (!m_activeTransaction.has_value())
        {
            throw CommandError("no_transaction", "There is no active transaction to roll back.");
        }
        const std::uint64_t transactionId = m_activeTransaction->id;
        RestoreState(m_activeTransaction->beforeState);
        m_sceneChangeTracker.RollbackScope();
        m_activeTransaction.reset();
        return MakeSuccess(requestId, command, transactionId);
    }
    if (command == "transaction.undo")
    {
        if (m_activeTransaction.has_value())
        {
            throw CommandError("transaction_active", "Commit or roll back the active transaction first.");
        }
        if (m_journalCursor == 0)
        {
            throw CommandError("nothing_to_undo", "The command journal has nothing to undo.");
        }
        --m_journalCursor;
        const JournalEntry& entry = m_journal[m_journalCursor];
        RestoreState(entry.beforeState);
        m_sceneChangeTracker.Record(entry.sceneChanges);
        return MakeSuccess(requestId, command, entry.transactionId);
    }
    if (command == "transaction.redo")
    {
        if (m_activeTransaction.has_value())
        {
            throw CommandError("transaction_active", "Commit or roll back the active transaction first.");
        }
        if (m_journalCursor >= m_journal.size())
        {
            throw CommandError("nothing_to_redo", "The command journal has nothing to redo.");
        }
        const JournalEntry& entry = m_journal[m_journalCursor++];
        RestoreState(entry.afterState);
        m_sceneChangeTracker.Record(entry.sceneChanges);
        return MakeSuccess(requestId, command, entry.transactionId);
    }

    if (!IsMutatingCommand(command))
    {
        return MakeSuccess(requestId, command, 0, ApplyCommand(command, arguments));
    }

    const json replayRequest{{"command", command}, {"arguments", arguments}};
    if (m_activeTransaction.has_value())
    {
        json commandBefore = CaptureState();
        try
        {
            json data = ApplyCommand(command, arguments);
            const SceneChangeCategory sceneChanges =
                ClassifySceneChange(command, arguments);
            m_sceneChangeTracker.Record(sceneChanges);
            m_activeTransaction->sceneChanges |= sceneChanges;
            m_activeTransaction->requests.push_back(replayRequest);
            return MakeSuccess(requestId, command, m_activeTransaction->id, std::move(data));
        }
        catch (...)
        {
            RestoreState(commandBefore);
            throw;
        }
    }

    const std::uint64_t transactionId = m_nextTransactionId++;
    json beforeState = CaptureState();
    try
    {
        json data = ApplyCommand(command, arguments);
        const SceneChangeCategory sceneChanges =
            ClassifySceneChange(command, arguments);
        AppendJournalEntry({
            transactionId,
            {replayRequest},
            beforeState,
            CaptureState(),
            sceneChanges});
        m_sceneChangeTracker.Record(sceneChanges);
        return MakeSuccess(requestId, command, transactionId, std::move(data));
    }
    catch (...)
    {
        RestoreState(beforeState);
        throw;
    }
}

nlohmann::json CommandProcessor::ApplyCommand(
    const std::string& command,
    const nlohmann::json& arguments)
{
    if (!arguments.is_object())
    {
        throw CommandError("invalid_arguments", "Command arguments must be a JSON object.");
    }

    if (command == "engine.describe")
    {
        return DescribeEngine();
    }
    if (command == "world.status")
    {
        return {
            {"entityCount", m_world.GetEntityCount()},
            {"simulationTick", m_simulationTick},
            {"fixedDeltaSeconds", m_fixedDeltaSeconds},
            {"randomSeed", m_randomSeed},
            {"journalEntries", m_journalCursor},
            {"transactionActive", m_activeTransaction.has_value()}};
    }
    if (command == "world.snapshot")
    {
        return {{"snapshot", CaptureState()}};
    }
    if (command == "world.restore")
    {
        RestoreState(arguments.at("snapshot"));
        return {{"restored", true}};
    }
    if (command == "world.save")
    {
        const std::filesystem::path path = ResolveProjectPath(arguments.at("path").get<std::string>());
        std::string error;
        if (!WorldSerializer::Save(path, m_world, &error))
        {
            throw CommandError("world_save_failed", error);
        }
        return {{"path", path.generic_string()}};
    }
    if (command == "world.load")
    {
        const std::filesystem::path path = ResolveProjectPath(arguments.at("path").get<std::string>());
        std::string error;
        if (!WorldSerializer::Load(path, m_world, &error))
        {
            throw CommandError("world_load_failed", error);
        }
        return {{"path", path.generic_string()}, {"entityCount", m_world.GetEntityCount()}};
    }
    if (command == "entity.create")
    {
        std::optional<EntityId> requestedId;
        if (arguments.contains("id"))
        {
            requestedId = EntityId::Parse(arguments.at("id").get<std::string>());
            if (!requestedId.has_value())
            {
                throw CommandError("invalid_entity", "id is not a valid UUID.");
            }
        }
        EntityRecord& entity = m_world.CreateEntity(
            arguments.value("name", std::string("Entity")), requestedId);
        return {{"entity", entity.id.ToString()}};
    }
    if (command == "entity.delete")
    {
        const EntityId id = ReadEntityArgument(arguments);
        if (!m_world.DestroyEntity(id))
        {
            throw CommandError("entity_not_found", "The requested entity does not exist.");
        }
        return {{"entity", id.ToString()}};
    }
    if (command == "entity.get")
    {
        const EntityId id = ReadEntityArgument(arguments);
        const EntityRecord* entity = m_world.FindEntity(id);
        if (entity == nullptr)
        {
            throw CommandError("entity_not_found", "The requested entity does not exist.");
        }
        return {{"entity", SerializeEntity(*entity)}};
    }
    if (command == "entity.list")
    {
        json entities = json::array();
        for (const auto& [id, entity] : m_world.GetEntities())
        {
            (void)id;
            entities.push_back(SerializeEntity(entity));
        }
        return {{"entities", std::move(entities)}};
    }
    if (command == "component.add")
    {
        EntityRecord* entity = m_world.FindEntity(ReadEntityArgument(arguments));
        if (entity == nullptr)
        {
            throw CommandError("entity_not_found", "The requested entity does not exist.");
        }
        const std::string component = arguments.at("component").get<std::string>();
        const ComponentDescriptor* descriptor = m_reflection.FindComponent(component);
        if (descriptor == nullptr)
        {
            throw CommandError("component_not_found", "The component type is not registered.");
        }
        if (descriptor->required)
        {
            throw CommandError("component_exists", "Required components already exist on every entity.");
        }
        if (component == "Hierarchy")
        {
            if (entity->hierarchy.has_value()) throw CommandError("component_exists", "Hierarchy already exists.");
            entity->hierarchy.emplace();
        }
        else if (component == "MeshRenderer")
        {
            if (entity->meshRenderer.has_value()) throw CommandError("component_exists", "MeshRenderer already exists.");
            entity->meshRenderer.emplace();
        }
        else if (component == "MaterialOverride")
        {
            if (entity->materialOverride.has_value()) throw CommandError("component_exists", "MaterialOverride already exists.");
            if (!entity->meshRenderer.has_value()) throw CommandError("component_dependency_missing", "MaterialOverride requires MeshRenderer.");
            entity->materialOverride.emplace();
        }
        else if (component == "Camera")
        {
            if (entity->camera.has_value()) throw CommandError("component_exists", "Camera already exists.");
            entity->camera.emplace();
        }
        else if (component == "DirectionalLight")
        {
            if (entity->directionalLight.has_value()) throw CommandError("component_exists", "DirectionalLight already exists.");
            entity->directionalLight.emplace();
        }
        else if (component == "PointLight")
        {
            if (entity->pointLight.has_value()) throw CommandError("component_exists", "PointLight already exists.");
            entity->pointLight.emplace();
        }
        else if (component == "SpotLight")
        {
            if (entity->spotLight.has_value()) throw CommandError("component_exists", "SpotLight already exists.");
            entity->spotLight.emplace();
        }
        if (arguments.contains("properties"))
        {
            SetComponentProperties(*entity, component, arguments.at("properties"));
        }
        return {{"entity", entity->id.ToString()}, {"component", component}};
    }
    if (command == "component.remove")
    {
        EntityRecord* entity = m_world.FindEntity(ReadEntityArgument(arguments));
        if (entity == nullptr) throw CommandError("entity_not_found", "The requested entity does not exist.");
        const std::string component = arguments.at("component").get<std::string>();
        const ComponentDescriptor* descriptor = m_reflection.FindComponent(component);
        if (descriptor == nullptr) throw CommandError("component_not_found", "The component type is not registered.");
        if (descriptor->required) throw CommandError("required_component", "Required components cannot be removed.");
        bool removed = false;
        if (component == "Hierarchy")
        {
            removed = entity->hierarchy.has_value();
            entity->hierarchy.reset();
        }
        if (component == "MeshRenderer")
        {
            removed = entity->meshRenderer.has_value();
            entity->meshRenderer.reset();
            entity->materialOverride.reset();
        }
        if (component == "MaterialOverride")
        {
            removed = entity->materialOverride.has_value();
            entity->materialOverride.reset();
        }
        if (component == "Camera")
        {
            removed = entity->camera.has_value();
            entity->camera.reset();
        }
        if (component == "DirectionalLight")
        {
            removed = entity->directionalLight.has_value();
            entity->directionalLight.reset();
        }
        if (component == "PointLight")
        {
            removed = entity->pointLight.has_value();
            entity->pointLight.reset();
        }
        if (component == "SpotLight")
        {
            removed = entity->spotLight.has_value();
            entity->spotLight.reset();
        }
        if (!removed) throw CommandError("component_not_found", "The entity does not contain that component.");
        return {{"entity", entity->id.ToString()}, {"component", component}};
    }
    if (command == "component.get")
    {
        const EntityRecord* entity = m_world.FindEntity(ReadEntityArgument(arguments));
        if (entity == nullptr) throw CommandError("entity_not_found", "The requested entity does not exist.");
        const std::string component = arguments.at("component").get<std::string>();
        return {{"component", component}, {"properties", SerializeComponent(*entity, component)}};
    }
    if (command == "component.set")
    {
        EntityRecord* entity = m_world.FindEntity(ReadEntityArgument(arguments));
        if (entity == nullptr) throw CommandError("entity_not_found", "The requested entity does not exist.");
        const std::string component = arguments.at("component").get<std::string>();
        SetComponentProperties(*entity, component, arguments.at("properties"));
        return {{"entity", entity->id.ToString()}, {"component", component}};
    }
    if (command == "simulation.configure")
    {
        if (arguments.contains("fixedDeltaSeconds"))
        {
            const double value = arguments.at("fixedDeltaSeconds").get<double>();
            if (value <= 0.0 || value > 1.0)
            {
                throw CommandError("invalid_timestep", "fixedDeltaSeconds must be in the range (0, 1].");
            }
            m_fixedDeltaSeconds = value;
        }
        if (arguments.contains("randomSeed")) m_randomSeed = arguments.at("randomSeed").get<std::uint64_t>();
        return {{"fixedDeltaSeconds", m_fixedDeltaSeconds}, {"randomSeed", m_randomSeed}};
    }
    if (command == "simulation.step")
    {
        const std::uint64_t count = arguments.value("count", 1ull);
        if (count == 0 || count > 1'000'000ull)
        {
            throw CommandError("invalid_step_count", "Simulation step count must be between 1 and 1000000.");
        }
        m_simulationTick += count;
        return {
            {"simulationTick", m_simulationTick},
            {"simulationTimeSeconds", static_cast<double>(m_simulationTick) * m_fixedDeltaSeconds}};
    }
    if (command == "journal.list")
    {
        json entries = json::array();
        for (std::size_t index = 0; index < m_journal.size(); ++index)
        {
            entries.push_back({
                {"transactionId", m_journal[index].transactionId},
                {"applied", index < m_journalCursor},
                {"requestCount", m_journal[index].requests.size()},
                {"worldHash", WorldSerializer::ComputeHash(m_journal[index].afterState)}});
        }
        return {{"entries", std::move(entries)}};
    }
    if (command == "journal.export")
    {
        return {{"journal", ExportJournal()}};
    }

    throw CommandError("unknown_command", "The command is not registered: " + command);
}

nlohmann::json CommandProcessor::CaptureState() const
{
    return {
        {"format", "PrismEngineSnapshot"},
        {"version", 1},
        {"simulation", {
            {"fixedDeltaSeconds", m_fixedDeltaSeconds},
            {"tick", m_simulationTick},
            {"randomSeed", m_randomSeed}}},
        {"world", WorldSerializer::Serialize(m_world)}};
}

void CommandProcessor::RestoreState(const nlohmann::json& state)
{
    if (state.value("format", std::string{}) != "PrismEngineSnapshot" || state.value("version", 0) != 1)
    {
        throw CommandError("invalid_snapshot", "The snapshot format or version is not supported.");
    }
    World restored;
    WorldSerializer::Deserialize(state.at("world"), restored);
    const json& simulation = state.at("simulation");
    const double fixedDelta = simulation.at("fixedDeltaSeconds").get<double>();
    if (fixedDelta <= 0.0 || fixedDelta > 1.0)
    {
        throw CommandError("invalid_snapshot", "The snapshot contains an invalid fixed timestep.");
    }
    m_world = std::move(restored);
    m_fixedDeltaSeconds = fixedDelta;
    m_simulationTick = simulation.at("tick").get<std::uint64_t>();
    m_randomSeed = simulation.at("randomSeed").get<std::uint64_t>();
}

void CommandProcessor::InitializeWorld(World world)
{
    m_world = std::move(world);
    m_simulationTick = 0;
    m_fixedDeltaSeconds = 1.0 / 60.0;
    m_randomSeed = 1;
    ResetJournal(CaptureState());
}

std::string CommandProcessor::ComputeStateHash() const
{
    return WorldSerializer::ComputeHash(CaptureState());
}

bool CommandProcessor::SaveSnapshot(
    const std::filesystem::path& path,
    std::string* outError) const
{
    try
    {
        const std::filesystem::path resolved = ResolveProjectPath(path);
        if (!resolved.parent_path().empty())
        {
            std::filesystem::create_directories(resolved.parent_path());
        }
        std::ofstream output(resolved, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error("Could not open the snapshot file for writing.");
        }
        output << CaptureState().dump(2) << '\n';
        if (!output)
        {
            throw std::runtime_error("Failed while writing the snapshot file.");
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr) *outError = exception.what();
        return false;
    }
}

bool CommandProcessor::LoadSnapshotFile(
    const std::filesystem::path& path,
    std::string* outError)
{
    try
    {
        const std::filesystem::path resolved = ResolveProjectPath(path);
        std::ifstream input(resolved, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("Could not open the snapshot file for reading.");
        }
        json snapshot;
        input >> snapshot;
        ResetJournal(snapshot);
        m_sceneChangeTracker.Record(
            SceneChangeCategory::FullRebuild);
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr) *outError = exception.what();
        return false;
    }
}

nlohmann::json CommandProcessor::ExportJournal() const
{
    json entries = json::array();
    for (std::size_t index = 0; index < m_journalCursor; ++index)
    {
        const JournalEntry& entry = m_journal[index];
        entries.push_back({
            {"transactionId", entry.transactionId},
            {"requests", entry.requests},
            {"expectedHash", WorldSerializer::ComputeHash(entry.afterState)}});
    }
    return {
        {"format", "PrismCommandJournal"},
        {"version", 1},
        {"initialState", m_initialState},
        {"entries", std::move(entries)}};
}

bool CommandProcessor::SaveJournal(const std::filesystem::path& path, std::string* outError) const
{
    try
    {
        const std::filesystem::path resolved = ResolveProjectPath(path);
        if (!resolved.parent_path().empty()) std::filesystem::create_directories(resolved.parent_path());
        std::ofstream output(resolved, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not open the journal file for writing.");
        output << ExportJournal().dump(2) << '\n';
        if (!output) throw std::runtime_error("Failed while writing the journal file.");
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr) *outError = exception.what();
        return false;
    }
}

bool CommandProcessor::ReplayJournal(const nlohmann::json& journal, std::string* outError)
{
    try
    {
        if (journal.value("format", std::string{}) != "PrismCommandJournal"
            || journal.value("version", 0) != 1)
        {
            throw std::runtime_error("The command journal format or version is not supported.");
        }

        ResetJournal(journal.at("initialState"));
        m_sceneChangeTracker.BeginCommitScope();
        for (const json& serializedEntry : journal.at("entries"))
        {
            JournalEntry entry{};
            entry.transactionId = serializedEntry.at("transactionId").get<std::uint64_t>();
            entry.beforeState = CaptureState();
            for (const json& request : serializedEntry.at("requests"))
            {
                const std::string command = request.at("command").get<std::string>();
                if (!IsMutatingCommand(command))
                {
                    throw std::runtime_error("A replay journal contains a non-mutating command.");
                }
                (void)ApplyCommand(command, request.value("arguments", json::object()));
                const SceneChangeCategory sceneChanges =
                    ClassifySceneChange(
                        command,
                        request.value("arguments", json::object()));
                entry.sceneChanges |= sceneChanges;
                m_sceneChangeTracker.Record(sceneChanges);
                entry.requests.push_back(request);
            }
            entry.afterState = CaptureState();
            const std::string actualHash = WorldSerializer::ComputeHash(entry.afterState);
            if (actualHash != serializedEntry.at("expectedHash").get<std::string>())
            {
                throw std::runtime_error("Deterministic replay hash mismatch in transaction "
                    + std::to_string(entry.transactionId) + '.');
            }
            m_nextTransactionId = std::max(m_nextTransactionId, entry.transactionId + 1);
            AppendJournalEntry(std::move(entry));
        }
        (void)m_sceneChangeTracker.CommitScope();
        return true;
    }
    catch (const std::exception& exception)
    {
        m_sceneChangeTracker.RollbackScope();
        if (outError != nullptr) *outError = exception.what();
        return false;
    }
}

bool CommandProcessor::ReplayJournalFile(const std::filesystem::path& path, std::string* outError)
{
    try
    {
        const std::filesystem::path resolved = ResolveProjectPath(path);
        std::ifstream input(resolved, std::ios::binary);
        if (!input) throw std::runtime_error("Could not open the command journal.");
        json journal;
        input >> journal;
        return ReplayJournal(journal, outError);
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr) *outError = exception.what();
        return false;
    }
}

nlohmann::json CommandProcessor::DescribeEngine() const
{
    json components = json::array();
    for (const ComponentDescriptor& component : m_reflection.GetComponents())
    {
        json properties = json::array();
        for (const PropertyDescriptor& property : component.properties)
        {
            properties.push_back({
                {"name", property.name},
                {"type", ToString(property.type)},
                {"writable", property.writable},
                {"displayName", property.displayName},
                {"category", property.category},
                {"minimum", property.minimum},
                {"maximum", property.maximum},
                {"step", property.step},
                {"hasRange", property.hasRange},
                {"acceptedAssetType", property.acceptedAssetType}});
        }
        components.push_back({
            {"name", component.name},
            {"required", component.required},
            {"properties", std::move(properties)}});
    }

    static constexpr const char* Commands[] = {
        "engine.describe", "world.status", "world.snapshot", "world.restore", "world.save", "world.load",
        "entity.create", "entity.delete", "entity.get", "entity.list",
        "component.add", "component.remove", "component.get", "component.set",
        "simulation.configure", "simulation.step",
        "transaction.begin", "transaction.commit", "transaction.rollback", "transaction.undo", "transaction.redo",
        "journal.list", "journal.export"};
    return {
        {"protocol", "PrismHarness/1"},
        {"components", std::move(components)},
        {"commands", Commands},
        {"capabilities", {
            {"idempotentRequests", true},
            {"transactions", true},
            {"undoRedo", true},
            {"deterministicReplay", true},
            {"fixedTimestep", true}}}};
}

nlohmann::json CommandProcessor::SerializeEntity(const EntityRecord& entity) const
{
    json components;
    components["Name"] = SerializeComponent(entity, "Name");
    components["Transform"] = SerializeComponent(entity, "Transform");
    if (entity.hierarchy.has_value()) components["Hierarchy"] = SerializeComponent(entity, "Hierarchy");
    if (entity.meshRenderer.has_value()) components["MeshRenderer"] = SerializeComponent(entity, "MeshRenderer");
    if (entity.materialOverride.has_value()) components["MaterialOverride"] = SerializeComponent(entity, "MaterialOverride");
    if (entity.camera.has_value()) components["Camera"] = SerializeComponent(entity, "Camera");
    if (entity.directionalLight.has_value()) components["DirectionalLight"] = SerializeComponent(entity, "DirectionalLight");
    if (entity.pointLight.has_value()) components["PointLight"] = SerializeComponent(entity, "PointLight");
    if (entity.spotLight.has_value()) components["SpotLight"] = SerializeComponent(entity, "SpotLight");
    return {{"id", entity.id.ToString()}, {"components", std::move(components)}};
}

nlohmann::json CommandProcessor::SerializeComponent(
    const EntityRecord& entity,
    const std::string_view component) const
{
    if (component == "Name") return {{"value", entity.name.value}};
    if (component == "Transform")
    {
        return {
            {"position", ToJson(entity.transform.position)},
            {"rotation", ToJson(entity.transform.rotation)},
            {"scale", ToJson(entity.transform.scale)}};
    }
    if (component == "Hierarchy")
    {
        if (!entity.hierarchy.has_value()) throw CommandError("component_not_found", "The entity has no Hierarchy component.");
        return {{"parent", entity.hierarchy->parent.has_value()
            ? json(entity.hierarchy->parent->ToString()) : json(nullptr)}};
    }
    if (component == "MeshRenderer")
    {
        if (!entity.meshRenderer.has_value()) throw CommandError("component_not_found", "The entity has no MeshRenderer component.");
        return {
            {"meshAsset", entity.meshRenderer->meshAsset},
            {"materialAsset", entity.meshRenderer->materialAsset},
            {"visible", entity.meshRenderer->visible}};
    }
    if (component == "MaterialOverride")
    {
        if (!entity.materialOverride.has_value()) throw CommandError("component_not_found", "The entity has no MaterialOverride component.");
        const MaterialOverrideComponent& material = *entity.materialOverride;
        return {
            {"albedoColor", ToJson(material.albedoColor)},
            {"emissiveColor", ToJson(material.emissiveColor)},
            {"metallic", material.metallic},
            {"roughness", material.roughness},
            {"occlusionStrength", material.occlusionStrength},
            {"normalScale", material.normalScale},
            {"emissiveStrength", material.emissiveStrength},
            {"alphaCutoff", material.alphaCutoff},
            {"alphaMode", material.alphaMode},
            {"useAlbedoTexture", material.useAlbedoTexture},
            {"useMetallicRoughnessTexture", material.useMetallicRoughnessTexture},
            {"useNormalTexture", material.useNormalTexture},
            {"useOcclusionTexture", material.useOcclusionTexture},
            {"useEmissiveTexture", material.useEmissiveTexture}};
    }
    if (component == "Camera")
    {
        if (!entity.camera.has_value()) throw CommandError("component_not_found", "The entity has no Camera component.");
        return {
            {"fieldOfViewY", entity.camera->fieldOfViewY},
            {"nearPlane", entity.camera->nearPlane},
            {"farPlane", entity.camera->farPlane}};
    }
    if (component == "DirectionalLight")
    {
        if (!entity.directionalLight.has_value()) throw CommandError("component_not_found", "The entity has no DirectionalLight component.");
        return {
            {"direction", ToJson(entity.directionalLight->direction)},
            {"color", ToJson(entity.directionalLight->color)},
            {"intensity", entity.directionalLight->intensity}};
    }
    if (component == "PointLight")
    {
        if (!entity.pointLight.has_value()) throw CommandError("component_not_found", "The entity has no PointLight component.");
        return {
            {"color", ToJson(entity.pointLight->color)},
            {"intensity", entity.pointLight->intensity},
            {"range", entity.pointLight->range},
            {"castsShadow", entity.pointLight->castsShadow}};
    }
    if (component == "SpotLight")
    {
        if (!entity.spotLight.has_value()) throw CommandError("component_not_found", "The entity has no SpotLight component.");
        return {
            {"direction", ToJson(entity.spotLight->direction)},
            {"color", ToJson(entity.spotLight->color)},
            {"intensity", entity.spotLight->intensity},
            {"range", entity.spotLight->range},
            {"innerAngleRadians", entity.spotLight->innerAngleRadians},
            {"outerAngleRadians", entity.spotLight->outerAngleRadians},
            {"castsShadow", entity.spotLight->castsShadow}};
    }
    throw CommandError("component_not_found", "The component type is not registered.");
}

void CommandProcessor::SetComponentProperties(
    EntityRecord& entity,
    const std::string_view component,
    const nlohmann::json& properties)
{
    if (!properties.is_object()) throw CommandError("invalid_properties", "properties must be a JSON object.");
    if (m_reflection.FindComponent(component) == nullptr)
    {
        throw CommandError("component_not_found", "The component type is not registered.");
    }
    for (const auto& [name, value] : properties.items())
    {
        if (m_reflection.FindProperty(component, name) == nullptr)
        {
            throw CommandError("property_not_found", "The component property is not registered: " + name);
        }
        if (component == "Name" && name == "value") entity.name.value = value.get<std::string>();
        else if (component == "Transform" && name == "position") entity.transform.position = ReadDouble3(value, "Transform.position");
        else if (component == "Transform" && name == "rotation") entity.transform.rotation = ReadFloat3(value, "Transform.rotation");
        else if (component == "Transform" && name == "scale") entity.transform.scale = ReadFloat3(value, "Transform.scale");
        else if (component == "Hierarchy" && name == "parent")
        {
            if (!entity.hierarchy.has_value()) throw CommandError("component_not_found", "Add Hierarchy before setting it.");
            if (value.is_null()) entity.hierarchy->parent.reset();
            else
            {
                const std::optional<EntityId> parent = EntityId::Parse(value.get<std::string>());
                if (!parent.has_value() || *parent == entity.id || m_world.FindEntity(*parent) == nullptr)
                {
                    throw CommandError("invalid_parent", "Hierarchy.parent must reference another existing entity.");
                }
                std::optional<EntityId> ancestor = parent;
                while (ancestor.has_value())
                {
                    if (*ancestor == entity.id)
                    {
                        throw CommandError("hierarchy_cycle", "Hierarchy.parent would create a cycle.");
                    }
                    const EntityRecord* ancestorEntity = m_world.FindEntity(*ancestor);
                    ancestor = ancestorEntity != nullptr && ancestorEntity->hierarchy.has_value()
                        ? ancestorEntity->hierarchy->parent
                        : std::nullopt;
                }
                entity.hierarchy->parent = *parent;
            }
        }
        else if (component == "MeshRenderer" && name == "meshAsset")
        {
            if (!entity.meshRenderer.has_value()) throw CommandError("component_not_found", "Add MeshRenderer before setting it.");
            entity.meshRenderer->meshAsset = value.get<std::string>();
        }
        else if (component == "MeshRenderer" && name == "materialAsset")
        {
            if (!entity.meshRenderer.has_value()) throw CommandError("component_not_found", "Add MeshRenderer before setting it.");
            entity.meshRenderer->materialAsset = value.get<std::string>();
        }
        else if (component == "MeshRenderer" && name == "visible")
        {
            if (!entity.meshRenderer.has_value()) throw CommandError("component_not_found", "Add MeshRenderer before setting it.");
            entity.meshRenderer->visible = value.get<bool>();
        }
        else if (component == "MaterialOverride")
        {
            if (!entity.materialOverride.has_value()) throw CommandError("component_not_found", "Add MaterialOverride before setting it.");
            MaterialOverrideComponent& material = *entity.materialOverride;
            if (name == "albedoColor") material.albedoColor = ReadFloat4(value, "MaterialOverride.albedoColor");
            else if (name == "emissiveColor") material.emissiveColor = ReadFloat3(value, "MaterialOverride.emissiveColor");
            else if (name == "metallic") material.metallic = value.get<float>();
            else if (name == "roughness") material.roughness = value.get<float>();
            else if (name == "occlusionStrength") material.occlusionStrength = value.get<float>();
            else if (name == "normalScale") material.normalScale = value.get<float>();
            else if (name == "emissiveStrength") material.emissiveStrength = value.get<float>();
            else if (name == "alphaCutoff") material.alphaCutoff = value.get<float>();
            else if (name == "alphaMode") material.alphaMode = value.get<float>();
            else if (name == "useAlbedoTexture") material.useAlbedoTexture = value.get<bool>();
            else if (name == "useMetallicRoughnessTexture") material.useMetallicRoughnessTexture = value.get<bool>();
            else if (name == "useNormalTexture") material.useNormalTexture = value.get<bool>();
            else if (name == "useOcclusionTexture") material.useOcclusionTexture = value.get<bool>();
            else if (name == "useEmissiveTexture") material.useEmissiveTexture = value.get<bool>();
        }
        else if (component == "Camera" && name == "fieldOfViewY")
        {
            if (!entity.camera.has_value()) throw CommandError("component_not_found", "Add Camera before setting it.");
            entity.camera->fieldOfViewY = value.get<float>();
        }
        else if (component == "Camera" && name == "nearPlane")
        {
            if (!entity.camera.has_value()) throw CommandError("component_not_found", "Add Camera before setting it.");
            entity.camera->nearPlane = value.get<float>();
        }
        else if (component == "Camera" && name == "farPlane")
        {
            if (!entity.camera.has_value()) throw CommandError("component_not_found", "Add Camera before setting it.");
            entity.camera->farPlane = value.get<float>();
        }
        else if (component == "DirectionalLight" && name == "direction")
        {
            if (!entity.directionalLight.has_value()) throw CommandError("component_not_found", "Add DirectionalLight before setting it.");
            entity.directionalLight->direction = ReadFloat3(value, "DirectionalLight.direction");
        }
        else if (component == "DirectionalLight" && name == "color")
        {
            if (!entity.directionalLight.has_value()) throw CommandError("component_not_found", "Add DirectionalLight before setting it.");
            entity.directionalLight->color = ReadFloat3(value, "DirectionalLight.color");
        }
        else if (component == "DirectionalLight" && name == "intensity")
        {
            if (!entity.directionalLight.has_value()) throw CommandError("component_not_found", "Add DirectionalLight before setting it.");
            entity.directionalLight->intensity = value.get<float>();
        }
        else if (component == "PointLight" && name == "color")
        {
            if (!entity.pointLight.has_value()) throw CommandError("component_not_found", "Add PointLight before setting it.");
            entity.pointLight->color = ReadFloat3(value, "PointLight.color");
        }
        else if (component == "PointLight" && name == "intensity")
        {
            if (!entity.pointLight.has_value()) throw CommandError("component_not_found", "Add PointLight before setting it.");
            entity.pointLight->intensity = value.get<float>();
        }
        else if (component == "PointLight" && name == "range")
        {
            if (!entity.pointLight.has_value()) throw CommandError("component_not_found", "Add PointLight before setting it.");
            entity.pointLight->range = value.get<float>();
        }
        else if (component == "PointLight" && name == "castsShadow")
        {
            if (!entity.pointLight.has_value()) throw CommandError("component_not_found", "Add PointLight before setting it.");
            entity.pointLight->castsShadow = value.get<bool>();
        }
        else if (component == "SpotLight" && name == "direction")
        {
            if (!entity.spotLight.has_value()) throw CommandError("component_not_found", "Add SpotLight before setting it.");
            entity.spotLight->direction = ReadFloat3(value, "SpotLight.direction");
        }
        else if (component == "SpotLight" && name == "color")
        {
            if (!entity.spotLight.has_value()) throw CommandError("component_not_found", "Add SpotLight before setting it.");
            entity.spotLight->color = ReadFloat3(value, "SpotLight.color");
        }
        else if (component == "SpotLight" && name == "intensity")
        {
            if (!entity.spotLight.has_value()) throw CommandError("component_not_found", "Add SpotLight before setting it.");
            entity.spotLight->intensity = value.get<float>();
        }
        else if (component == "SpotLight" && name == "range")
        {
            if (!entity.spotLight.has_value()) throw CommandError("component_not_found", "Add SpotLight before setting it.");
            entity.spotLight->range = value.get<float>();
        }
        else if (component == "SpotLight" && name == "innerAngleRadians")
        {
            if (!entity.spotLight.has_value()) throw CommandError("component_not_found", "Add SpotLight before setting it.");
            entity.spotLight->innerAngleRadians = value.get<float>();
        }
        else if (component == "SpotLight" && name == "outerAngleRadians")
        {
            if (!entity.spotLight.has_value()) throw CommandError("component_not_found", "Add SpotLight before setting it.");
            entity.spotLight->outerAngleRadians = value.get<float>();
        }
        else if (component == "SpotLight" && name == "castsShadow")
        {
            if (!entity.spotLight.has_value()) throw CommandError("component_not_found", "Add SpotLight before setting it.");
            entity.spotLight->castsShadow = value.get<bool>();
        }
    }
    if (entity.camera.has_value()
        && (entity.camera->fieldOfViewY <= 0.0f || entity.camera->nearPlane <= 0.0f
            || entity.camera->farPlane <= entity.camera->nearPlane))
    {
        throw CommandError("invalid_camera", "Camera requires FOV > 0 and farPlane > nearPlane > 0.");
    }
    if (entity.pointLight.has_value() && entity.pointLight->range <= 0.0f)
    {
        throw CommandError("invalid_light", "PointLight.range must be greater than zero.");
    }
    if (entity.materialOverride.has_value())
    {
        const MaterialOverrideComponent& material = *entity.materialOverride;
        if (material.metallic < 0.0f || material.metallic > 1.0f
            || material.roughness < 0.0f || material.roughness > 1.0f
            || material.occlusionStrength < 0.0f || material.occlusionStrength > 1.0f
            || material.normalScale < 0.0f || material.emissiveStrength < 0.0f
            || material.alphaCutoff < 0.0f || material.alphaCutoff > 1.0f
            || material.alphaMode < 0.0f || material.alphaMode > 2.0f)
        {
            throw CommandError("invalid_material_override", "MaterialOverride parameters are outside their supported ranges.");
        }
    }
    if (entity.spotLight.has_value()
        && (entity.spotLight->range <= 0.0f
            || entity.spotLight->innerAngleRadians <= 0.0f
            || entity.spotLight->outerAngleRadians
                <= entity.spotLight->innerAngleRadians
            || entity.spotLight->outerAngleRadians
                >= 1.57079633f))
    {
        throw CommandError(
            "invalid_light",
            "SpotLight requires range > 0 and 0 < innerAngleRadians < outerAngleRadians < pi/2.");
    }
}

EntityId CommandProcessor::ReadEntityArgument(
    const nlohmann::json& arguments,
    const std::string_view field) const
{
    const std::optional<EntityId> id = EntityId::Parse(arguments.at(std::string(field)).get<std::string>());
    if (!id.has_value()) throw CommandError("invalid_entity", std::string(field) + " is not a valid UUID.");
    return *id;
}

std::filesystem::path CommandProcessor::ResolveProjectPath(const std::filesystem::path& path) const
{
    const std::filesystem::path candidate = std::filesystem::absolute(
        path.is_absolute() ? path : m_projectRoot / path).lexically_normal();

    auto rootIterator = m_projectRoot.begin();
    auto candidateIterator = candidate.begin();
    for (; rootIterator != m_projectRoot.end(); ++rootIterator, ++candidateIterator)
    {
        if (candidateIterator == candidate.end()
            || Lowercase(rootIterator->string()) != Lowercase(candidateIterator->string()))
        {
            throw CommandError("path_outside_project", "Harness file access is restricted to the project root.");
        }
    }
    return candidate;
}

bool CommandProcessor::IsMutatingCommand(const std::string_view command) const
{
    return command == "world.restore" || command == "world.load"
        || command == "entity.create" || command == "entity.delete"
        || command == "component.add" || command == "component.remove" || command == "component.set"
        || command == "simulation.configure" || command == "simulation.step";
}

SceneChangeCategory CommandProcessor::ClassifySceneChange(
    const std::string_view command,
    const nlohmann::json& arguments) const
{
    if (command == "world.restore" || command == "world.load")
    {
        return SceneChangeCategory::FullRebuild;
    }
    if (command == "entity.create")
    {
        return SceneChangeCategory::EntityTopology;
    }
    if (command == "entity.delete")
    {
        return SceneChangeCategory::EntityTopology
            | SceneChangeCategory::Hierarchy;
    }
    if (command == "simulation.configure"
        || command == "simulation.step")
    {
        return SceneChangeCategory::None;
    }
    if (command != "component.add"
        && command != "component.remove"
        && command != "component.set")
    {
        return SceneChangeCategory::FullRebuild;
    }

    const std::string component =
        arguments.value("component", std::string{});
    if (component == "Hierarchy")
    {
        return SceneChangeCategory::Hierarchy;
    }
    if (component == "Transform")
    {
        return SceneChangeCategory::Transform;
    }
    if (component == "MaterialOverride")
    {
        return SceneChangeCategory::Material;
    }
    if (component == "Camera")
    {
        return SceneChangeCategory::Camera;
    }
    if (component == "DirectionalLight"
        || component == "PointLight"
        || component == "SpotLight")
    {
        return SceneChangeCategory::Lighting;
    }
    if (component == "MeshRenderer")
    {
        if (command == "component.set"
            && arguments.contains("properties"))
        {
            SceneChangeCategory result = SceneChangeCategory::None;
            const nlohmann::json& properties = arguments.at("properties");
            if (properties.contains("visible"))
            {
                result |= SceneChangeCategory::Visibility;
            }
            if (properties.contains("meshAsset")
                || properties.contains("materialAsset"))
            {
                result |= SceneChangeCategory::Material;
            }
            return result;
        }
        return SceneChangeCategory::EntityTopology
            | SceneChangeCategory::Visibility
            | SceneChangeCategory::Material;
    }
    // Name and future components still rebuild conservatively until their
    // extraction policy is explicit.
    return SceneChangeCategory::FullRebuild;
}

nlohmann::json CommandProcessor::MakeSuccess(
    const std::string& requestId,
    const std::string& command,
    const std::uint64_t transactionId,
    nlohmann::json data) const
{
    if (data.is_null()) data = json::object();
    return {
        {"success", true},
        {"requestId", requestId},
        {"command", command},
        {"transactionId", transactionId},
        {"worldHash", ComputeStateHash()},
        {"data", std::move(data)}};
}

nlohmann::json CommandProcessor::MakeFailure(
    const std::string& requestId,
    const std::string& command,
    std::string code,
    std::string message) const
{
    return {
        {"success", false},
        {"requestId", requestId},
        {"command", command},
        {"worldHash", ComputeStateHash()},
        {"error", {{"code", std::move(code)}, {"message", std::move(message)}}}};
}

void CommandProcessor::AppendJournalEntry(JournalEntry entry)
{
    if (m_journalCursor < m_journal.size())
    {
        m_journal.erase(m_journal.begin() + static_cast<std::ptrdiff_t>(m_journalCursor), m_journal.end());
    }
    m_journal.push_back(std::move(entry));
    m_journalCursor = m_journal.size();
}

void CommandProcessor::ResetJournal(const nlohmann::json& initialState)
{
    RestoreState(initialState);
    m_initialState = initialState;
    m_journal.clear();
    m_journalCursor = 0;
    m_activeTransaction.reset();
    m_requestCache.clear();
    m_nextTransactionId = 1;
    m_sceneChangeTracker.Reset();
}

World& CommandProcessor::GetWorld() { return m_world; }
const World& CommandProcessor::GetWorld() const { return m_world; }
const ReflectionRegistry& CommandProcessor::GetReflection() const { return m_reflection; }

SceneChangeTracker& CommandProcessor::GetSceneChangeTracker() noexcept
{
    return m_sceneChangeTracker;
}

const SceneChangeTracker&
CommandProcessor::GetSceneChangeTracker() const noexcept
{
    return m_sceneChangeTracker;
}

nlohmann::json CommandProcessor::GetComponentProperties(
    const EntityId entity,
    const std::string_view component) const
{
    const EntityRecord* record = m_world.FindEntity(entity);
    if (record == nullptr)
    {
        return nlohmann::json::object();
    }
    try
    {
        return SerializeComponent(*record, component);
    }
    catch (const CommandError&)
    {
        return nlohmann::json::object();
    }
}
} // namespace Prism::Engine
