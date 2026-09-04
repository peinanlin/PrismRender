#include "Platform/FileDialog.h"

#if defined(_WIN32)
#include <Windows.h>
#include <ShObjIdl.h>
#include <wrl/client.h>
#endif

namespace Prism::Platform
{
std::optional<std::filesystem::path> OpenFileDialog(
    const std::wstring& title,
    const std::vector<FileDialogFilter>& filters)
{
#if defined(_WIN32)
    const HRESULT initializeResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitialize = SUCCEEDED(initializeResult);

    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog))))
    {
        if (uninitialize) CoUninitialize();
        return std::nullopt;
    }

    std::vector<COMDLG_FILTERSPEC> specifications;
    specifications.reserve(filters.size());
    for (const FileDialogFilter& filter : filters)
    {
        specifications.push_back({
            filter.name.c_str(), filter.pattern.c_str()});
    }
    if (!specifications.empty())
    {
        dialog->SetFileTypes(
            static_cast<UINT>(specifications.size()),
            specifications.data());
    }
    dialog->SetTitle(title.c_str());
    dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);

    if (FAILED(dialog->Show(nullptr)))
    {
        if (uninitialize) CoUninitialize();
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)))
    {
        if (uninitialize) CoUninitialize();
        return std::nullopt;
    }

    PWSTR selectedPath = nullptr;
    if (FAILED(item->GetDisplayName(
            SIGDN_FILESYSPATH, &selectedPath)))
    {
        if (uninitialize) CoUninitialize();
        return std::nullopt;
    }
    const std::filesystem::path result(selectedPath);
    CoTaskMemFree(selectedPath);
    if (uninitialize) CoUninitialize();
    return result;
#else
    (void)title;
    (void)filters;
    return std::nullopt;
#endif
}
}
