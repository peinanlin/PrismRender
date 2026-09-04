#include "Core/BuildSymbolIdentity.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace Prism::Core
{
namespace
{
std::filesystem::path GetCurrentExecutablePath()
{
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
    {
        throw std::runtime_error(
            "Could not resolve the current executable path.");
    }
    return std::filesystem::path(
        std::wstring_view(path.data(), length));
}

std::string Hex64(const std::uint64_t value)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0')
           << std::setw(16) << value;
    return output.str();
}

std::string WideToUtf8(const std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0)
    {
        return {};
    }
    std::string result(
        static_cast<std::size_t>(required),
        '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr);
    return result;
}

CodeViewIdentity ReadIndexIdentity(
    const std::filesystem::path& path)
{
    CodeViewIdentity identity{};
    SYMSRV_INDEX_INFOW info{};
    info.sizeofstruct = sizeof(info);
    if (!SymSrvGetFileIndexInfoW(
            path.c_str(),
            &info,
            0))
    {
        identity.errorMessage =
            "DbgHelp could not read CodeView identity. Win32 error "
            + std::to_string(GetLastError()) + '.';
        return identity;
    }
    identity.available = true;
    identity.guid = FormatGuid(&info.guid);
    identity.age = info.age;
    identity.signature = info.sig;
    identity.timestamp = info.timestamp;
    identity.imageSize = info.size;
    identity.pdbFile = WideToUtf8(info.pdbfile);
    return identity;
}

bool SameCodeViewIdentity(
    const CodeViewIdentity& left,
    const CodeViewIdentity& right)
{
    return left.available
        && right.available
        && !left.guid.empty()
        && left.guid == right.guid
        && left.age == right.age;
}

nlohmann::json SerializeCodeViewIdentity(
    const CodeViewIdentity& identity)
{
    return {
        {"available", identity.available},
        {"guid", identity.guid},
        {"age", identity.age},
        {"signature", identity.signature},
        {"timestamp", identity.timestamp},
        {"imageSize", identity.imageSize},
        {"pdbFile", identity.pdbFile},
        {"errorMessage", identity.errorMessage}};
}
} // namespace

std::string HashFileFNV1a64(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error(
            "Could not open a build identity file for hashing: "
            + path.string());
    }
    std::uint64_t hash = 14695981039346656037ull;
    std::array<char, 64 * 1024> buffer{};
    while (input)
    {
        input.read(
            buffer.data(),
            static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        for (std::streamsize index = 0;
             index < count;
             ++index)
        {
            hash ^= static_cast<unsigned char>(
                buffer[static_cast<std::size_t>(index)]);
            hash *= 1099511628211ull;
        }
    }
    return Hex64(hash);
}

std::string FormatGuid(const void* guidBytes)
{
    if (guidBytes == nullptr)
    {
        return {};
    }
    const GUID& guid =
        *static_cast<const GUID*>(guidBytes);
    std::ostringstream output;
    output << '{'
           << std::uppercase
           << std::hex
           << std::setfill('0')
           << std::setw(8) << guid.Data1 << '-'
           << std::setw(4) << guid.Data2 << '-'
           << std::setw(4) << guid.Data3 << '-'
           << std::setw(2)
           << static_cast<unsigned int>(guid.Data4[0])
           << std::setw(2)
           << static_cast<unsigned int>(guid.Data4[1])
           << '-';
    for (std::size_t index = 2;
         index < std::size(guid.Data4);
         ++index)
    {
        output << std::setw(2)
               << static_cast<unsigned int>(
                   guid.Data4[index]);
    }
    output << '}';
    return output.str();
}

BuildSymbolIdentity CaptureBuildSymbolIdentity(
    const std::filesystem::path& executablePath,
    const std::filesystem::path& pdbPath)
{
    BuildSymbolIdentity identity{};
    try
    {
        identity.executablePath = std::filesystem::absolute(
            executablePath.empty()
                ? GetCurrentExecutablePath()
                : executablePath).lexically_normal();
        if (!std::filesystem::is_regular_file(
                identity.executablePath))
        {
            throw std::runtime_error(
                "The build executable does not exist.");
        }
        identity.executableBytes =
            std::filesystem::file_size(
                identity.executablePath);
        identity.executableHash =
            HashFileFNV1a64(identity.executablePath);
        identity.executableCodeView =
            ReadIndexIdentity(identity.executablePath);

        if (!pdbPath.empty())
        {
            identity.pdbPath = std::filesystem::absolute(
                pdbPath).lexically_normal();
        }
        else
        {
            identity.pdbPath = identity.executablePath;
            identity.pdbPath.replace_extension(".pdb");
            if (!std::filesystem::is_regular_file(
                    identity.pdbPath)
                && !identity.executableCodeView.pdbFile.empty())
            {
                identity.pdbPath =
                    identity.executablePath.parent_path()
                    / std::filesystem::path(
                        identity.executableCodeView.pdbFile)
                          .filename();
            }
        }

        if (std::filesystem::is_regular_file(identity.pdbPath))
        {
            identity.pdbBytes =
                std::filesystem::file_size(identity.pdbPath);
            identity.pdbHash =
                HashFileFNV1a64(identity.pdbPath);
            identity.pdbCodeView =
                ReadIndexIdentity(identity.pdbPath);
            identity.pdbMatchesExecutable =
                SameCodeViewIdentity(
                    identity.executableCodeView,
                    identity.pdbCodeView);
        }
        else
        {
            identity.pdbCodeView.errorMessage =
                "The matching PDB file was not found.";
        }

        const std::string codeViewKey =
            identity.executableCodeView.available
            ? identity.executableCodeView.guid
                + '-' + std::to_string(
                    identity.executableCodeView.age)
            : std::string("no-codeview");
        identity.buildId =
            codeViewKey + '-' + identity.executableHash;
        identity.valid =
            identity.executableCodeView.available
            && identity.pdbCodeView.available
            && identity.pdbMatchesExecutable;
        if (!identity.valid)
        {
            identity.errorMessage =
                "The executable and PDB CodeView identities do not match.";
        }
    }
    catch (const std::exception& exception)
    {
        identity.valid = false;
        identity.errorMessage = exception.what();
    }
    return identity;
}

nlohmann::json SerializeBuildSymbolIdentity(
    const BuildSymbolIdentity& identity)
{
    return {
        {"format", "PrismBuildSymbolIdentity"},
        {"version", 1},
        {"valid", identity.valid},
        {"buildId", identity.buildId},
        {"executable", {
            {"path", identity.executablePath.generic_string()},
            {"bytes", identity.executableBytes},
            {"hash", identity.executableHash},
            {"codeView", SerializeCodeViewIdentity(
                identity.executableCodeView)}}},
        {"pdb", {
            {"path", identity.pdbPath.generic_string()},
            {"bytes", identity.pdbBytes},
            {"hash", identity.pdbHash},
            {"codeView", SerializeCodeViewIdentity(
                identity.pdbCodeView)}}},
        {"pdbMatchesExecutable",
         identity.pdbMatchesExecutable},
        {"errorMessage", identity.errorMessage}};
}

bool WriteBuildSymbolIdentity(
    const std::filesystem::path& path,
    const BuildSymbolIdentity& identity,
    std::string* outError)
{
    try
    {
        if (path.empty())
        {
            return true;
        }
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(
                path.parent_path());
        }
        const std::filesystem::path temporary =
            path.parent_path()
            / (path.filename().string() + ".tmp");
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error(
                    "Could not create the Build Symbol Identity report.");
            }
            output
                << SerializeBuildSymbolIdentity(identity).dump(2)
                << '\n';
        }
        if (std::filesystem::exists(path))
        {
            std::filesystem::remove(path);
        }
        std::filesystem::rename(temporary, path);
        return true;
    }
    catch (const std::exception& exception)
    {
        if (outError != nullptr)
        {
            *outError = exception.what();
        }
        return false;
    }
}
} // namespace Prism::Core
