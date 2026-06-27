#include "pch.h"
#include "AltTabGrouped.h"
#include "Logging.h"

namespace
{
    const wchar_t ControlWindowClassName[] = L"GroupedTaskView_Control";

    enum ControlMessage : UINT
    {
        WM_ATG_TOGGLE = WM_APP + 1,
    };
}

AltTabGrouped* AltTabGrouped::s_instance = nullptr;

AltTabGrouped::AltTabGrouped(HINSTANCE hinstance, DWORD mainThreadId) :
    m_hinstance(hinstance), m_mainThreadId(mainThreadId)
{
    s_instance = this;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = ControlWndProcStatic;
    wc.hInstance = hinstance;
    wc.lpszClassName = ControlWindowClassName;
    RegisterClassExW(&wc);
    m_controlWnd = CreateWindowExW(0, ControlWindowClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hinstance, this);

    m_taskView.Initialize(hinstance);

    // Run the hook on a dedicated thread; wait until it is installed.
    m_hookReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_hookThread = std::thread(&AltTabGrouped::HookThreadMain, this);
    if (m_hookReady)
    {
        WaitForSingleObject(m_hookReady, 5000);
    }
}

void AltTabGrouped::HookThreadMain()
{
    m_hookThreadId = GetCurrentThreadId();

    m_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, m_hinstance, 0);
    if (!m_keyboardHook)
    {
        Logger::error(L"Failed to install the low-level keyboard hook for AltTabGrouped");
    }
    else
    {
        Logger::info(L"AltTabGrouped keyboard hook installed (Win+Tab) on dedicated thread");
    }

    if (m_hookReady)
    {
        SetEvent(m_hookReady);
    }

    // Minimal pump: this thread does nothing but service the hook, so the hook
    // is never starved by UI work happening on the main thread.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (m_keyboardHook)
    {
        UnhookWindowsHookEx(m_keyboardHook);
        m_keyboardHook = nullptr;
    }
}

AltTabGrouped::~AltTabGrouped()
{
    if (m_hookThreadId)
    {
        PostThreadMessageW(m_hookThreadId, WM_QUIT, 0, 0);
    }
    if (m_hookThread.joinable())
    {
        m_hookThread.join();
    }
    if (m_hookReady)
    {
        CloseHandle(m_hookReady);
        m_hookReady = nullptr;
    }
    if (m_controlWnd)
    {
        DestroyWindow(m_controlWnd);
        m_controlWnd = nullptr;
    }
    s_instance = nullptr;
}

LRESULT CALLBACK AltTabGrouped::KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && s_instance)
    {
        const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        // Ignore our own synthetic events (e.g. the Start-menu suppression key).
        if (!(info->flags & LLKHF_INJECTED))
        {
            if (s_instance->HandleKey(wParam, *info))
            {
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool AltTabGrouped::HandleKey(WPARAM message, const KBDLLHOOKSTRUCT& info)
{
    const bool keyDown = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
    const bool keyUp = (message == WM_KEYUP || message == WM_SYSKEYUP);

    if (info.vkCode == VK_LWIN || info.vkCode == VK_RWIN)
    {
        if (keyDown)
        {
            m_winDown = true;
        }
        else if (keyUp)
        {
            m_winDown = false;
        }
        return false;
    }

    // Win+Tab toggles our grouped Task View instead of the shell's.
    if (keyDown && info.vkCode == VK_TAB && m_winDown)
    {
        PostMessageW(m_controlWnd, WM_ATG_TOGGLE, 0, 0);
        // The shell never saw the Tab, so Win looks like a lone tap that would
        // pop Start on release; mark it as "used" with a throwaway key.
        SuppressStartMenu();
        return true;
    }

    return false;
}

// Inject a reserved no-op key so Explorer treats the Win press as part of a
// combo and does not open Start when Win is released.
void AltTabGrouped::SuppressStartMenu()
{
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = 0xFF; // VK reserved / no-op
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 0xFF;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT));
}

void AltTabGrouped::ShowForSelfTest()
{
    PostMessageW(m_controlWnd, WM_ATG_TOGGLE, 0, 0);
}

LRESULT CALLBACK AltTabGrouped::ControlWndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* self = (msg == WM_NCCREATE)
                     ? reinterpret_cast<AltTabGrouped*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams)
                     : reinterpret_cast<AltTabGrouped*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->ControlWndProc(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT AltTabGrouped::ControlWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ATG_TOGGLE:
        m_taskView.Toggle();
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
