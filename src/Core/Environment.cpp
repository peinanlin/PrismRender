#include "Core/Environment.h"

#include <cstdlib>

namespace Prism::Core
{
std::string ReadEnvironmentVariableValue(const char* name)
{
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
    {
        return {};
    }
    const std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
#endif
}

bool IsEnvironmentVariableEnabled(const char* name)
{
    const std::string value =
        ReadEnvironmentVariableValue(name);
    return value == "1" || value == "true" || value == "TRUE"
        || value == "on" || value == "ON";
}
} // namespace Prism::Core
