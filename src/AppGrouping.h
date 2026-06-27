#pragma once

#include "WindowEnumerator.h"

#include <string>
#include <vector>

// A set of windows that all belong to the same application.
struct AppGroup
{
    std::wstring key; // normalized app identity
    std::wstring name; // header text shown above the group
    std::wstring matchHint; // original app name, used to author rename rules
    HICON icon = nullptr; // representative icon (not owned)
    std::vector<WindowInfo> windows; // windows in MRU order (front = most recent)
};

// Resolves the app identity of each window and clusters windows that share an
// identity into AppGroups. Group order follows the most-recently-used order of
// the first window seen for each app, matching the Z-order returned by the
// enumerator.
class AppGrouping
{
public:
    static std::vector<AppGroup> Group(std::vector<WindowInfo> windows);

private:
    static void ResolveIdentity(WindowInfo& info);
    static std::wstring ResolveAumid(HWND hwnd);
    static std::wstring ExecutablePath(DWORD processId);
    static std::wstring FriendlyName(const std::wstring& exePath, const std::wstring& aumid);
    static HICON ResolveIcon(HWND hwnd, const std::wstring& exePath);
};
