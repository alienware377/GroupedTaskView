#include "pch.h"
#include "TrayIcon.h"
#include "resource.h"
#include "SettingsWindow.h"
#include "Settings.h"

namespace
{
    const wchar_t TrayWindowClassName[] = L"GroupedTaskView_Tray";
    constexpr UINT WM_ATG_TRAY = WM_APP + 100;
    constexpr UINT ID_TRAY_EXIT = 1;
    constexpr UINT ID_TRAY_TITLE = 2;
    constexpr UINT ID_TRAY_SETTINGS = 3;
    constexpr UINT ID_TRAY_OVERRIDE = 4;
}

bool TrayIcon::Create(HINSTANCE hinstance, DWORD mainThreadId)
{
    m_hinstance = hinstance;
    m_mainThreadId = mainThreadId;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = hinstance;
    wc.lpszClassName = TrayWindowClassName;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(0, TrayWindowClassName, L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, hinstance, this);
    if (!m_hwnd)
    {
        return false;
    }

    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = m_hwnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_ATG_TRAY;
    m_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
    if (!m_nid.hIcon)
    {
        m_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    wcscpy_s(m_nid.szTip, L"Grouped Task View");
    return Shell_NotifyIconW(NIM_ADD, &m_nid) == TRUE;
}

TrayIcon::~TrayIcon()
{
    if (m_hwnd)
    {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        DestroyWindow(m_hwnd);
    }
}

LRESULT CALLBACK TrayIcon::WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* self = (msg == WM_NCCREATE)
                     ? reinterpret_cast<TrayIcon*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams)
                     : reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->WndProc(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TrayIcon::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ATG_TRAY:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
        {
            ShowMenu();
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_EXIT)
        {
            PostThreadMessageW(m_mainThreadId, WM_QUIT, 0, 0);
        }
        else if (LOWORD(wParam) == ID_TRAY_SETTINGS)
        {
            ShowSettingsWindow(m_hinstance);
        }
        else if (LOWORD(wParam) == ID_TRAY_OVERRIDE)
        {
            Settings::SetOverrideSystemTaskView(!Settings::OverrideSystemTaskView());
        }
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void TrayIcon::ShowMenu()
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_TRAY_TITLE, L"Grouped Task View — running");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (Settings::OverrideSystemTaskView() ? MF_CHECKED : MF_UNCHECKED),
                ID_TRAY_OVERRIDE, L"Replace system Task View (taskbar button, swipe)");
    AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS, L"Settings…");
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    // Required so the menu dismisses correctly when the user clicks elsewhere.
    SetForegroundWindow(m_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}
