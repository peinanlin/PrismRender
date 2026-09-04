#include "Core/MinidumpSymbolizer.h"

#include "Core/BuildSymbolIdentity.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace Prism::Core
{
namespace
{
using json = nlohmann::json;

constexpr std::uint32_t CodeViewRsdsSignature =
    0x53445352u;

std::mutex DbgHelpMutex;

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

std::wstring Utf8ToWide(const std::string_view value)
{
    if (value.empty())
    {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0)
    {
        return {};
    }
    std::wstring result(
        static_cast<std::size_t>(required),
        L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required);
    return result;
}

std::string Lowercase(std::string value)
{
    std::ranges::transform(
        value,
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(
                std::tolower(character));
        });
    return value;
}

std::string HexAddress(const std::uint64_t value)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setfill('0')
           << std::setw(16) << value;
    return output.str();
}

class MappedFile
{
public:
    explicit MappedFile(const std::filesystem::path& path)
    {
        m_file = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (m_file == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error(
                "Could not open a diagnostics input file.");
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(m_file, &size)
            || size.QuadPart <= 0)
        {
            Close();
            throw std::runtime_error(
                "A diagnostics input file has an invalid size.");
        }
        m_size = static_cast<std::size_t>(
            size.QuadPart);
        m_mapping = CreateFileMappingW(
            m_file,
            nullptr,
            PAGE_READONLY,
            0,
            0,
            nullptr);
        if (m_mapping == nullptr)
        {
            Close();
            throw std::runtime_error(
                "Could not create a diagnostics file mapping.");
        }
        m_data = static_cast<const std::byte*>(
            MapViewOfFile(
                m_mapping,
                FILE_MAP_READ,
                0,
                0,
                0));
        if (m_data == nullptr)
        {
            Close();
            throw std::runtime_error(
                "Could not map a diagnostics input file.");
        }
    }

    ~MappedFile()
    {
        Close();
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    [[nodiscard]] const std::byte* Data() const
    {
        return m_data;
    }

    [[nodiscard]] std::size_t Size() const
    {
        return m_size;
    }

    [[nodiscard]] const void* PointerAt(
        const std::uint64_t offset,
        const std::size_t bytes) const
    {
        if (offset > m_size
            || bytes > m_size - static_cast<std::size_t>(offset))
        {
            throw std::runtime_error(
                "A Minidump RVA is outside the mapped file.");
        }
        return m_data + static_cast<std::size_t>(offset);
    }

private:
    void Close()
    {
        if (m_data != nullptr)
        {
            UnmapViewOfFile(m_data);
            m_data = nullptr;
        }
        if (m_mapping != nullptr)
        {
            CloseHandle(m_mapping);
            m_mapping = nullptr;
        }
        if (m_file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(m_file);
            m_file = INVALID_HANDLE_VALUE;
        }
    }
    HANDLE m_file = INVALID_HANDLE_VALUE;
    HANDLE m_mapping = nullptr;
    const std::byte* m_data = nullptr;
    std::size_t m_size = 0;
};

template<typename T>
T* ReadDumpStream(
    const MappedFile& file,
    const MINIDUMP_STREAM_TYPE streamType,
    ULONG* streamBytes = nullptr)
{
    PMINIDUMP_DIRECTORY directory = nullptr;
    void* stream = nullptr;
    ULONG bytes = 0;
    if (!MiniDumpReadDumpStream(
            const_cast<void*>(
                static_cast<const void*>(file.Data())),
            streamType,
            &directory,
            &stream,
            &bytes))
    {
        return nullptr;
    }
    if (streamBytes != nullptr)
    {
        *streamBytes = bytes;
    }
    return static_cast<T*>(stream);
}

std::wstring ReadDumpString(
    const MappedFile& file,
    const RVA rva)
{
    if (rva == 0)
    {
        return {};
    }
    const auto* string = static_cast<const MINIDUMP_STRING*>(
        file.PointerAt(rva, sizeof(ULONG32)));
    const std::size_t bytes = string->Length;
    (void)file.PointerAt(
        rva + offsetof(MINIDUMP_STRING, Buffer),
        bytes);
    return std::wstring(
        string->Buffer,
        string->Buffer + bytes / sizeof(wchar_t));
}

CodeViewIdentity ReadDumpCodeView(
    const MappedFile& file,
    const MINIDUMP_LOCATION_DESCRIPTOR& location)
{
    CodeViewIdentity identity{};
    if (location.Rva == 0
        || location.DataSize < 24)
    {
        identity.errorMessage =
            "The dump module has no RSDS CodeView record.";
        return identity;
    }
    const auto* bytes = static_cast<const std::byte*>(
        file.PointerAt(location.Rva, location.DataSize));
    std::uint32_t signature = 0;
    std::memcpy(&signature, bytes, sizeof(signature));
    if (signature != CodeViewRsdsSignature)
    {
        identity.errorMessage =
            "The dump module CodeView record is not RSDS.";
        return identity;
    }
    GUID guid{};
    std::uint32_t age = 0;
    std::memcpy(&guid, bytes + 4, sizeof(guid));
    std::memcpy(&age, bytes + 20, sizeof(age));
    identity.available = true;
    identity.guid = FormatGuid(&guid);
    identity.age = age;
    identity.signature = signature;
    const char* pdbName =
        reinterpret_cast<const char*>(bytes + 24);
    const std::size_t maximumName =
        location.DataSize - 24;
    const std::size_t nameLength =
        std::find(
            pdbName,
            pdbName + maximumName,
            '\0') - pdbName;
    identity.pdbFile.assign(pdbName, nameLength);
    return identity;
}

json SerializeCodeView(const CodeViewIdentity& identity)
{
    return {
        {"available", identity.available},
        {"guid", identity.guid},
        {"age", identity.age},
        {"signature", identity.signature},
        {"pdbFile", identity.pdbFile},
        {"errorMessage", identity.errorMessage}};
}

bool SameCodeView(
    const CodeViewIdentity& left,
    const CodeViewIdentity& right)
{
    return left.available
        && right.available
        && !left.guid.empty()
        && left.guid == right.guid
        && left.age == right.age;
}

struct DumpModule
{
    std::uint64_t baseAddress = 0;
    std::uint32_t imageSize = 0;
    std::uint32_t timestamp = 0;
    std::wstring path;
    std::filesystem::path localImagePath;
    CodeViewIdentity codeView;
    std::vector<std::pair<std::uint64_t, std::uint64_t>>
        executableRanges;
    bool symbolsLoaded = false;
    std::string symbolLoadError;
};

const DumpModule* FindModule(
    const std::vector<DumpModule>& modules,
    const std::uint64_t address)
{
    const auto found = std::ranges::find_if(
        modules,
        [address](const DumpModule& module)
        {
            return address >= module.baseAddress
                && address
                    < module.baseAddress
                        + module.imageSize;
        });
    return found == modules.end() ? nullptr : &*found;
}

bool IsExecutableModuleAddress(
    const DumpModule& module,
    const std::uint64_t address)
{
    return std::ranges::any_of(
        module.executableRanges,
        [address](const auto& range)
        {
            return address >= range.first
                && address < range.second;
        });
}

DumpModule* FindTargetModule(
    std::vector<DumpModule>& modules,
    const std::filesystem::path& executablePath,
    const std::uint64_t exceptionAddress)
{
    const std::string executableName = Lowercase(
        executablePath.filename().string());
    const auto named = std::ranges::find_if(
        modules,
        [&](const DumpModule& module)
        {
            return Lowercase(
                std::filesystem::path(module.path)
                    .filename().string())
                == executableName;
        });
    if (named != modules.end())
    {
        return &*named;
    }
    const DumpModule* containing =
        FindModule(modules, exceptionAddress);
    return containing == nullptr
        ? nullptr
        : &modules[
            static_cast<std::size_t>(
                containing - modules.data())];
}

struct MemoryRange
{
    std::uint64_t start = 0;
    std::uint64_t size = 0;
    const std::byte* data = nullptr;
};

class DumpMemoryReader
{
public:
    void Add(
        const std::uint64_t start,
        const std::uint64_t size,
        const std::byte* data)
    {
        if (size > 0 && data != nullptr)
        {
            m_ranges.push_back({start, size, data});
        }
    }

    bool Read(
        const std::uint64_t address,
        void* buffer,
        const std::uint32_t requested,
        std::uint32_t& bytesRead) const
    {
        bytesRead = 0;
        const auto found = std::ranges::find_if(
            m_ranges,
            [&](const MemoryRange& range)
            {
                return address >= range.start
                    && address < range.start + range.size;
            });
        if (found == m_ranges.end())
        {
            return false;
        }
        const std::uint64_t offset =
            address - found->start;
        const std::uint64_t available =
            found->size - offset;
        bytesRead = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                requested,
                available));
        std::memcpy(
            buffer,
            found->data
                + static_cast<std::size_t>(offset),
            bytesRead);
        return bytesRead > 0;
    }

private:
    std::vector<MemoryRange> m_ranges;
};

thread_local const DumpMemoryReader*
    ActiveMemoryReader = nullptr;

BOOL CALLBACK ReadDumpMemory(
    HANDLE,
    const DWORD64 baseAddress,
    PVOID buffer,
    const DWORD size,
    LPDWORD bytesRead)
{
    if (ActiveMemoryReader == nullptr)
    {
        return FALSE;
    }
    std::uint32_t read = 0;
    const bool success = ActiveMemoryReader->Read(
        baseAddress,
        buffer,
        size,
        read);
    if (bytesRead != nullptr)
    {
        *bytesRead = read;
    }
    return success ? TRUE : FALSE;
}

void AddMemoryRanges(
    const MappedFile& file,
    DumpMemoryReader& reader)
{
    if (const auto* list =
            ReadDumpStream<MINIDUMP_MEMORY_LIST>(
                file,
                MemoryListStream))
    {
        for (std::uint32_t index = 0;
             index < list->NumberOfMemoryRanges;
             ++index)
        {
            const MINIDUMP_MEMORY_DESCRIPTOR& range =
                list->MemoryRanges[index];
            reader.Add(
                range.StartOfMemoryRange,
                range.Memory.DataSize,
                static_cast<const std::byte*>(
                    file.PointerAt(
                        range.Memory.Rva,
                        range.Memory.DataSize)));
        }
    }
    if (const auto* list =
            ReadDumpStream<MINIDUMP_MEMORY64_LIST>(
                file,
                Memory64ListStream))
    {
        std::uint64_t rva = list->BaseRva;
        for (std::uint64_t index = 0;
             index < list->NumberOfMemoryRanges;
             ++index)
        {
            const MINIDUMP_MEMORY_DESCRIPTOR64& range =
                list->MemoryRanges[index];
            reader.Add(
                range.StartOfMemoryRange,
                range.DataSize,
                static_cast<const std::byte*>(
                    file.PointerAt(rva, static_cast<std::size_t>(
                        range.DataSize))));
            rva += range.DataSize;
        }
    }
    if (const auto* list =
            ReadDumpStream<MINIDUMP_THREAD_LIST>(
                file,
                ThreadListStream))
    {
        for (std::uint32_t index = 0;
             index < list->NumberOfThreads;
             ++index)
        {
            const MINIDUMP_MEMORY_DESCRIPTOR& stack =
                list->Threads[index].Stack;
            if (stack.Memory.Rva != 0
                && stack.Memory.DataSize > 0)
            {
                reader.Add(
                    stack.StartOfMemoryRange,
                    stack.Memory.DataSize,
                    static_cast<const std::byte*>(
                        file.PointerAt(
                            stack.Memory.Rva,
                            stack.Memory.DataSize)));
            }
        }
    }
}

void AddImageMemoryRanges(
    const std::filesystem::path& imagePath,
    DumpModule& module,
    DumpMemoryReader& reader,
    std::vector<std::unique_ptr<MappedFile>>& mappedImages)
{
    try
    {
        auto image = std::make_unique<MappedFile>(
            imagePath);
        const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(
            image->PointerAt(0, sizeof(IMAGE_DOS_HEADER)));
        if (dos->e_magic != IMAGE_DOS_SIGNATURE
            || dos->e_lfanew <= 0)
        {
            return;
        }
        const std::size_t ntOffset =
            static_cast<std::size_t>(dos->e_lfanew);
        const auto* signature =
            static_cast<const DWORD*>(
                image->PointerAt(ntOffset, sizeof(DWORD)));
        if (*signature != IMAGE_NT_SIGNATURE)
        {
            return;
        }
        const auto* fileHeader =
            static_cast<const IMAGE_FILE_HEADER*>(
                image->PointerAt(
                    ntOffset + sizeof(DWORD),
                    sizeof(IMAGE_FILE_HEADER)));
        const std::size_t optionalOffset =
            ntOffset + sizeof(DWORD)
            + sizeof(IMAGE_FILE_HEADER);
        const auto* optionalMagic =
            static_cast<const WORD*>(
                image->PointerAt(
                    optionalOffset,
                    sizeof(WORD)));

        std::uint32_t headerBytes = 0;
        if (*optionalMagic
            == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            const auto* optionalHeader =
                static_cast<const IMAGE_OPTIONAL_HEADER64*>(
                    image->PointerAt(
                        optionalOffset,
                        sizeof(IMAGE_OPTIONAL_HEADER64)));
            headerBytes = optionalHeader->SizeOfHeaders;
        }
        else if (*optionalMagic
                 == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            const auto* optionalHeader =
                static_cast<const IMAGE_OPTIONAL_HEADER32*>(
                    image->PointerAt(
                        optionalOffset,
                        sizeof(IMAGE_OPTIONAL_HEADER32)));
            headerBytes = optionalHeader->SizeOfHeaders;
        }
        else
        {
            return;
        }

        reader.Add(
            module.baseAddress,
            std::min<std::uint64_t>(
                headerBytes,
                image->Size()),
            image->Data());

        const std::size_t sectionOffset =
            optionalOffset
            + fileHeader->SizeOfOptionalHeader;
        const std::size_t sectionBytes =
            static_cast<std::size_t>(
                fileHeader->NumberOfSections)
            * sizeof(IMAGE_SECTION_HEADER);
        const auto* sections =
            static_cast<const IMAGE_SECTION_HEADER*>(
                image->PointerAt(
                    sectionOffset,
                    sectionBytes));
        for (std::uint16_t index = 0;
             index < fileHeader->NumberOfSections;
             ++index)
        {
            const IMAGE_SECTION_HEADER& section =
                sections[index];
            if (section.SizeOfRawData == 0)
            {
                continue;
            }
            reader.Add(
                module.baseAddress
                    + section.VirtualAddress,
                section.SizeOfRawData,
                static_cast<const std::byte*>(
                    image->PointerAt(
                        section.PointerToRawData,
                        section.SizeOfRawData)));
            if ((section.Characteristics
                 & IMAGE_SCN_MEM_EXECUTE) != 0)
            {
                const std::uint64_t start =
                    module.baseAddress
                    + section.VirtualAddress;
                const std::uint64_t size =
                    std::max<std::uint64_t>(
                        section.Misc.VirtualSize,
                        section.SizeOfRawData);
                module.executableRanges.emplace_back(
                    start,
                    start + size);
            }
        }
        mappedImages.push_back(std::move(image));
    }
    catch (const std::exception&)
    {
        // Missing non-target module images reduce stack depth but do not
        // invalidate the dump or the target build identity.
    }
}

json SymbolizeAddress(
    HANDLE process,
    const std::vector<DumpModule>& modules,
    std::uint64_t address,
    std::size_t index);

void AppendStackScanFrames(
    HANDLE process,
    const DumpMemoryReader& memoryReader,
    const std::vector<DumpModule>& modules,
    const std::uint64_t stackPointer,
    const std::size_t maximumFrames,
    json& stack)
{
    constexpr std::uint64_t MaximumScanBytes =
        64ull * 1024ull;
    std::vector<std::uint64_t> acceptedAddresses;
    acceptedAddresses.reserve(maximumFrames);
    for (const json& frame : stack)
    {
        acceptedAddresses.push_back(
            frame.value("address", 0ull));
    }

    for (std::uint64_t offset = 0;
         offset < MaximumScanBytes
         && stack.size() < maximumFrames;
         offset += sizeof(std::uint64_t))
    {
        std::uint64_t candidate = 0;
        std::uint32_t bytesRead = 0;
        if (!memoryReader.Read(
                stackPointer + offset,
                &candidate,
                sizeof(candidate),
                bytesRead)
            || bytesRead != sizeof(candidate)
            || candidate == 0
            || std::ranges::find(
                   acceptedAddresses,
                   candidate)
                != acceptedAddresses.end())
        {
            continue;
        }
        const DumpModule* module =
            FindModule(modules, candidate);
        if (module == nullptr
            || !IsExecutableModuleAddress(
                *module,
                candidate))
        {
            continue;
        }
        json frame = SymbolizeAddress(
            process,
            modules,
            candidate,
            stack.size());
        if (!frame.contains("symbol"))
        {
            continue;
        }
        frame["unwindMethod"] = "stack_scan";
        frame["stackOffset"] = offset;
        acceptedAddresses.push_back(candidate);
        stack.push_back(std::move(frame));
    }
}

std::wstring BuildSymbolSearchPath(
    const MinidumpSymbolizationOptions& options)
{
    std::vector<std::filesystem::path> paths;
    if (!options.executablePath.empty())
    {
        paths.push_back(
            options.executablePath.parent_path());
    }
    if (!options.pdbPath.empty())
    {
        paths.push_back(options.pdbPath.parent_path());
    }
    paths.insert(
        paths.end(),
        options.symbolPaths.begin(),
        options.symbolPaths.end());

    std::wstring result;
    for (const std::filesystem::path& path : paths)
    {
        if (path.empty())
        {
            continue;
        }
        if (!result.empty())
        {
            result.push_back(L';');
        }
        result += std::filesystem::absolute(path)
                      .lexically_normal().wstring();
    }
    return result;
}

json SymbolizeAddress(
    HANDLE process,
    const std::vector<DumpModule>& modules,
    const std::uint64_t address,
    const std::size_t index)
{
    json frame = {
        {"index", index},
        {"address", address},
        {"addressHex", HexAddress(address)}};
    if (const DumpModule* module =
            FindModule(modules, address))
    {
        frame["module"] =
            WideToUtf8(
                std::filesystem::path(module->path)
                    .filename().wstring());
        frame["modulePath"] =
            WideToUtf8(module->path);
        frame["moduleOffset"] =
            address - module->baseAddress;
        frame["symbolsLoaded"] =
            module->symbolsLoaded;
    }

    std::array<unsigned char,
        sizeof(SYMBOL_INFOW)
            + MAX_SYM_NAME * sizeof(wchar_t)> storage{};
    auto* symbol =
        reinterpret_cast<SYMBOL_INFOW*>(storage.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (SymFromAddrW(
            process,
            address,
            &displacement,
            symbol))
    {
        frame["symbol"] = WideToUtf8(symbol->Name);
        frame["symbolDisplacement"] = displacement;
    }

    IMAGEHLP_LINEW64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    if (SymGetLineFromAddrW64(
            process,
            address,
            &lineDisplacement,
            &line))
    {
        frame["file"] =
            WideToUtf8(line.FileName);
        frame["line"] = line.LineNumber;
        frame["lineDisplacement"] =
            lineDisplacement;
    }
    return frame;
}

json SerializeModule(const DumpModule& module)
{
    return {
        {"path", WideToUtf8(module.path)},
        {"name", WideToUtf8(
            std::filesystem::path(module.path)
                .filename().wstring())},
        {"baseAddress", module.baseAddress},
        {"baseAddressHex",
         HexAddress(module.baseAddress)},
        {"imageSize", module.imageSize},
        {"timestamp", module.timestamp},
        {"codeView", SerializeCodeView(
            module.codeView)},
        {"symbolsLoaded", module.symbolsLoaded},
        {"symbolLoadError",
         module.symbolLoadError}};
}
} // namespace

MinidumpSymbolizationResult SymbolizeMinidump(
    const MinidumpSymbolizationOptions& options)
{
    MinidumpSymbolizationResult result{};
    try
    {
        if (options.dumpPath.empty()
            || !std::filesystem::is_regular_file(
                options.dumpPath))
        {
            throw std::runtime_error(
                "The Minidump does not exist.");
        }
        if (options.executablePath.empty()
            || !std::filesystem::is_regular_file(
                options.executablePath))
        {
            throw std::runtime_error(
                "The executable used for symbolization does not exist.");
        }
        if (options.maximumFrames == 0
            || options.maximumFrames > 1024)
        {
            throw std::runtime_error(
                "maximumFrames must be between 1 and 1024.");
        }

        MappedFile file(options.dumpPath);
        const auto* exception =
            ReadDumpStream<MINIDUMP_EXCEPTION_STREAM>(
                file,
                ExceptionStream);
        const auto* threadList =
            ReadDumpStream<MINIDUMP_THREAD_LIST>(
                file,
                ThreadListStream);
        const MINIDUMP_THREAD* selectedThread = nullptr;
        if (exception == nullptr)
        {
            if (threadList == nullptr || threadList->NumberOfThreads == 0u)
            {
                result.errorCode =
                    "minidump_thread_context_missing";
                throw std::runtime_error(
                    "The Minidump has neither an exception nor a thread context.");
            }
            // A user-mode hang dump has no ExceptionStream. The first thread
            // is the process entry thread in dumps produced by
            // MiniDumpWriteDump and is the most useful default for diagnosing
            // startup and frame-pump stalls.
            selectedThread = &threadList->Threads[0];
        }
        const auto* moduleList =
            ReadDumpStream<MINIDUMP_MODULE_LIST>(
                file,
                ModuleListStream);
        if (moduleList == nullptr)
        {
            result.errorCode =
                "minidump_module_stream_missing";
            throw std::runtime_error(
                "The Minidump has no module list.");
        }
        const auto* systemInfo =
            ReadDumpStream<MINIDUMP_SYSTEM_INFO>(
                file,
                SystemInfoStream);
        if (systemInfo == nullptr)
        {
            result.errorCode =
                "minidump_system_stream_missing";
            throw std::runtime_error(
                "The Minidump has no system information.");
        }

        const MINIDUMP_LOCATION_DESCRIPTOR& contextLocation =
            exception != nullptr
            ? exception->ThreadContext
            : selectedThread->ThreadContext;
        CONTEXT context{};
        const std::size_t contextBytes = contextLocation.DataSize;
        const void* serializedContext = file.PointerAt(
            contextLocation.Rva, contextBytes);
        std::memcpy(&context, serializedContext,
            std::min(sizeof(context), contextBytes));
        std::uint64_t contextAddress = 0u;
#if defined(_M_X64)
        if (systemInfo->ProcessorArchitecture
            == PROCESSOR_ARCHITECTURE_AMD64)
            contextAddress = context.Rip;
#endif
#if defined(_M_IX86)
        if (systemInfo->ProcessorArchitecture
            == PROCESSOR_ARCHITECTURE_INTEL)
            contextAddress = context.Eip;
#endif

        std::vector<DumpModule> modules;
        modules.reserve(moduleList->NumberOfModules);
        for (std::uint32_t index = 0;
             index < moduleList->NumberOfModules;
             ++index)
        {
            const MINIDUMP_MODULE& serialized =
                moduleList->Modules[index];
            DumpModule module{};
            module.baseAddress =
                serialized.BaseOfImage;
            module.imageSize =
                serialized.SizeOfImage;
            module.timestamp =
                serialized.TimeDateStamp;
            module.path = ReadDumpString(
                file,
                serialized.ModuleNameRva);
            module.codeView = ReadDumpCodeView(
                file,
                serialized.CvRecord);
            modules.push_back(std::move(module));
        }

        const BuildSymbolIdentity buildIdentity =
            CaptureBuildSymbolIdentity(
                options.executablePath,
                options.pdbPath);
        DumpModule* targetModule = FindTargetModule(
            modules,
            options.executablePath,
            contextAddress);
        if (targetModule == nullptr)
        {
            result.errorCode =
                "minidump_target_module_missing";
            throw std::runtime_error(
                "The requested executable is not present in the Minidump.");
        }
        const bool executableMatchesDump =
            SameCodeView(
                targetModule->codeView,
                buildIdentity.executableCodeView);
        const bool pdbMatchesDump =
            SameCodeView(
                targetModule->codeView,
                buildIdentity.pdbCodeView);
        const bool identityMatches =
            buildIdentity.valid
            && executableMatchesDump
            && pdbMatchesDump;

        json moduleReports = json::array();
        json stack = json::array();
        std::string stackError;
        if (!options.requireIdentityMatch
            || identityMatches)
        {
            std::scoped_lock dbgHelpLock(DbgHelpMutex);
            HANDLE process = GetCurrentProcess();
            SymSetOptions(
                SYMOPT_DEFERRED_LOADS
                | SYMOPT_LOAD_LINES
                | SYMOPT_UNDNAME
                | SYMOPT_FAIL_CRITICAL_ERRORS);
            const std::wstring searchPath =
                BuildSymbolSearchPath(options);
            if (!SymInitializeW(
                    process,
                    searchPath.empty()
                        ? nullptr
                        : searchPath.c_str(),
                    FALSE))
            {
                stackError =
                    "SymInitializeW failed with Win32 error "
                    + std::to_string(GetLastError()) + '.';
            }
            else
            {
                for (DumpModule& module : modules)
                {
                    std::filesystem::path imagePath(module.path);
                    if (&module == targetModule)
                    {
                        imagePath = options.executablePath;
                    }
                    module.localImagePath = imagePath;
                    if (!std::filesystem::is_regular_file(
                            imagePath))
                    {
                        module.symbolLoadError =
                            "The module image is unavailable on the symbolization machine.";
                        continue;
                    }
                    const DWORD64 loaded = SymLoadModuleExW(
                        process,
                        nullptr,
                        imagePath.c_str(),
                        std::filesystem::path(module.path)
                            .filename().c_str(),
                        module.baseAddress,
                        module.imageSize,
                        nullptr,
                        0);
                    if (loaded == 0)
                    {
                        module.symbolLoadError =
                            "SymLoadModuleExW failed with Win32 error "
                            + std::to_string(GetLastError()) + '.';
                    }
                    else
                    {
                        IMAGEHLP_MODULEW64 moduleInfo{};
                        moduleInfo.SizeOfStruct =
                            sizeof(moduleInfo);
                        module.symbolsLoaded =
                            SymGetModuleInfoW64(
                                process,
                                module.baseAddress,
                                &moduleInfo)
                            && moduleInfo.SymType != SymNone;
                    }
                }

                DumpMemoryReader memoryReader;
                AddMemoryRanges(file, memoryReader);
                std::vector<std::unique_ptr<MappedFile>>
                    mappedImages;
                mappedImages.reserve(modules.size());
                for (DumpModule& module : modules)
                {
                    if (std::filesystem::is_regular_file(
                            module.localImagePath))
                    {
                        AddImageMemoryRanges(
                            module.localImagePath,
                            module,
                            memoryReader,
                            mappedImages);
                    }
                }
                DWORD machineType = 0;
                STACKFRAME64 frame{};
#if defined(_M_X64)
                if (systemInfo->ProcessorArchitecture
                    == PROCESSOR_ARCHITECTURE_AMD64)
                {
                    machineType =
                        IMAGE_FILE_MACHINE_AMD64;
                    frame.AddrPC.Offset = context.Rip;
                    frame.AddrStack.Offset = context.Rsp;
                    frame.AddrFrame.Offset = context.Rbp;
                }
#endif
#if defined(_M_IX86)
                if (systemInfo->ProcessorArchitecture
                    == PROCESSOR_ARCHITECTURE_INTEL)
                {
                    machineType = IMAGE_FILE_MACHINE_I386;
                    frame.AddrPC.Offset = context.Eip;
                    frame.AddrStack.Offset = context.Esp;
                    frame.AddrFrame.Offset = context.Ebp;
                }
#endif
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrStack.Mode = AddrModeFlat;
                frame.AddrFrame.Mode = AddrModeFlat;
                const std::uint64_t exceptionAddress = contextAddress;
                json exceptionFrame = SymbolizeAddress(
                    process,
                    modules,
                    exceptionAddress,
                    0);
                exceptionFrame["unwindMethod"] = exception != nullptr
                    ? "exception_context"
                    : "thread_context";
                stack.push_back(std::move(
                    exceptionFrame));
                if (machineType == 0)
                {
                    stackError =
                        "The Minidump processor architecture is unsupported.";
                }
                else
                {
                    ActiveMemoryReader = &memoryReader;
                    std::uint64_t previousAddress =
                        exceptionAddress;
                    for (std::size_t index = 1;
                         index < options.maximumFrames;
                         ++index)
                    {
                        if (!StackWalk64(
                                machineType,
                                process,
                                nullptr,
                                &frame,
                                &context,
                                ReadDumpMemory,
                                SymFunctionTableAccess64,
                                SymGetModuleBase64,
                                nullptr)
                            || frame.AddrPC.Offset == 0)
                        {
                            if (stack.size() == 1
                                && stackError.empty())
                            {
                                stackError =
                                    "StackWalk64 stopped at the exception frame. Win32 error "
                                    + std::to_string(GetLastError())
                                    + '.';
                            }
                            break;
                        }
                        if (frame.AddrPC.Offset
                            == previousAddress)
                        {
                            continue;
                        }
                        previousAddress =
                            frame.AddrPC.Offset;
                        json unwoundFrame = SymbolizeAddress(
                            process,
                            modules,
                            frame.AddrPC.Offset,
                            index);
                        unwoundFrame["unwindMethod"] =
                            "stack_walk";
                        stack.push_back(
                            std::move(unwoundFrame));
                    }
                    ActiveMemoryReader = nullptr;
                    if (stack.size() == 1)
                    {
                        AppendStackScanFrames(
                            process,
                            memoryReader,
                            modules,
                            context.Rsp,
                            options.maximumFrames,
                            stack);
                        if (stack.size() > 1)
                        {
                            stackError +=
                                " A conservative executable-section stack scan recovered additional symbolized frames.";
                        }
                    }
                }
                SymCleanup(process);
            }
        }

        for (const DumpModule& module : modules)
        {
            moduleReports.push_back(
                SerializeModule(module));
        }

        result.report = {
            {"format", "PrismMinidumpSymbolization"},
            {"version", 1},
            {"success", identityMatches
                && !stack.empty()},
            {"dump", {
                {"path", std::filesystem::absolute(
                    options.dumpPath).lexically_normal()
                        .generic_string()},
                {"bytes", std::filesystem::file_size(
                    options.dumpPath)}}},
            {"exception", exception != nullptr
                ? json{{"present", true},
                    {"threadId", exception->ThreadId},
                    {"code", exception->ExceptionRecord.ExceptionCode},
                    {"address", exception->ExceptionRecord.ExceptionAddress},
                    {"addressHex", HexAddress(
                        exception->ExceptionRecord.ExceptionAddress)}}
                : json{{"present", false},
                    {"threadId", selectedThread->ThreadId},
                    {"address", contextAddress},
                    {"addressHex", HexAddress(contextAddress)}}},
            {"buildIdentity",
             SerializeBuildSymbolIdentity(buildIdentity)},
            {"dumpTargetModule",
             SerializeModule(*targetModule)},
            {"identity", {
                {"matches", identityMatches},
                {"executableMatchesDump",
                 executableMatchesDump},
                {"pdbMatchesDump", pdbMatchesDump},
                {"requireIdentityMatch",
                 options.requireIdentityMatch}}},
            {"stack", std::move(stack)},
            {"stackError", stackError},
            {"modules", std::move(moduleReports)}};

        if (options.requireIdentityMatch
            && !identityMatches)
        {
            result.errorCode =
                "symbol_identity_mismatch";
            result.errorMessage =
                "The Minidump, executable, and PDB CodeView identities do not match.";
            result.report["success"] = false;
            return result;
        }
        if (result.report.at("stack").empty())
        {
            result.errorCode =
                "minidump_stack_walk_failed";
            result.errorMessage =
                stackError.empty()
                ? "The Minidump stack could not be reconstructed."
                : stackError;
            result.report["success"] = false;
            return result;
        }
        result.success = true;
        result.report["success"] = true;
        return result;
    }
    catch (const std::exception& exception)
    {
        if (result.errorCode.empty())
        {
            result.errorCode =
                "minidump_symbolization_failed";
        }
        result.errorMessage = exception.what();
        if (result.report.empty())
        {
            result.report = {
                {"format", "PrismMinidumpSymbolization"},
                {"version", 1},
                {"success", false},
                {"error", {
                    {"code", result.errorCode},
                    {"message", result.errorMessage}}}};
        }
        return result;
    }
}

bool WriteMinidumpSymbolizationReport(
    const std::filesystem::path& path,
    const MinidumpSymbolizationResult& result,
    std::string* outError)
{
    try
    {
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(
                path.parent_path());
        }
        const std::filesystem::path temporary =
            path.parent_path()
            / (path.filename().string() + ".tmp");
        json report = result.report;
        report["success"] = result.success;
        if (!result.success)
        {
            report["error"] = {
                {"code", result.errorCode},
                {"message", result.errorMessage}};
        }
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error(
                    "Could not create the Minidump symbolization report.");
            }
            output << report.dump(2) << '\n';
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
