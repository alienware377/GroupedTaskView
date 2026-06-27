#include "pch.h"
#include "SettingsWindow.h"
#include "Settings.h"
#include "resource.h"

#include <commctrl.h>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace
{
    const wchar_t kClass[] = L"GroupedTaskView_Settings";

    enum : int
    {
        ID_LIST = 1001,
        ID_MATCH = 1002,
        ID_NAME = 1003,
        ID_ADD = 1004,
        ID_UPDATE = 1005,
        ID_REMOVE = 1006,
        ID_SAVE = 1007,
        ID_CANCEL = 1008,
    };

    struct State
    {
        HWND list = nullptr, match = nullptr, name = nullptr;
        std::vector<GroupRule> rules;
    };

    HWND g_wnd = nullptr;
    std::wstring g_prefillMatch, g_prefillName;

    std::wstring GetText(HWND edit)
    {
        const int n = GetWindowTextLengthW(edit);
        std::wstring s(n, L'\0');
        if (n) GetWindowTextW(edit, s.data(), n + 1);
        return s;
    }

    void RefreshList(State* st)
    {
        ListView_DeleteAllItems(st->list);
        for (int i = 0; i < static_cast<int>(st->rules.size()); ++i)
        {
            LVITEMW it{};
            it.mask = LVIF_TEXT;
            it.iItem = i;
            it.pszText = st->rules[i].match.data();
            ListView_InsertItem(st->list, &it);
            ListView_SetItemText(st->list, i, 1, st->rules[i].name.data());
        }
    }

    int SelectedRow(State* st)
    {
        return ListView_GetNextItem(st->list, -1, LVNI_SELECTED);
    }

    HWND MakeButton(HWND parent, HINSTANCE hi, int id, const wchar_t* text, int x, int y, int w, int h)
    {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hi, nullptr);
    }
    HWND MakeStatic(HWND parent, HINSTANCE hi, const wchar_t* text, int x, int y, int w, int h, DWORD extra = 0)
    {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | extra, x, y, w, h, parent, nullptr, hi, nullptr);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg)
        {
        case WM_CREATE:
        {
            st = new State();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            HINSTANCE hi = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

            MakeStatic(hwnd, hi,
                       L"Group windows by app. Add a rule to rename a group, or to merge apps: any window whose "
                       L"app name or .exe contains “Match” is placed in “Group name”. Give several "
                       L"rules the same Group name to merge those apps together.",
                       16, 12, 588, 56);

            st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                       WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                       16, 76, 588, 196, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)), hi, nullptr);
            ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.cx = 250; col.pszText = const_cast<wchar_t*>(L"Match (app name or .exe)");
            ListView_InsertColumn(st->list, 0, &col);
            col.cx = 320; col.pszText = const_cast<wchar_t*>(L"Group name");
            ListView_InsertColumn(st->list, 1, &col);

            MakeStatic(hwnd, hi, L"Match:", 16, 290, 50, 20);
            st->match = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                        70, 288, 230, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MATCH)), hi, nullptr);
            MakeStatic(hwnd, hi, L"Group name:", 314, 290, 80, 20);
            st->name = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                       398, 288, 206, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_NAME)), hi, nullptr);

            MakeButton(hwnd, hi, ID_ADD, L"Add", 16, 324, 90, 28);
            MakeButton(hwnd, hi, ID_UPDATE, L"Update selected", 112, 324, 130, 28);
            MakeButton(hwnd, hi, ID_REMOVE, L"Remove selected", 248, 324, 130, 28);

            MakeButton(hwnd, hi, ID_SAVE, L"Save && Close", 404, 372, 120, 30);
            MakeButton(hwnd, hi, ID_CANCEL, L"Cancel", 532, 372, 72, 30);

            st->rules = Settings::LoadRules();
            RefreshList(st);
            if (!g_prefillMatch.empty() || !g_prefillName.empty())
            {
                SetWindowTextW(st->match, g_prefillMatch.c_str());
                SetWindowTextW(st->name, g_prefillName.c_str());
                SetFocus(st->name);
                SendMessageW(st->name, EM_SETSEL, 0, -1);
            }
            return 0;
        }

        case WM_NOTIFY:
        {
            auto* nm = reinterpret_cast<LPNMHDR>(lParam);
            if (nm->idFrom == ID_LIST && nm->code == LVN_ITEMCHANGED)
            {
                const int sel = SelectedRow(st);
                if (sel >= 0 && sel < static_cast<int>(st->rules.size()))
                {
                    SetWindowTextW(st->match, st->rules[sel].match.c_str());
                    SetWindowTextW(st->name, st->rules[sel].name.c_str());
                }
            }
            return 0;
        }

        case WM_COMMAND:
        {
            const int id = LOWORD(wParam);
            if (id == ID_ADD)
            {
                std::wstring m = GetText(st->match), n = GetText(st->name);
                if (!m.empty() && !n.empty())
                {
                    st->rules.push_back(GroupRule{ m, n });
                    RefreshList(st);
                }
            }
            else if (id == ID_UPDATE)
            {
                const int sel = SelectedRow(st);
                if (sel >= 0)
                {
                    st->rules[sel] = GroupRule{ GetText(st->match), GetText(st->name) };
                    RefreshList(st);
                }
            }
            else if (id == ID_REMOVE)
            {
                const int sel = SelectedRow(st);
                if (sel >= 0)
                {
                    st->rules.erase(st->rules.begin() + sel);
                    RefreshList(st);
                }
            }
            else if (id == ID_SAVE)
            {
                Settings::SaveRules(st->rules);
                DestroyWindow(hwnd);
            }
            else if (id == ID_CANCEL)
            {
                DestroyWindow(hwnd);
            }
            return 0;
        }

        case WM_DESTROY:
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            g_wnd = nullptr;
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }
}

void ShowSettingsWindow(HINSTANCE hinstance, const wchar_t* prefillMatch, const wchar_t* prefillName)
{
    g_prefillMatch = prefillMatch ? prefillMatch : L"";
    g_prefillName = prefillName ? prefillName : L"";

    if (g_wnd && IsWindow(g_wnd))
    {
        SetForegroundWindow(g_wnd);
        return;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        wc.hIcon = LoadIconW(hinstance, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassExW(&wc);
        registered = true;
    }

    const int w = 636, h = 452;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_wnd = CreateWindowExW(0, kClass, L"Grouped Task View — Settings",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            x, y, w, h, nullptr, nullptr, hinstance, nullptr);
    if (g_wnd)
    {
        ShowWindow(g_wnd, SW_SHOW);
        SetForegroundWindow(g_wnd);
    }
}
