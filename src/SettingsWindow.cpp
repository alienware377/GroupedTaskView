#include "pch.h"
#include "SettingsWindow.h"
#include "Settings.h"
#include "resource.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <vector>

#pragma comment(lib, "comctl32.lib")

// A custom-painted, dark settings window in the same style as the app's other
// tools: purple caption, rounded cards, owner-drawn checkboxes and buttons. Only
// the two text boxes are real controls; everything else is painted with GDI+ and
// hit-tested by hand.

namespace
{
    const wchar_t kClass[] = L"GroupedTaskView_Settings";
    const wchar_t kAppName[] = L"Grouped Task View";

    enum : int { ID_MATCH = 1002, ID_NAME = 1003 };

    // Palette.
    const Gdiplus::Color kBg(255, 0x1C, 0x1D, 0x24);
    const Gdiplus::Color kPanel(255, 0x23, 0x25, 0x2E);
    const Gdiplus::Color kRowHover(255, 0x2C, 0x2E, 0x38);
    const Gdiplus::Color kRowSel(255, 0x38, 0x2A, 0x45);
    const Gdiplus::Color kBorder(255, 0x35, 0x38, 0x44);
    const Gdiplus::Color kInput(255, 0x2A, 0x2C, 0x35);
    const Gdiplus::Color kText(255, 0xEC, 0xEC, 0xF1);
    const Gdiplus::Color kMuted(255, 0x9A, 0x9C, 0xA8);
    const Gdiplus::Color kAccent(255, 0xA9, 0x4F, 0xC0);
    const Gdiplus::Color kAccentHover(255, 0xBC, 0x66, 0xD3);
    const Gdiplus::Color kAccentSoft(255, 0x5E, 0x33, 0x70);
    const Gdiplus::Color kAccentSoftHover(255, 0x74, 0x40, 0x89);
    const Gdiplus::Color kAccentText(255, 0xC9, 0x7D, 0xDD);
    const Gdiplus::Color kCheck(255, 0x1F, 0x6F, 0xEB);
    const Gdiplus::Color kChip(255, 0x33, 0x35, 0x40);
    const Gdiplus::Color kDanger(255, 0xE5, 0x53, 0x4B);
    const Gdiplus::Color kDangerBorder(255, 0x5C, 0x2B, 0x30);
    const Gdiplus::Color kDangerHover(255, 0x3A, 0x22, 0x26);
    const Gdiplus::Color kButton(255, 0x2E, 0x30, 0x38);
    const Gdiplus::Color kButtonHover(255, 0x3A, 0x3C, 0x46);
    const COLORREF kInputRef = RGB(0x2A, 0x2C, 0x35);
    const COLORREF kTextRef = RGB(0xEC, 0xEC, 0xF1);

    // Layout, in 96-DPI units; scaled by the window's DPI at paint time.
    const int kW = 560, kH = 664, kPad = 24;
    const int kRowH = 44, kListY = 262, kListH = 5 * kRowH + 12;
    const int kEditLabelY = kListY + kListH + 14, kEditY = kEditLabelY + 20, kEditH = 36;
    const int kMatchX = kPad, kMatchW = 168, kNameX = kMatchX + kMatchW + 10, kNameW = 168;
    const int kAddX = kNameX + kNameW + 10, kAddW = 84, kClearX = kAddX + kAddW + 8, kClearW = kW - kPad - (kAddX + kAddW + 8);
    const int kFooterY = kEditY + kEditH + 22;
    const int kBtnY = kH - kPad - 38, kBtnH = 38, kCancelW = 100, kSaveW = 110;

    // Clickable things, for hover and hit-testing.
    enum Hit : int
    {
        HIT_NONE = 0, HIT_OVERRIDE, HIT_STARTUP, HIT_ADD, HIT_CLEAR, HIT_SAVE, HIT_CANCEL,
        HIT_ROW = 100,      // + row index
        HIT_REMOVE = 10000, // + row index
    };

    struct State
    {
        HWND hwnd = nullptr, match = nullptr, name = nullptr;
        HFONT editFont = nullptr;
        HBRUSH inputBrush = nullptr;
        std::vector<GroupRule> rules;
        bool overrideTV = false, startup = false;
        int selected = -1, hover = HIT_NONE, scroll = 0;
        HWND focusEdit = nullptr;
        UINT dpi = 96;
    };

    HWND g_wnd = nullptr;

    int S(const State* st, int v) { return MulDiv(v, static_cast<int>(st->dpi), 96); }
    float SF(const State* st, float v) { return v * st->dpi / 96.0f; }

    std::wstring GetText(HWND edit)
    {
        const int n = GetWindowTextLengthW(edit);
        std::wstring s(n, L'\0');
        if (n) GetWindowTextW(edit, s.data(), n + 1);
        return s;
    }

    std::wstring Trim(std::wstring s)
    {
        const auto b = s.find_first_not_of(L" \t");
        if (b == std::wstring::npos) return L"";
        const auto e = s.find_last_not_of(L" \t");
        return s.substr(b, e - b + 1);
    }

    // ---- Start-with-Windows shortcut (same one the installer creates) ----

    std::wstring StartupLinkPath()
    {
        PWSTR p = nullptr;
        std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &p)))
            out = std::wstring(p) + L"\\" + kAppName + L".lnk";
        CoTaskMemFree(p);
        return out;
    }

    bool StartupEnabled()
    {
        const std::wstring p = StartupLinkPath();
        return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    void SetStartupEnabled(bool on)
    {
        const std::wstring link = StartupLinkPath();
        if (link.empty()) return;
        if (!on)
        {
            DeleteFileW(link.c_str());
            return;
        }
        if (StartupEnabled()) return;
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        dir = dir.substr(0, dir.find_last_of(L'\\'));

        IShellLinkW* sl = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
        sl->SetPath(exe);
        sl->SetWorkingDirectory(dir.c_str());
        sl->SetDescription(kAppName);
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))))
        {
            pf->Save(link.c_str(), TRUE);
            pf->Release();
        }
        sl->Release();
    }

    // ---- Geometry helpers ----

    RECT R(const State* st, int x, int y, int w, int h)
    {
        return RECT{ S(st, x), S(st, y), S(st, x + w), S(st, y + h) };
    }

    RECT CheckRowRect(const State* st, int i) { return R(st, kPad, 92 + i * 36, kW - 2 * kPad, 32); }
    RECT ListRect(const State* st) { return R(st, kPad, kListY, kW - 2 * kPad, kListH); }
    RECT RowRect(const State* st, int i)
    {
        RECT r = R(st, kPad + 6, kListY + 6 + i * kRowH, kW - 2 * kPad - 12, kRowH - 4);
        OffsetRect(&r, 0, -st->scroll);
        return r;
    }
    RECT RemoveRect(const State* st, int i)
    {
        RECT row = RowRect(st, i);
        const int s = S(st, 28);
        const int cy = (row.top + row.bottom) / 2;
        return RECT{ row.right - S(st, 8) - s, cy - s / 2, row.right - S(st, 8), cy + s / 2 };
    }
    RECT MatchBox(const State* st) { return R(st, kMatchX, kEditY, kMatchW, kEditH); }
    RECT NameBox(const State* st) { return R(st, kNameX, kEditY, kNameW, kEditH); }
    RECT AddRect(const State* st) { return R(st, kAddX, kEditY, kAddW, kEditH); }
    RECT ClearRect(const State* st) { return R(st, kClearX, kEditY, kClearW, kEditH); }
    RECT SaveRect(const State* st) { return R(st, kW - kPad - kCancelW - 10 - kSaveW, kBtnY, kSaveW, kBtnH); }
    RECT CancelRect(const State* st) { return R(st, kW - kPad - kCancelW, kBtnY, kCancelW, kBtnH); }

    int MaxScroll(const State* st)
    {
        const int content = S(st, 12 + static_cast<int>(st->rules.size()) * kRowH);
        return std::max(0, content - S(st, kListH));
    }

    bool In(const RECT& r, POINT p) { return PtInRect(&r, p) != FALSE; }

    int HitTest(const State* st, POINT p)
    {
        for (int i = 0; i < 2; ++i)
            if (In(CheckRowRect(st, i), p)) return i == 0 ? HIT_OVERRIDE : HIT_STARTUP;
        if (In(AddRect(st), p)) return HIT_ADD;
        if (In(ClearRect(st), p)) return HIT_CLEAR;
        if (In(SaveRect(st), p)) return HIT_SAVE;
        if (In(CancelRect(st), p)) return HIT_CANCEL;
        if (In(ListRect(st), p))
        {
            for (int i = 0; i < static_cast<int>(st->rules.size()); ++i)
            {
                if (In(RemoveRect(st, i), p)) return HIT_REMOVE + i;
                if (In(RowRect(st, i), p)) return HIT_ROW + i;
            }
        }
        return HIT_NONE;
    }

    // ---- Painting ----

    void RoundRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float rad)
    {
        const float d = rad * 2;
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
        path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
        path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
        path.CloseFigure();
    }

    Gdiplus::RectF F(const RECT& r)
    {
        return Gdiplus::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                              static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
    }

    void FillRound(Gdiplus::Graphics& g, const RECT& r, float rad, const Gdiplus::Color& fill,
                   const Gdiplus::Color* border = nullptr, float borderW = 1.0f)
    {
        Gdiplus::RectF rf = F(r);
        rf.Width -= 1; rf.Height -= 1;
        Gdiplus::GraphicsPath path;
        RoundRectPath(path, rf, rad);
        Gdiplus::SolidBrush b(fill);
        g.FillPath(&b, &path);
        if (border)
        {
            Gdiplus::Pen pen(*border, borderW);
            g.DrawPath(&pen, &path);
        }
    }

    void Text(Gdiplus::Graphics& g, const std::wstring& s, const Gdiplus::Font& font, const Gdiplus::Color& c,
              const Gdiplus::RectF& r, Gdiplus::StringAlignment h = Gdiplus::StringAlignmentNear,
              Gdiplus::StringAlignment v = Gdiplus::StringAlignmentCenter, bool wrap = false)
    {
        Gdiplus::StringFormat fmt;
        fmt.SetAlignment(h);
        fmt.SetLineAlignment(v);
        fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        if (!wrap) fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::SolidBrush b(c);
        g.DrawString(s.c_str(), static_cast<INT>(s.size()), &font, r, &fmt, &b);
    }

    float MeasureW(Gdiplus::Graphics& g, const std::wstring& s, const Gdiplus::Font& font)
    {
        Gdiplus::RectF out;
        Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
        fmt.SetFormatFlags(Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
        g.MeasureString(s.c_str(), static_cast<INT>(s.size()), &font, Gdiplus::PointF(0, 0), &fmt, &out);
        return out.Width;
    }

    void DrawCheckRow(Gdiplus::Graphics& g, const State* st, const RECT& row, bool checked, bool hover,
                      const wchar_t* label, const Gdiplus::Font& font)
    {
        const int box = S(st, 20);
        const int cy = (row.top + row.bottom) / 2;
        RECT b{ row.left, cy - box / 2, row.left + box, cy + box / 2 };
        if (checked)
        {
            FillRound(g, b, SF(st, 4), kCheck);
            Gdiplus::Pen tick(Gdiplus::Color(255, 255, 255, 255), SF(st, 2.2f));
            tick.SetLineJoin(Gdiplus::LineJoinRound);
            tick.SetStartCap(Gdiplus::LineCapRound);
            tick.SetEndCap(Gdiplus::LineCapRound);
            const float x = static_cast<float>(b.left), y = static_cast<float>(b.top), s = static_cast<float>(box);
            Gdiplus::PointF pts[3] = { { x + s * 0.24f, y + s * 0.52f }, { x + s * 0.43f, y + s * 0.70f }, { x + s * 0.76f, y + s * 0.32f } };
            g.DrawLines(&tick, pts, 3);
        }
        else
        {
            const Gdiplus::Color edge = hover ? kAccentText : kMuted;
            FillRound(g, b, SF(st, 4), kInput, &edge, SF(st, 1.5f));
        }
        Gdiplus::RectF tr(static_cast<float>(b.right + S(st, 12)), static_cast<float>(row.top),
                          static_cast<float>(row.right - b.right - S(st, 12)), static_cast<float>(row.bottom - row.top));
        Text(g, label, font, kText, tr);
    }

    void DrawButton(Gdiplus::Graphics& g, const State* st, const RECT& r, const std::wstring& label,
                    const Gdiplus::Font& font, const Gdiplus::Color& fill, const Gdiplus::Color& textC,
                    const Gdiplus::Color* border = nullptr)
    {
        FillRound(g, r, SF(st, 6), fill, border);
        Text(g, label, font, textC, F(r), Gdiplus::StringAlignmentCenter);
    }

    void Paint(State* st, HDC hdc)
    {
        RECT client;
        GetClientRect(st->hwnd, &client);
        const int cw = client.right, ch = client.bottom;
        if (cw <= 0 || ch <= 0) return;

        Gdiplus::Bitmap buffer(cw, ch, PixelFormat32bppPARGB);
        Gdiplus::Graphics g(&buffer);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        g.Clear(kBg);

        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::FontFamily semi(L"Segoe UI Semibold");
        Gdiplus::FontFamily& bold = semi.IsAvailable() ? semi : family;
        Gdiplus::Font title(&bold, SF(st, 19), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font section(&bold, SF(st, 13), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font body(&family, SF(st, 15), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font bodyBold(&bold, SF(st, 15), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font smallFont(&family, SF(st, 13), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font chipFont(&family, SF(st, 13.5f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

        // Header: dot + app name + "Settings".
        {
            const float dot = SF(st, 10);
            Gdiplus::SolidBrush db(kAccent);
            g.FillEllipse(&db, SF(st, kPad), SF(st, 31) - dot / 2, dot, dot);
            const float tx = SF(st, kPad + 20);
            Gdiplus::RectF tr(tx, SF(st, 16), SF(st, 300), SF(st, 30));
            Text(g, kAppName, title, kText, tr);
            const float tw = MeasureW(g, kAppName, title);
            Gdiplus::RectF sr(tx + tw + SF(st, 10), SF(st, 18), SF(st, 120), SF(st, 30));
            Text(g, L"Settings", body, kMuted, sr);
        }

        // General.
        Text(g, L"GENERAL", section, kMuted, Gdiplus::RectF(SF(st, kPad), SF(st, 62), SF(st, 300), SF(st, 22)));
        DrawCheckRow(g, st, CheckRowRect(st, 0), st->overrideTV, st->hover == HIT_OVERRIDE,
                     L"Replace system Task View (taskbar button, swipe)", body);
        DrawCheckRow(g, st, CheckRowRect(st, 1), st->startup, st->hover == HIT_STARTUP,
                     L"Start Grouped Task View when Windows starts", body);

        Gdiplus::Pen divider(kBorder, 1.0f);
        g.DrawLine(&divider, SF(st, kPad), SF(st, 176), SF(st, kW - kPad), SF(st, 176));

        // Grouping rules.
        Text(g, L"GROUPING RULES", section, kMuted, Gdiplus::RectF(SF(st, kPad), SF(st, 190), SF(st, 300), SF(st, 22)));
        Text(g, L"Add a rule to rename a group or merge apps together. Any app whose name or .exe "
                L"contains Match goes into Group name.",
             smallFont, kMuted, Gdiplus::RectF(SF(st, kPad), SF(st, 212), SF(st, kW - 2 * kPad), SF(st, 44)),
             Gdiplus::StringAlignmentNear, Gdiplus::StringAlignmentNear, true);

        const RECT list = ListRect(st);
        FillRound(g, list, SF(st, 10), kPanel, &kBorder);

        if (st->rules.empty())
        {
            Text(g, L"No rules yet. Add one below.", body, kMuted, F(list), Gdiplus::StringAlignmentCenter);
        }
        else
        {
            RECT inner = list;
            InflateRect(&inner, -S(st, 2), -S(st, 2));
            g.SetClip(F(inner));
            // One shared chip width so the arrows and group names line up in a column.
            float chipCol = SF(st, 60);
            for (const auto& r : st->rules)
                chipCol = std::max(chipCol, MeasureW(g, r.match, chipFont) + SF(st, 22));
            chipCol = std::min(chipCol, SF(st, 190));
            for (int i = 0; i < static_cast<int>(st->rules.size()); ++i)
            {
                const RECT row = RowRect(st, i);
                if (row.bottom < list.top || row.top > list.bottom) continue;
                const bool sel = (i == st->selected);
                const bool hov = (st->hover == HIT_ROW + i || st->hover == HIT_REMOVE + i);
                if (sel)
                {
                    FillRound(g, row, SF(st, 8), kRowSel);
                    RECT bar{ row.left, row.top + S(st, 8), row.left + S(st, 3), row.bottom - S(st, 8) };
                    FillRound(g, bar, SF(st, 1.5f), kAccent);
                }
                else if (hov)
                {
                    FillRound(g, row, SF(st, 8), kRowHover);
                }

                // Match chip.
                const std::wstring& m = st->rules[i].match;
                const float maxChip = SF(st, 190);
                const float chipW = std::min(maxChip, MeasureW(g, m, chipFont) + SF(st, 22));
                const float cy = (row.top + row.bottom) / 2.0f;
                Gdiplus::RectF chip(static_cast<float>(row.left + S(st, 12)), cy - SF(st, 13), chipW, SF(st, 26));
                {
                    Gdiplus::GraphicsPath p;
                    RoundRectPath(p, chip, SF(st, 6));
                    Gdiplus::SolidBrush cb(kChip);
                    g.FillPath(&cb, &p);
                }
                Text(g, m, chipFont, kText, chip, Gdiplus::StringAlignmentCenter);

                // Arrow + group name.
                const float ax = chip.X + chipCol + SF(st, 10);
                Text(g, L"→", body, kMuted, Gdiplus::RectF(ax, static_cast<float>(row.top), SF(st, 20),
                                                                   static_cast<float>(row.bottom - row.top)));
                const RECT rem = RemoveRect(st, i);
                const float nx = ax + SF(st, 26);
                Text(g, st->rules[i].name, bodyBold, kText,
                     Gdiplus::RectF(nx, static_cast<float>(row.top), rem.left - S(st, 10) - nx,
                                    static_cast<float>(row.bottom - row.top)));

                // Remove button.
                const bool remHot = (st->hover == HIT_REMOVE + i);
                FillRound(g, rem, SF(st, 6), remHot ? kDangerHover : kPanel, &kDangerBorder);
                Gdiplus::Pen x(kDanger, SF(st, 1.6f));
                x.SetStartCap(Gdiplus::LineCapRound);
                x.SetEndCap(Gdiplus::LineCapRound);
                const float c1 = (rem.left + rem.right) / 2.0f, c2 = (rem.top + rem.bottom) / 2.0f, k = SF(st, 5);
                g.DrawLine(&x, c1 - k, c2 - k, c1 + k, c2 + k);
                g.DrawLine(&x, c1 - k, c2 + k, c1 + k, c2 - k);
            }
            g.ResetClip();

            // Scroll thumb.
            const int maxs = MaxScroll(st);
            if (maxs > 0)
            {
                const float trackTop = static_cast<float>(list.top + S(st, 8));
                const float trackH = static_cast<float>(list.bottom - list.top - S(st, 16));
                const float content = trackH + maxs;
                const float thumbH = std::max(SF(st, 24), trackH * trackH / content);
                const float thumbY = trackTop + (trackH - thumbH) * st->scroll / maxs;
                Gdiplus::RectF thumb(static_cast<float>(list.right - S(st, 7)), thumbY, SF(st, 3), thumbH);
                Gdiplus::GraphicsPath p;
                RoundRectPath(p, thumb, SF(st, 1.5f));
                Gdiplus::SolidBrush tb(kMuted);
                g.FillPath(&tb, &p);
            }
        }

        // Editor.
        Text(g, L"Match", smallFont, kMuted, Gdiplus::RectF(SF(st, kMatchX), SF(st, kEditLabelY), SF(st, kMatchW), SF(st, 18)));
        Text(g, L"Group name", smallFont, kMuted, Gdiplus::RectF(SF(st, kNameX), SF(st, kEditLabelY), SF(st, kNameW), SF(st, 18)));
        const Gdiplus::Color* mb = (st->focusEdit == st->match) ? &kAccent : &kBorder;
        const Gdiplus::Color* nb = (st->focusEdit == st->name) ? &kAccent : &kBorder;
        FillRound(g, MatchBox(st), SF(st, 6), kInput, mb, st->focusEdit == st->match ? SF(st, 1.5f) : 1.0f);
        FillRound(g, NameBox(st), SF(st, 6), kInput, nb, st->focusEdit == st->name ? SF(st, 1.5f) : 1.0f);

        const bool editing = st->selected >= 0;
        DrawButton(g, st, AddRect(st), editing ? L"Update" : L"+ Add", body,
                   st->hover == HIT_ADD ? kAccentSoftHover : kAccentSoft, kText);
        DrawButton(g, st, ClearRect(st), L"Clear", body, st->hover == HIT_CLEAR ? kButtonHover : kButton, kText, &kBorder);

        g.DrawLine(&divider, SF(st, kPad), SF(st, kFooterY), SF(st, kW - kPad), SF(st, kFooterY));

        // Footer.
        const RECT save = SaveRect(st);
        const float tipW = static_cast<float>(save.left) - SF(st, kPad + 12);
        Text(g, L"Tip: right-click a group in Task View to rename it.", smallFont, kMuted,
             Gdiplus::RectF(SF(st, kPad), SF(st, kFooterY + 12), SF(st, kW - 2 * kPad), SF(st, 20)));
        Text(g, L"Your settings stay on this PC.", smallFont, kAccentText,
             Gdiplus::RectF(SF(st, kPad), static_cast<float>(save.top), tipW, static_cast<float>(save.bottom - save.top)));

        DrawButton(g, st, save, L"Save", bodyBold, st->hover == HIT_SAVE ? kAccentHover : kAccent,
                   Gdiplus::Color(255, 255, 255, 255));
        DrawButton(g, st, CancelRect(st), L"Cancel", body, st->hover == HIT_CANCEL ? kButtonHover : kButton, kText, &kBorder);

        Gdiplus::Graphics screen(hdc);
        screen.DrawImage(&buffer, 0, 0, cw, ch);
    }

    // ---- Behaviour ----

    void Invalidate(State* st) { InvalidateRect(st->hwnd, nullptr, FALSE); }

    void LayoutEdits(State* st)
    {
        if (st->editFont) DeleteObject(st->editFont);
        st->editFont = CreateFontW(-S(st, 15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        const int eh = S(st, 22);
        auto place = [&](HWND e, const RECT& box)
        {
            SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(st->editFont), TRUE);
            const int y = box.top + (box.bottom - box.top - eh) / 2;
            MoveWindow(e, box.left + S(st, 10), y, box.right - box.left - S(st, 20), eh, TRUE);
        };
        place(st->match, MatchBox(st));
        place(st->name, NameBox(st));
    }

    void Select(State* st, int i)
    {
        st->selected = i;
        if (i >= 0 && i < static_cast<int>(st->rules.size()))
        {
            SetWindowTextW(st->match, st->rules[i].match.c_str());
            SetWindowTextW(st->name, st->rules[i].name.c_str());
        }
        Invalidate(st);
    }

    void ClearEditor(State* st)
    {
        st->selected = -1;
        SetWindowTextW(st->match, L"");
        SetWindowTextW(st->name, L"");
        SetFocus(st->match);
        Invalidate(st);
    }

    void ScrollToRow(State* st, int i)
    {
        const int top = S(st, 6 + i * kRowH), bottom = top + S(st, kRowH);
        if (top < st->scroll) st->scroll = top - S(st, 6);
        else if (bottom > st->scroll + S(st, kListH)) st->scroll = bottom - S(st, kListH) + S(st, 6);
        st->scroll = std::clamp(st->scroll, 0, MaxScroll(st));
    }

    void AddOrUpdate(State* st)
    {
        const std::wstring m = Trim(GetText(st->match)), n = Trim(GetText(st->name));
        if (m.empty() || n.empty())
        {
            SetFocus(m.empty() ? st->match : st->name);
            MessageBeep(MB_OK);
            return;
        }
        if (st->selected >= 0 && st->selected < static_cast<int>(st->rules.size()))
        {
            st->rules[st->selected] = GroupRule{ m, n };
        }
        else
        {
            st->rules.push_back(GroupRule{ m, n });
            ScrollToRow(st, static_cast<int>(st->rules.size()) - 1);
        }
        ClearEditor(st);
    }

    void ApplyPrefill(State* st, const std::wstring& match, const std::wstring& name)
    {
        if (match.empty() && name.empty()) return;
        for (int i = 0; i < static_cast<int>(st->rules.size()); ++i)
        {
            if (_wcsicmp(st->rules[i].match.c_str(), match.c_str()) == 0)
            {
                Select(st, i);
                ScrollToRow(st, i);
                SetFocus(st->name);
                SendMessageW(st->name, EM_SETSEL, 0, -1);
                return;
            }
        }
        st->selected = -1;
        SetWindowTextW(st->match, match.c_str());
        SetWindowTextW(st->name, name.c_str());
        SetFocus(st->name);
        SendMessageW(st->name, EM_SETSEL, 0, -1);
        Invalidate(st);
    }

    // Enter adds/updates, Tab hops between the two boxes, Esc closes.
    LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR ref)
    {
        auto* st = reinterpret_cast<State*>(ref);
        if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS | DefSubclassProc(hwnd, msg, wParam, lParam);
        if (msg == WM_KEYDOWN)
        {
            if (wParam == VK_RETURN) { AddOrUpdate(st); return 0; }
            if (wParam == VK_ESCAPE) { DestroyWindow(st->hwnd); return 0; }
            if (wParam == VK_TAB) { SetFocus(hwnd == st->match ? st->name : st->match); return 0; }
        }
        if (msg == WM_CHAR && (wParam == L'\r' || wParam == 27 || wParam == L'\t')) return 0;
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    void ApplyFrame(HWND hwnd)
    {
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        const COLORREF caption = RGB(0xA9, 0x4F, 0xC0), text = RGB(255, 255, 255);
        DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
        DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
        DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &caption, sizeof(caption));
    }

    void SizeForDpi(HWND hwnd, UINT dpi, const RECT* suggested)
    {
        RECT r{ 0, 0, MulDiv(kW, dpi, 96), MulDiv(kH, dpi, 96) };
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
        const int w = r.right - r.left, h = r.bottom - r.top;
        if (suggested)
        {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        else
        {
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(mon, &mi);
            const RECT& wa = mi.rcWork;
            SetWindowPos(hwnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2,
                         w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    std::wstring g_prefillMatch, g_prefillName;

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg)
        {
        case WM_CREATE:
        {
            st = new State();
            st->hwnd = hwnd;
            st->dpi = GetDpiForWindow(hwnd);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            HINSTANCE hi = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

            st->inputBrush = CreateSolidBrush(kInputRef);
            const DWORD es = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
            st->match = CreateWindowExW(0, L"EDIT", L"", es, 0, 0, 10, 10, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MATCH)), hi, nullptr);
            st->name = CreateWindowExW(0, L"EDIT", L"", es, 0, 0, 10, 10, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_NAME)), hi, nullptr);
            SetWindowSubclass(st->match, EditProc, 1, reinterpret_cast<DWORD_PTR>(st));
            SetWindowSubclass(st->name, EditProc, 1, reinterpret_cast<DWORD_PTR>(st));
            SendMessageW(st->match, EM_SETLIMITTEXT, 200, 0);
            SendMessageW(st->name, EM_SETLIMITTEXT, 200, 0);
            LayoutEdits(st);

            ApplyFrame(hwnd);
            st->rules = Settings::LoadRules();
            st->overrideTV = Settings::OverrideSystemTaskView();
            st->startup = StartupEnabled();
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (st) Paint(st, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLOREDIT:
        {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, kTextRef);
            SetBkColor(dc, kInputRef);
            return reinterpret_cast<LRESULT>(st->inputBrush);
        }

        case WM_DPICHANGED:
        {
            st->dpi = HIWORD(wParam);
            SizeForDpi(hwnd, st->dpi, reinterpret_cast<RECT*>(lParam));
            LayoutEdits(st);
            st->scroll = std::clamp(st->scroll, 0, MaxScroll(st));
            Invalidate(st);
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            const int h = HitTest(st, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            if (h != st->hover) { st->hover = h; Invalidate(st); }
            return 0;
        }

        case WM_MOUSELEAVE:
            if (st->hover != HIT_NONE) { st->hover = HIT_NONE; Invalidate(st); }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && st && st->hover != HIT_NONE)
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);

        case WM_MOUSEWHEEL:
        {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            st->scroll = std::clamp(st->scroll - MulDiv(delta, S(st, kRowH), WHEEL_DELTA), 0, MaxScroll(st));
            POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &p);
            st->hover = HitTest(st, p);
            Invalidate(st);
            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            const int h = HitTest(st, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
            if (h == HIT_OVERRIDE) { st->overrideTV = !st->overrideTV; Invalidate(st); }
            else if (h == HIT_STARTUP) { st->startup = !st->startup; Invalidate(st); }
            else if (h == HIT_ADD) AddOrUpdate(st);
            else if (h == HIT_CLEAR) ClearEditor(st);
            else if (h == HIT_SAVE)
            {
                Settings::SaveRules(st->rules);
                Settings::SetOverrideSystemTaskView(st->overrideTV);
                SetStartupEnabled(st->startup);
                DestroyWindow(hwnd);
            }
            else if (h == HIT_CANCEL) DestroyWindow(hwnd);
            else if (h >= HIT_REMOVE)
            {
                const int i = h - HIT_REMOVE;
                if (i < static_cast<int>(st->rules.size()))
                {
                    st->rules.erase(st->rules.begin() + i);
                    if (st->selected == i) ClearEditor(st);
                    else if (st->selected > i) --st->selected;
                    st->scroll = std::clamp(st->scroll, 0, MaxScroll(st));
                    POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    st->hover = HitTest(st, p);
                    Invalidate(st);
                }
            }
            else if (h >= HIT_ROW)
            {
                const int i = h - HIT_ROW;
                if (i == st->selected) ClearEditor(st);
                else
                {
                    Select(st, i);
                    SetFocus(st->name);
                    const int len = GetWindowTextLengthW(st->name);
                    SendMessageW(st->name, EM_SETSEL, len, len);
                }
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
            return DefWindowProcW(hwnd, msg, wParam, lParam);

        case WM_COMMAND:
        {
            const int code = HIWORD(wParam);
            const HWND ctl = reinterpret_cast<HWND>(lParam);
            if (code == EN_SETFOCUS) { st->focusEdit = ctl; Invalidate(st); }
            else if (code == EN_KILLFOCUS) { if (st->focusEdit == ctl) st->focusEdit = nullptr; Invalidate(st); }
            return 0;
        }

        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE && st && !GetFocus()) SetFocus(st->match);
            return 0;

        case WM_DESTROY:
            if (st)
            {
                RemoveWindowSubclass(st->match, EditProc, 1);
                RemoveWindowSubclass(st->name, EditProc, 1);
                if (st->editFont) DeleteObject(st->editFont);
                if (st->inputBrush) DeleteObject(st->inputBrush);
                delete st;
            }
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
    const std::wstring match = prefillMatch ? prefillMatch : L"";
    const std::wstring name = prefillName ? prefillName : L"";

    if (g_wnd && IsWindow(g_wnd))
    {
        if (auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(g_wnd, GWLP_USERDATA)))
            ApplyPrefill(st, match, name);
        ShowWindow(g_wnd, SW_RESTORE);
        SetForegroundWindow(g_wnd);
        return;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClass;
        wc.hIcon = LoadIconW(hinstance, MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassExW(&wc);
        registered = true;
    }

    // The rest of the app is DPI-unaware (the overlay relies on it), so only this
    // window is made per-monitor aware; that keeps its text crisp instead of
    // stretched and blurry.
    const DPI_AWARENESS_CONTEXT prev = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_wnd = CreateWindowExW(0, kClass, L"Grouped Task View: Settings",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, hinstance, nullptr);
    if (g_wnd)
    {
        SizeForDpi(g_wnd, GetDpiForWindow(g_wnd), nullptr);
        if (auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(g_wnd, GWLP_USERDATA)))
        {
            ShowWindow(g_wnd, SW_SHOW);
            SetForegroundWindow(g_wnd);
            SetFocus(st->match);
            ApplyPrefill(st, match, name);
        }
    }
    if (prev) SetThreadDpiAwarenessContext(prev);
}
