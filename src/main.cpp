#include "pch.h"

#include "AltTabGrouped.h"
#include "TrayIcon.h"

// GroupedTaskView - a standalone Win+Tab replacement that shows a Task-View-style
// overlay with windows grouped by application.

const wchar_t kInstanceMutex[] = L"Local\\GroupedTaskView_SingleInstance";

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ PWSTR lpCmdLine, _In_ int)
{
    // Single instance.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    const DWORD mainThreadId = GetCurrentThreadId();
    const std::wstring args = lpCmdLine ? std::wstring(lpCmdLine) : std::wstring();
    const bool selftest = args.find(L"selftest") != std::wstring::npos;

    {
        AltTabGrouped app(hInstance, mainThreadId);

        // Tray icon gives the user a graceful way to quit, since the app owns
        // Win+Tab while running.
        TrayIcon tray;
        tray.Create(hInstance, mainThreadId);

        if (selftest)
        {
            app.ShowForSelfTest();
        }

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    return 0;
}
