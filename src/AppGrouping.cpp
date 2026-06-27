#include "pch.h"
#include "AppGrouping.h"
#include "Settings.h"

#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>
#include <unordered_map>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")

namespace
{
    std::wstring ToLower(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return s;
    }

    std::wstring BaseNameNoExt(const std::wstring& path)
    {
        size_t slash = path.find_last_of(L"\\/");
        std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos)
        {
            name = name.substr(0, dot);
        }
        return name;
    }
}

std::wstring AppGrouping::ExecutablePath(DWORD processId)
{
    std::wstring result;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process)
    {
        wchar_t buffer[MAX_PATH * 2] = {};
        DWORD size = ARRAYSIZE(buffer);
        if (QueryFullProcessImageNameW(process, 0, buffer, &size))
        {
            result.assign(buffer, size);
        }
        CloseHandle(process);
    }
    return result;
}

// For packaged (UWP/Store) apps the visible window is hosted by
// ApplicationFrameHost.exe, so the executable path is useless for grouping.
// Their stable identity is the AppUserModelID, exposed on the window via the
// shell property store.
std::wstring AppGrouping::ResolveAumid(HWND hwnd)
{
    std::wstring aumid;
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) && store)
    {
        PROPVARIANT prop;
        PropVariantInit(&prop);
        if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &prop)) && prop.vt == VT_LPWSTR && prop.pwszVal)
        {
            aumid = prop.pwszVal;
        }
        PropVariantClear(&prop);
        store->Release();
    }
    return aumid;
}

std::wstring AppGrouping::FriendlyName(const std::wstring& exePath, const std::wstring& aumid)
{
    // Prefer the executable's FileDescription from its version resource.
    if (!exePath.empty())
    {
        DWORD handle = 0;
        DWORD size = GetFileVersionInfoSizeW(exePath.c_str(), &handle);
        if (size > 0)
        {
            std::vector<BYTE> data(size);
            if (GetFileVersionInfoW(exePath.c_str(), handle, size, data.data()))
            {
                struct LangCodePage
                {
                    WORD language;
                    WORD codePage;
                }* translate = nullptr;
                UINT translateLen = 0;
                if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translate), &translateLen) && translateLen >= sizeof(LangCodePage))
                {
                    wchar_t subBlock[64];
                    swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\FileDescription", translate->language, translate->codePage);
                    wchar_t* desc = nullptr;
                    UINT descLen = 0;
                    if (VerQueryValueW(data.data(), subBlock, reinterpret_cast<void**>(&desc), &descLen) && desc && descLen > 1)
                    {
                        return std::wstring(desc, descLen - 1);
                    }
                }
            }
        }
        return BaseNameNoExt(exePath);
    }

    if (!aumid.empty())
    {
        // AUMIDs look like "Family_hash!App"; the bang-suffix is the least ugly piece.
        size_t bang = aumid.find_last_of(L'!');
        if (bang != std::wstring::npos && bang + 1 < aumid.size())
        {
            return aumid.substr(bang + 1);
        }
        return aumid;
    }

    return L"Unknown";
}

HICON AppGrouping::ResolveIcon(HWND hwnd, const std::wstring& exePath)
{
    // Ask the window for its icon first; it is the most accurate.
    HICON icon = reinterpret_cast<HICON>(SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 200, nullptr));
    if (!icon)
    {
        icon = reinterpret_cast<HICON>(SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 200, nullptr));
    }
    if (!icon)
    {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
    }
    if (!icon)
    {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
    }
    if (!icon && !exePath.empty())
    {
        // ExtractIcon returns a handle the caller must destroy; we accept the
        // small leak over the lifetime of a switcher invocation for simplicity.
        icon = ExtractIconW(GetModuleHandleW(nullptr), exePath.c_str(), 0);
    }
    return icon;
}

void AppGrouping::ResolveIdentity(WindowInfo& info)
{
    const std::wstring exePath = ExecutablePath(info.processId);
    const std::wstring exeLower = ToLower(exePath);

    std::wstring aumid;
    const bool isFrameHost = exeLower.find(L"applicationframehost.exe") != std::wstring::npos;
    if (isFrameHost || exeLower.find(L"\\windowsapps\\") != std::wstring::npos)
    {
        aumid = ResolveAumid(info.hwnd);
    }

    if (!aumid.empty())
    {
        info.appKey = ToLower(aumid);
    }
    else if (!exeLower.empty())
    {
        info.appKey = exeLower;
    }
    else
    {
        info.appKey = ToLower(info.title);
    }

    info.appName = FriendlyName(isFrameHost ? std::wstring{} : exePath, aumid);
    info.icon = ResolveIcon(info.hwnd, exePath);
}

std::vector<AppGroup> AppGrouping::Group(std::vector<WindowInfo> windows)
{
    std::vector<AppGroup> groups;
    std::unordered_map<std::wstring, size_t> indexByKey;

    // User-defined rename / custom-grouping rules.
    const std::vector<GroupRule> rules = Settings::LoadRules();

    for (auto& window : windows)
    {
        ResolveIdentity(window);
        const std::wstring originalName = window.appName; // for authoring rename rules

        // A matching rule overrides the app's group key + display name.
        std::wstring ruleKey, ruleName;
        if (Settings::ApplyRules(rules, window.appKey, window.appName, ruleKey, ruleName))
        {
            window.appKey = ruleKey;
            window.appName = ruleName;
        }

        auto it = indexByKey.find(window.appKey);
        if (it == indexByKey.end())
        {
            AppGroup group{};
            group.key = window.appKey;
            group.name = window.appName;
            group.matchHint = originalName;
            group.icon = window.icon;
            indexByKey[window.appKey] = groups.size();
            group.windows.push_back(window);
            groups.push_back(std::move(group));
        }
        else
        {
            groups[it->second].windows.push_back(window);
        }
    }

    return groups;
}
