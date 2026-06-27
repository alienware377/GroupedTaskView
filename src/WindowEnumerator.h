#pragma once

#include <Windows.h>
#include <string>
#include <vector>

// A single top-level window that is eligible to appear in the Alt+Tab switcher.
struct WindowInfo
{
    HWND hwnd = nullptr;
    DWORD processId = 0;
    std::wstring title;

    // App identity, filled in by AppGrouping.
    std::wstring appKey; // normalized identity used for grouping (AUMID or lowercased exe path)
    std::wstring appName; // human friendly app name shown as the group header
    HICON icon = nullptr; // best-effort window/app icon (not owned, do not destroy)
};

// Enumerates the windows that the shell would show in the standard Alt+Tab list,
// using the well known "alt-tab test": visible, titled, not cloaked, not a
// tool window unless explicitly an app window, and the root owner of its chain.
class WindowEnumerator
{
public:
    static std::vector<WindowInfo> Enumerate();

private:
    static bool IsAltTabWindow(HWND hwnd);
    static bool IsCloaked(HWND hwnd);
};
