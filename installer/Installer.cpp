// Self-contained installer for Grouped Task View. The application executable is
// embedded as a resource; running this installs it to the user's profile, adds
// Start Menu + Startup shortcuts and an Add/Remove Programs entry, and starts it.
// Run with /uninstall to remove everything. No admin rights required (per-user).

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <string>

#include "resource.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
    const wchar_t kAppName[] = L"Grouped Task View";
    const wchar_t kExeName[] = L"GroupedTaskView.exe";
    const wchar_t kProcName[] = L"GroupedTaskView.exe";
    const wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\GroupedTaskView";
    const wchar_t kVersion[] = L"1.0.0";

    std::wstring KnownDir(REFKNOWNFOLDERID id)
    {
        PWSTR p = nullptr;
        std::wstring result;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p)
        {
            result = p;
        }
        if (p) CoTaskMemFree(p);
        return result;
    }

    std::wstring InstallDir() { return KnownDir(FOLDERID_LocalAppData) + L"\\GroupedTaskView"; }
    std::wstring SelfPath()
    {
        wchar_t buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return buf;
    }

    bool WriteEmbeddedExe(const std::wstring& path)
    {
        HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_APPEXE), RT_RCDATA);
        if (!res) return false;
        HGLOBAL h = LoadResource(nullptr, res);
        if (!h) return false;
        void* data = LockResource(h);
        DWORD size = SizeofResource(nullptr, res);
        if (!data || !size) return false;

        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const BOOL ok = WriteFile(f, data, size, &written, nullptr);
        CloseHandle(f);
        return ok && written == size;
    }

    bool CreateShortcut(const std::wstring& target, const std::wstring& linkPath, const std::wstring& desc, const std::wstring& workingDir)
    {
        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        {
            return false;
        }
        link->SetPath(target.c_str());
        link->SetDescription(desc.c_str());
        link->SetWorkingDirectory(workingDir.c_str());
        link->SetIconLocation(target.c_str(), 0);

        IPersistFile* file = nullptr;
        bool ok = false;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))))
        {
            ok = SUCCEEDED(file->Save(linkPath.c_str(), TRUE));
            file->Release();
        }
        link->Release();
        return ok;
    }

    std::wstring StartMenuLink() { return KnownDir(FOLDERID_Programs) + L"\\" + kAppName + L".lnk"; }
    std::wstring StartupLink() { return KnownDir(FOLDERID_Startup) + L"\\" + kAppName + L".lnk"; }

    void SetUninstallKey(const std::wstring& dir, const std::wstring& uninstaller)
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        {
            return;
        }
        auto setStr = [&](const wchar_t* name, const std::wstring& val) {
            RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()), static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
        };
        const std::wstring exe = dir + L"\\" + kExeName;
        const std::wstring uninstallCmd = L"\"" + uninstaller + L"\" /uninstall";
        setStr(L"DisplayName", kAppName);
        setStr(L"DisplayVersion", kVersion);
        setStr(L"Publisher", L"GroupedTaskView");
        setStr(L"DisplayIcon", exe);
        setStr(L"InstallLocation", dir);
        setStr(L"UninstallString", uninstallCmd);
        DWORD one = 1;
        RegSetValueExW(key, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
        RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
        RegCloseKey(key);
    }

    void KillRunning()
    {
        // Best-effort: signal any running instance to exit via taskkill.
        std::wstring cmd = L"taskkill /IM ";
        cmd += kProcName;
        cmd += L" /F";
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::wstring mutable_cmd = cmd;
        if (CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            WaitForSingleObject(pi.hProcess, 4000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    void RemoveDirRecursive(const std::wstring& dir)
    {
        std::wstring from = dir;
        from.push_back(L'\0'); // double-null terminated
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_NO_UI;
        SHFileOperationW(&op);
    }

    int Install()
    {
        const int answer = MessageBoxW(nullptr,
            L"Install Grouped Task View?\r\n\r\n"
            L"This replaces Win+Tab with a Task View that groups your windows by app, "
            L"and starts automatically when you sign in.\r\n\r\n"
            L"It installs to your user profile (no administrator rights needed).",
            kAppName, MB_OKCANCEL | MB_ICONINFORMATION);
        if (answer != IDOK)
        {
            return 0;
        }

        KillRunning();
        const std::wstring dir = InstallDir();
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

        const std::wstring exePath = dir + L"\\" + kExeName;
        if (!WriteEmbeddedExe(exePath))
        {
            MessageBoxW(nullptr, L"Failed to write the application files.", kAppName, MB_OK | MB_ICONERROR);
            return 1;
        }

        // Copy this installer in as the uninstaller.
        const std::wstring uninstaller = dir + L"\\uninstall.exe";
        CopyFileW(SelfPath().c_str(), uninstaller.c_str(), FALSE);

        CreateShortcut(exePath, StartMenuLink(), kAppName, dir);
        CreateShortcut(exePath, StartupLink(), kAppName, dir);
        SetUninstallKey(dir, uninstaller);

        // Launch it.
        ShellExecuteW(nullptr, L"open", exePath.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);

        MessageBoxW(nullptr,
            L"Grouped Task View is installed and running.\r\n\r\n"
            L"Press Win+Tab to open it. Quit anytime from its tray icon.",
            kAppName, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    int Uninstall()
    {
        const int answer = MessageBoxW(nullptr, L"Remove Grouped Task View?", kAppName, MB_OKCANCEL | MB_ICONQUESTION);
        if (answer != IDOK)
        {
            return 0;
        }

        KillRunning();
        DeleteFileW(StartMenuLink().c_str());
        DeleteFileW(StartupLink().c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);

        // Schedule the install dir (incl. the running uninstaller) for deletion on reboot,
        // then best-effort delete now.
        const std::wstring dir = InstallDir();
        RemoveDirRecursive(dir);
        MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);

        MessageBoxW(nullptr, L"Grouped Task View has been removed.", kAppName, MB_OK | MB_ICONINFORMATION);
        return 0;
    }
}

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR lpCmdLine, _In_ int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring args = lpCmdLine ? std::wstring(lpCmdLine) : std::wstring();
    const int rc = (args.find(L"/uninstall") != std::wstring::npos || args.find(L"uninstall") != std::wstring::npos)
                       ? Uninstall()
                       : Install();
    CoUninitialize();
    return rc;
}
