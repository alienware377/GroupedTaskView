#include "pch.h"
#include "WindowEnumerator.h"

#include <dwmapi.h>

// DWMWA_CLOAKED is only available when targeting Win8+; guard for older SDKs.
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

// Cloak reasons. Only DWM_CLOAKED_SHELL (suspended UWP / system-hidden UI such
// as "Windows Input Experience") should be excluded; DWM_CLOAKED_APP windows are
// real app windows (e.g. Chrome/Explorer windows, including ones on another
// virtual desktop) that the user still wants to see and switch to.
#ifndef DWM_CLOAKED_APP
#define DWM_CLOAKED_APP 0x0000001
#define DWM_CLOAKED_SHELL 0x0000002
#define DWM_CLOAKED_INHERITED 0x0000004
#endif

namespace
{
    // Returns the "last visible active popup" of the owner chain, mirroring the
    // heuristic the shell uses to decide which window of an owner chain is the
    // one that belongs in Alt+Tab.
    HWND GetLastVisibleActivePopup(HWND hwnd)
    {
        HWND root = GetAncestor(hwnd, GA_ROOTOWNER);
        HWND lastPopup = root;
        HWND walk = root;
        while (walk)
        {
            HWND popup = GetLastActivePopup(walk);
            if (popup == walk)
            {
                break;
            }
            if (IsWindowVisible(popup))
            {
                lastPopup = popup;
                break;
            }
            walk = popup;
        }
        return lastPopup;
    }
}

bool WindowEnumerator::IsCloaked(HWND hwnd)
{
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
    {
        // Exclude only shell-cloaked windows (suspended UWP / hidden system UI),
        // not app-cloaked ones (real windows, incl. those on other desktops).
        return (cloaked & DWM_CLOAKED_SHELL) != 0;
    }
    return false;
}

bool WindowEnumerator::IsAltTabWindow(HWND hwnd)
{
    if (!IsWindowVisible(hwnd))
    {
        return false;
    }

    // Must have a non-empty title.
    if (GetWindowTextLengthW(hwnd) == 0)
    {
        return false;
    }

    // The window has to be the representative window of its owner chain.
    if (GetLastVisibleActivePopup(hwnd) != hwnd)
    {
        return false;
    }

    // NOTE: cloaking is intentionally NOT used to exclude here. Windows on other
    // virtual desktops are cloaked too, and we want them in the grouped view.
    // Suspended-UWP / system junk is filtered later by virtual-desktop membership
    // (see TaskView::BuildModel).

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    // App windows are always shown; tool windows never are.
    if (exStyle & WS_EX_APPWINDOW)
    {
        return true;
    }
    if (exStyle & WS_EX_TOOLWINDOW)
    {
        return false;
    }

    // Skip windows owned by another window (dialogs etc.) unless they opted in
    // via WS_EX_APPWINDOW above.
    if (GetWindow(hwnd, GW_OWNER) != nullptr)
    {
        return false;
    }

    return true;
}

std::vector<WindowInfo> WindowEnumerator::Enumerate()
{
    std::vector<WindowInfo> result;

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* out = reinterpret_cast<std::vector<WindowInfo>*>(lParam);
            if (!IsAltTabWindow(hwnd))
            {
                return TRUE;
            }

            WindowInfo info{};
            info.hwnd = hwnd;
            GetWindowThreadProcessId(hwnd, &info.processId);

            wchar_t title[512] = {};
            GetWindowTextW(hwnd, title, ARRAYSIZE(title));
            info.title = title;

            out->push_back(std::move(info));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));

    return result;
}
