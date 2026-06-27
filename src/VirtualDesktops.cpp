#include "pch.h"
#include "VirtualDesktops.h"

#include <objbase.h>

namespace
{
    const wchar_t VirtualDesktopsKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";

    std::vector<GUID> ReadGuidList()
    {
        std::vector<GUID> guids;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, VirtualDesktopsKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            return guids;
        }

        DWORD type = 0;
        DWORD size = 0;
        if (RegQueryValueExW(key, L"VirtualDesktopIDs", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_BINARY && size >= sizeof(GUID))
        {
            std::vector<BYTE> blob(size);
            if (RegQueryValueExW(key, L"VirtualDesktopIDs", nullptr, nullptr, blob.data(), &size) == ERROR_SUCCESS)
            {
                const size_t count = size / sizeof(GUID);
                guids.resize(count);
                memcpy(guids.data(), blob.data(), count * sizeof(GUID));
            }
        }
        RegCloseKey(key);
        return guids;
    }

    GUID ReadCurrentGuid()
    {
        GUID current{};
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, VirtualDesktopsKey, 0, KEY_READ, &key) == ERROR_SUCCESS)
        {
            DWORD size = sizeof(GUID);
            RegQueryValueExW(key, L"CurrentVirtualDesktop", nullptr, nullptr, reinterpret_cast<BYTE*>(&current), &size);
            RegCloseKey(key);
        }
        return current;
    }

    std::wstring ReadDesktopString(const GUID& guid, const wchar_t* valueName)
    {
        wchar_t guidStr[64] = {};
        StringFromGUID2(guid, guidStr, ARRAYSIZE(guidStr));

        std::wstring subKey = std::wstring(VirtualDesktopsKey) + L"\\Desktops\\" + guidStr;
        HKEY key = nullptr;
        std::wstring value;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS)
        {
            wchar_t buffer[1024] = {};
            DWORD size = sizeof(buffer);
            DWORD type = 0;
            if (RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size) == ERROR_SUCCESS && type == REG_SZ && buffer[0])
            {
                value = buffer;
            }
            RegCloseKey(key);
        }
        return value;
    }

    std::wstring CurrentWallpaper()
    {
        wchar_t path[MAX_PATH] = {};
        if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) && path[0])
        {
            return path;
        }
        return std::wstring();
    }

    void SendChord(WORD vk)
    {
        INPUT inputs[6] = {};
        auto key = [](INPUT& in, WORD code, bool up) {
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = code;
            in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        };
        key(inputs[0], VK_LCONTROL, false);
        key(inputs[1], VK_LWIN, false);
        key(inputs[2], vk, false);
        key(inputs[3], vk, true);
        key(inputs[4], VK_LWIN, true);
        key(inputs[5], VK_LCONTROL, true);
        SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT));
    }
}

std::vector<DesktopInfo> VirtualDesktops::Enumerate(int& currentIndex)
{
    currentIndex = 0;
    std::vector<DesktopInfo> result;

    const std::vector<GUID> guids = ReadGuidList();
    const GUID current = ReadCurrentGuid();
    const std::wstring fallbackWallpaper = CurrentWallpaper();

    for (size_t i = 0; i < guids.size(); ++i)
    {
        DesktopInfo info{};
        info.id = guids[i];
        info.name = ReadDesktopString(guids[i], L"Name");
        if (info.name.empty())
        {
            info.name = L"Desktop " + std::to_wstring(i + 1);
        }
        info.wallpaperPath = ReadDesktopString(guids[i], L"Wallpaper");
        if (info.wallpaperPath.empty())
        {
            info.wallpaperPath = fallbackWallpaper;
        }
        info.isCurrent = IsEqualGUID(guids[i], current) != 0;
        if (info.isCurrent)
        {
            currentIndex = static_cast<int>(i);
        }
        result.push_back(std::move(info));
    }

    // Always present at least the current desktop so the strip is never empty.
    if (result.empty())
    {
        DesktopInfo only{};
        only.name = L"Desktop 1";
        only.wallpaperPath = fallbackWallpaper;
        only.isCurrent = true;
        result.push_back(std::move(only));
    }
    return result;
}

void VirtualDesktops::SwitchTo(int targetIndex, int currentIndex)
{
    if (targetIndex == currentIndex)
    {
        return;
    }
    const WORD vk = (targetIndex > currentIndex) ? VK_RIGHT : VK_LEFT;
    const int steps = (targetIndex > currentIndex) ? (targetIndex - currentIndex) : (currentIndex - targetIndex);
    for (int i = 0; i < steps; ++i)
    {
        SendChord(vk);
    }
}

void VirtualDesktops::CreateNew()
{
    SendChord('D');
}
