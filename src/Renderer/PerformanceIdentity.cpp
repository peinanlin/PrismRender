#include "Renderer/PerformanceIdentity.h"

#if defined(_WIN32)
#include <Windows.h>
#include <intrin.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>
#include <stdexcept>
#include <vector>

namespace Prism::Renderer
{
namespace
{
std::uint64_t HashBytes(
    std::uint64_t hash,
    const unsigned char* bytes,
    const std::size_t byteCount)
{
    for (std::size_t index = 0; index < byteCount; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t HashText(
    const std::uint64_t seed,
    const std::string_view text)
{
    return HashBytes(
        seed,
        reinterpret_cast<const unsigned char*>(text.data()),
        text.size());
}

std::string Hex64(const std::uint64_t value)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16)
           << value;
    return output.str();
}

std::uint64_t HashFile(
    std::uint64_t hash,
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error(
            "Could not open a performance identity input file.");
    }
    std::array<char, 64 * 1024> buffer{};
    while (input)
    {
        input.read(
            buffer.data(),
            static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        hash = HashBytes(
            hash,
            reinterpret_cast<const unsigned char*>(buffer.data()),
            static_cast<std::size_t>(count));
    }
    return hash;
}

bool IsShaderSource(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::ranges::transform(
        extension,
        extension.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return extension == ".hlsl" || extension == ".slang";
}

std::pair<std::string, std::uint32_t> HashShaderDirectory(
    const std::filesystem::path& shaderDirectory)
{
    if (!std::filesystem::is_directory(shaderDirectory))
    {
        return {"missing", 0};
    }
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(
             shaderDirectory))
    {
        if (entry.is_regular_file()
            && IsShaderSource(entry.path()))
        {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);

    std::uint64_t hash = 14695981039346656037ull;
    for (const std::filesystem::path& file : files)
    {
        const std::string relative =
            file.lexically_relative(shaderDirectory).generic_string();
        hash = HashText(hash, relative);
        hash = HashText(hash, "=");
        hash = HashFile(hash, file);
        hash = HashText(hash, ";");
    }
    return {
        Hex64(hash),
        static_cast<std::uint32_t>(files.size())};
}

std::filesystem::path GetExecutablePath()
{
#if defined(_WIN32)
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
    {
        throw std::runtime_error(
            "Could not resolve the executable path.");
    }
    return std::filesystem::path(
        std::wstring_view(path.data(), length));
#else
    std::array<char, 4096> path{};
    const ssize_t length = readlink(
        "/proc/self/exe",
        path.data(),
        path.size() - 1);
    if (length <= 0)
    {
        throw std::runtime_error(
            "Could not resolve the executable path.");
    }
    path[static_cast<std::size_t>(length)] = '\0';
    return std::filesystem::path(path.data());
#endif
}

std::string GetCpuName()
{
#if defined(_WIN32)
    std::array<int, 4> registers{};
    __cpuid(registers.data(), static_cast<int>(0x80000000u));
    if (static_cast<unsigned int>(registers[0]) < 0x80000004u)
    {
        return "Unknown CPU";
    }
    std::array<char, 49> brand{};
    for (unsigned int leaf = 0; leaf < 3; ++leaf)
    {
        __cpuid(
            registers.data(),
            static_cast<int>(0x80000002u + leaf));
        std::memcpy(
            brand.data() + leaf * 16,
            registers.data(),
            16);
    }
    std::string value(brand.data());
    const std::size_t first = value.find_first_not_of(' ');
    const std::size_t last = value.find_last_not_of(' ');
    return first == std::string::npos
        ? std::string("Unknown CPU")
        : value.substr(first, last - first + 1);
#else
    std::ifstream cpuInfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuInfo, line))
    {
        const std::size_t separator = line.find(':');
        if (separator == std::string::npos)
        {
            continue;
        }
        const std::string key = line.substr(0, separator);
        if (key.find("model name") == std::string::npos
            && key.find("Hardware") == std::string::npos)
        {
            continue;
        }
        const std::size_t first =
            line.find_first_not_of(" \t", separator + 1);
        if (first != std::string::npos)
        {
            return line.substr(first);
        }
    }
    return "Unknown CPU";
#endif
}

std::string GetBuildConfiguration()
{
#if defined(PRISM_RENDER_BUILD_CONFIGURATION)
    return PRISM_RENDER_BUILD_CONFIGURATION;
#elif defined(_DEBUG)
    return "Debug";
#elif defined(NDEBUG)
    return "Release";
#else
    return "RelWithDebInfo";
#endif
}

std::string GetCompiler()
{
#if defined(_MSC_FULL_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#elif defined(__clang__)
    return "Clang " + std::string(__clang_version__);
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "."
        + std::to_string(__GNUC_MINOR__) + "."
        + std::to_string(__GNUC_PATCHLEVEL__);
#else
    return "Unknown Compiler";
#endif
}

std::string GetArchitecture()
{
#if defined(_M_X64)
    return "x64";
#elif defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(__amd64__)
    return "x64";
#elif defined(__aarch64__)
    return "arm64";
#else
    return "unknown";
#endif
}

std::string GetCanonicalApiName(const RHI::GraphicsApi api)
{
    switch (api)
    {
    case RHI::GraphicsApi::Direct3D12: return "d3d12";
    case RHI::GraphicsApi::Direct3D11: return "d3d11";
    case RHI::GraphicsApi::Vulkan: return "vulkan";
    }
    return "unknown";
}
} // namespace

RuntimePerformanceIdentity CaptureRuntimePerformanceIdentity(
    const RHI::GraphicsApi graphicsApi,
    const RHI::GraphicsAdapterInfo& adapter,
    const std::filesystem::path& shaderDirectory)
{
    RuntimePerformanceIdentity identity{};
    identity.graphicsApi = GetCanonicalApiName(graphicsApi);
    identity.adapter = adapter;
    identity.cpuName = GetCpuName();
    identity.buildConfiguration = GetBuildConfiguration();
    identity.compiler = GetCompiler();
    identity.architecture = GetArchitecture();
    const auto [shaderRevision, shaderFileCount] =
        HashShaderDirectory(shaderDirectory);
    identity.shaderRevision = shaderRevision;
    identity.shaderFileCount = shaderFileCount;
    identity.executableHash = Hex64(
        HashFile(
            14695981039346656037ull,
            GetExecutablePath()));
    return identity;
}

nlohmann::json SerializeRuntimePerformanceIdentity(
    const RuntimePerformanceIdentity& identity)
{
    return {
        {"format", "PrismRuntimePerformanceIdentity"},
        {"version", 1},
        {"identity", {
            {"graphicsApi", identity.graphicsApi},
            {"adapterName", identity.adapter.name},
            {"vendorId", identity.adapter.vendorId},
            {"deviceId", identity.adapter.deviceId},
            {"dedicatedVideoMemoryBytes",
             identity.adapter.dedicatedVideoMemoryBytes},
            {"sharedSystemMemoryBytes",
             identity.adapter.sharedSystemMemoryBytes},
            {"driverVersionRaw",
             identity.adapter.driverVersionRaw},
            {"driverVersion", identity.adapter.driverVersion},
            {"apiVersion", identity.adapter.apiVersion},
            {"cpuName", identity.cpuName},
            {"buildConfiguration",
             identity.buildConfiguration},
            {"compiler", identity.compiler},
            {"architecture", identity.architecture},
            {"shaderRevision", identity.shaderRevision},
            {"shaderFileCount", identity.shaderFileCount},
            {"executableHash", identity.executableHash}}}};
}

bool WriteRuntimePerformanceIdentity(
    const std::filesystem::path& path,
    const RuntimePerformanceIdentity& identity,
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
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create the performance identity report.");
        }
        output << SerializeRuntimePerformanceIdentity(identity).dump(2)
               << '\n';
        if (!output)
        {
            throw std::runtime_error(
                "Could not write the performance identity report.");
        }
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
} // namespace Prism::Renderer
