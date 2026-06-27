#pragma once

#include "TaskView.h"

#include <Windows.h>
#include <thread>

// Core controller for the grouped Task View. It installs a low-level keyboard
// hook to intercept Win+Tab before the shell opens the built-in Task View, and
// toggles a full-screen, app-grouped overlay (TaskView) in its place.
//
// The hook only decides whether to swallow Win+Tab and posts the heavy work to a
// hidden control window so it never blocks long enough for Windows to drop it.
// Once the overlay is open it is the foreground window and handles its own
// keyboard and mouse input directly.
class AltTabGrouped
{
public:
    AltTabGrouped(HINSTANCE hinstance, DWORD mainThreadId);
    ~AltTabGrouped();

    AltTabGrouped(const AltTabGrouped&) = delete;
    AltTabGrouped& operator=(const AltTabGrouped&) = delete;

    // Opens the overlay once without a keypress (used by the --selftest launch
    // to validate rendering, since injected Win+Tab is intentionally ignored).
    void ShowForSelfTest();

private:
    static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam);
    bool HandleKey(WPARAM message, const KBDLLHOOKSTRUCT& info); // returns true to swallow

    static LRESULT CALLBACK ControlWndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT ControlWndProc(HWND, UINT, WPARAM, LPARAM);

    void HookThreadMain(); // owns the keyboard hook + its message pump
    static void SuppressStartMenu();

    HINSTANCE m_hinstance = nullptr;
    DWORD m_mainThreadId = 0;
    HWND m_controlWnd = nullptr;

    // The low-level keyboard hook lives on its own thread so it is never starved
    // by heavy UI work on the main thread (which would let Windows drop it).
    std::thread m_hookThread;
    DWORD m_hookThreadId = 0;
    HHOOK m_keyboardHook = nullptr;
    HANDLE m_hookReady = nullptr;

    TaskView m_taskView;
    bool m_winDown = false;

    static AltTabGrouped* s_instance;
};
