#pragma once

#include <Windows.h>

#include <string>
#include <vector>

// Lightweight virtual-desktop access that avoids the undocumented
// IVirtualDesktopManagerInternal COM interface (whose GUIDs churn between
// Windows builds). Instead it reads the desktop list/names/current index from
// the registry and navigates by synthesizing the Ctrl+Win+Arrow / Ctrl+Win+D
// shortcuts, which are stable across builds.
struct DesktopInfo
{
    GUID id{}; // desktop identity, used to map windows to desktops
    std::wstring name;
    std::wstring wallpaperPath; // per-desktop wallpaper, or the system wallpaper
    bool isCurrent = false;
};

class VirtualDesktops
{
public:
    // Snapshot of the current desktops, in order.
    static std::vector<DesktopInfo> Enumerate(int& currentIndex);

    // Switch from currentIndex to targetIndex using Ctrl+Win+Left/Right.
    static void SwitchTo(int targetIndex, int currentIndex);

    // Create a new desktop (Ctrl+Win+D).
    static void CreateNew();
};
