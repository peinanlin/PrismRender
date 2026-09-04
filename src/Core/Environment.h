#pragma once

#include <string>

namespace Prism::Core
{
std::string ReadEnvironmentVariableValue(const char* name);
bool IsEnvironmentVariableEnabled(const char* name);
} // namespace Prism::Core
