#pragma once

#include <sstream>
#include <string>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Prism::Core
{
#if defined(_WIN32)
inline void ThrowIfFailed(HRESULT result, const char* message)
{
    if (SUCCEEDED(result))
    {
        return;
    }

    std::ostringstream stream;
    stream << message << " (HRESULT=0x" << std::hex << static_cast<unsigned long>(result) << ")";
    throw std::runtime_error(stream.str());
}
#endif

inline void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

inline void Check(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
} // namespace Prism::Core
