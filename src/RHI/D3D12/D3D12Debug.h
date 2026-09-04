#pragma once

#include <d3d12.h>

#include <string>
#include <string_view>

namespace Prism::RHI::D3D12
{
inline std::wstring Utf8ToWide(
    const std::string_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (required <= 0)
    {
        return std::wstring(
            text.begin(),
            text.end());
    }
    std::wstring result(
        static_cast<std::size_t>(required),
        L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        required);
    return result;
}

inline void SetD3D12DebugName(
    ID3D12Object* object,
    const std::string_view name)
{
    if (object == nullptr || name.empty())
    {
        return;
    }
    const std::wstring wideName = Utf8ToWide(name);
    object->SetName(wideName.c_str());
}
} // namespace Prism::RHI::D3D12
