#pragma once

#include <Windows.h>
#include <shellapi.h>

// A minimal notification-area (system tray) icon. It gives the user a way to
// quit gracefully, since the app otherwise silently owns Win+Tab while running.
class TrayIcon
{
public:
    bool Create(HINSTANCE hinstance, DWORD mainThreadId);
    ~TrayIcon();

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void ShowMenu();

    HINSTANCE m_hinstance = nullptr;
    DWORD m_mainThreadId = 0;
    HWND m_hwnd = nullptr;
    NOTIFYICONDATAW m_nid{};
};
