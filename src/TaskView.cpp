#include "pch.h"
#include "TaskView.h"
#include "WindowEnumerator.h"

#include <windowsx.h>
#include <climits>
#include <algorithm>
#include <gdiplus.h>
#include <objbase.h>
#include <shobjidl.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif
#ifndef DWM_CLOAKED_SHELL
#define DWM_CLOAKED_SHELL 0x0000002
#endif

using namespace Gdiplus;

namespace
{
    const wchar_t TaskViewClassName[] = L"GroupedTaskView_Overlay";

    constexpr int Margin = 48;
    constexpr int GridTop = 40;
    constexpr int TitleBar = 30;
    constexpr int StripH = 172;
    constexpr int StackOff = 8; // per-card stack offset
    constexpr int MaxStack = 2; // max peeking back cards
    constexpr int CloseSize = 22;
    constexpr int DeskW = 214;
    constexpr int DeskH = 120;
    constexpr int DeskGap = 20;
    constexpr int GroupIcon = 26; // larger app icon per group

    void AddRound(GraphicsPath& path, int x, int y, int w, int h, int r)
    {
        if (w < 2 * r) r = w / 2;
        if (h < 2 * r) r = h / 2;
        path.Reset();
        path.AddArc(x, y, r, r, 180, 90);
        path.AddArc(x + w - r, y, r, r, 270, 90);
        path.AddArc(x + w - r, y + h - r, r, r, 0, 90);
        path.AddArc(x, y + h - r, r, r, 90, 90);
        path.CloseFigure();
    }
    void FillRound(Graphics& g, Brush& b, int x, int y, int w, int h, int r)
    {
        GraphicsPath p;
        AddRound(p, x, y, w, h, r);
        g.FillPath(&b, &p);
    }
    void StrokeRound(Graphics& g, Pen& pen, int x, int y, int w, int h, int r)
    {
        GraphicsPath p;
        AddRound(p, x, y, w, h, r);
        g.DrawPath(&pen, &p);
    }

    // Rounded top corners, square bottom (for tile title bars).
    void FillRoundTop(Graphics& g, Brush& b, int x, int y, int w, int h, int r)
    {
        if (w < 2 * r) r = w / 2;
        if (h < r) r = h;
        GraphicsPath p;
        p.AddArc(x, y, r, r, 180, 90);
        p.AddArc(x + w - r, y, r, r, 270, 90);
        p.AddLine(static_cast<REAL>(x + w), static_cast<REAL>(y + h), static_cast<REAL>(x), static_cast<REAL>(y + h));
        p.CloseFigure();
        g.FillPath(&b, &p);
    }

    // Square top, rounded bottom corners (for tile bodies under the title bar).
    void FillRoundBottom(Graphics& g, Brush& b, int x, int y, int w, int h, int r)
    {
        if (w < 2 * r) r = w / 2;
        if (h < r) r = h;
        GraphicsPath p;
        p.AddLine(static_cast<REAL>(x), static_cast<REAL>(y), static_cast<REAL>(x + w), static_cast<REAL>(y));
        p.AddArc(x + w - r, y + h - r, r, r, 0, 90);
        p.AddArc(x, y + h - r, r, r, 90, 90);
        p.CloseFigure();
        g.FillPath(&b, &p);
    }

    int NearestInDirection(const std::vector<RECT>& rects, int cur, int dx, int dy)
    {
        if (cur < 0 || cur >= static_cast<int>(rects.size()))
        {
            return cur;
        }
        const RECT c = rects[cur];
        const int ccx = (c.left + c.right) / 2, ccy = (c.top + c.bottom) / 2;
        int best = cur, bestScore = INT_MAX;
        for (int i = 0; i < static_cast<int>(rects.size()); ++i)
        {
            if (i == cur) continue;
            const RECT r = rects[i];
            const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            if (dx > 0 && cx <= ccx) continue;
            if (dx < 0 && cx >= ccx) continue;
            if (dy > 0 && cy <= ccy) continue;
            if (dy < 0 && cy >= ccy) continue;
            const int primary = dx != 0 ? abs(cx - ccx) : abs(cy - ccy);
            const int secondary = dx != 0 ? abs(cy - ccy) : abs(cx - ccx);
            const int score = primary + secondary * 3;
            if (score < bestScore) { bestScore = score; best = i; }
        }
        return best;
    }
}

bool TaskView::Initialize(HINSTANCE hinstance)
{
    m_hinstance = hinstance;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = hinstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = TaskViewClassName;
    RegisterClassExW(&wc);

    CreateOverlayWindow();
    return m_hwnd != nullptr;
}

// A top-level window belongs to whichever virtual desktop it was created on, and
// can't be shown on another. Recreating it on each open guarantees it appears on
// the desktop the user is currently viewing (so Win+Tab works on every desktop).
void TaskView::CreateOverlayWindow()
{
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED, TaskViewClassName, L"", WS_POPUP,
                             0, 0, 100, 100, nullptr, nullptr, m_hinstance, this);
    if (m_hwnd)
    {
        SetLayeredWindowAttributes(m_hwnd, 0, 252, LWA_ALPHA);
    }
}

// Build a blurred + acrylic-flattened copy of the desktop wallpaper to use as an
// acrylic backdrop behind the grid (and behind the tile title bars). Uses a true
// GDI+ Gaussian blur (smooth, never blocky) with a radius that scales with the
// screen size, so the blur looks the same on 1080p and 4K.
void TaskView::CaptureBlurredBackground()
{
    if (m_background)
    {
        DeleteObject(m_background);
        m_background = nullptr;
    }
    const int mw = m_monitor.right - m_monitor.left;
    const int mh = m_monitor.bottom - m_monitor.top;
    if (mw <= 0 || mh <= 0)
    {
        return;
    }

    // Render the wallpaper into a 32-bit bitmap. We blur at a reduced resolution
    // for speed; because a Gaussian blur is smooth, upscaling it stays smooth.
    const int down = 2;
    const int ww = std::max(1, mw / down);
    const int wh = std::max(1, mh / down);
    Bitmap work(ww, wh, PixelFormat32bppPARGB);
    {
        Graphics g(&work);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        SolidBrush base(Color(255, 20, 20, 24));
        g.FillRectangle(&base, 0, 0, ww, wh);
        wchar_t wpPath[MAX_PATH] = {};
        if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wpPath, 0) && wpPath[0])
        {
            auto* wp = Image::FromFile(wpPath);
            if (wp && wp->GetLastStatus() == Ok)
            {
                const double iw = wp->GetWidth(), ih = wp->GetHeight();
                if (iw > 0 && ih > 0)
                {
                    const double scale = std::max(ww / iw, wh / ih);
                    const int dw = static_cast<int>(iw * scale), dh = static_cast<int>(ih * scale);
                    g.DrawImage(wp, (ww - dw) / 2, (wh - dh) / 2, dw, dh);
                }
            }
            delete wp;
        }
    }

    // Fluid blur radius: ~4% of screen width, expressed at the work resolution.
    REAL radius = static_cast<REAL>(mw) * 0.04f / down;
    radius = std::max<REAL>(2.0f, std::min<REAL>(254.0f, radius));
    Blur blur;
    BlurParams bp{ radius, FALSE };
    blur.SetParameters(&bp);
    work.ApplyEffect(&blur, nullptr);

    // Acrylic luminosity flatten (reduce contrast: lift darks, crush highlights)
    // while upscaling back to full size.
    Bitmap finalBmp(mw, mh, PixelFormat32bppPARGB);
    {
        Graphics g(&finalBmp);
        g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
        // Combined: boost saturation (sat), then flatten contrast (scale cs, lift
        // off). The saturation block is the luminance-preserving mix, scaled by cs.
        const REAL cs = 0.55f, off = 0.14f, sat = 1.5f;
        const REAL lr = 0.2126f, lg = 0.7152f, lb = 0.0722f;
        const REAL ar = (1.0f - sat) * lr, ag = (1.0f - sat) * lg, ab = (1.0f - sat) * lb;
        ColorMatrix cm = {
            (ar + sat) * cs, ar * cs, ar * cs, 0, 0,
            ag * cs, (ag + sat) * cs, ag * cs, 0, 0,
            ab * cs, ab * cs, (ab + sat) * cs, 0, 0,
            0, 0, 0, 1, 0,
            off, off, off, 0, 1
        };
        ImageAttributes ia;
        ia.SetColorMatrix(&cm);
        g.DrawImage(&work, RectF(0, 0, static_cast<REAL>(mw), static_cast<REAL>(mh)),
                    0, 0, static_cast<REAL>(ww), static_cast<REAL>(wh), UnitPixel, &ia);
    }

    finalBmp.GetHBITMAP(Color(255, 0, 0, 0), &m_background);
}

LRESULT CALLBACK TaskView::WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* self = (msg == WM_NCCREATE)
                     ? reinterpret_cast<TaskView*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams)
                     : reinterpret_cast<TaskView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->WndProc(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

void TaskView::Toggle()
{
    m_visible ? Close() : Open();
}

void TaskView::Open()
{
    // Recreate the overlay so it lives on the desktop the user is viewing now.
    CreateOverlayWindow();
    if (!m_hwnd)
    {
        return;
    }

    HMONITOR mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    m_monitor = mi.rcMonitor;

    // Grab a blurred snapshot of the desktop while our window is still hidden.
    CaptureBlurredBackground();

    BuildModel();
    if (m_cells.empty())
    {
        m_groups.clear();
        return;
    }

    m_expandedGroup = -1;
    m_selCell = 0;
    m_selSub = 0;
    m_hotCell = m_hotSub = m_hotClose = m_hotDesktop = -1;

    const int mw = m_monitor.right - m_monitor.left;
    const int mh = m_monitor.bottom - m_monitor.top;
    m_gridBottom = mh - StripH - 8;

    SetWindowPos(m_hwnd, HWND_TOPMOST, m_monitor.left, m_monitor.top, mw, mh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    LayoutGroups();
    ShowWindow(m_hwnd, SW_SHOW);

    DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD me = GetCurrentThreadId();
    AttachThreadInput(me, fg, TRUE);
    SetForegroundWindow(m_hwnd);
    SetFocus(m_hwnd);
    AttachThreadInput(me, fg, FALSE);

    RegisterThumbnails();
    BuildDesktopThumbnails();
    m_visible = true;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TaskView::Close()
{
    if (!m_visible) return;
    m_visible = false;
    UnregisterThumbnails();
    ShowWindow(m_hwnd, SW_HIDE);
    if (m_background)
    {
        DeleteObject(m_background);
        m_background = nullptr;
    }
    m_groups.clear();
    m_cells.clear();
    m_thumbs.clear();
    m_desktops.clear();
    m_subRects.clear();
    m_expandedGroup = -1;
}

void TaskView::BuildModel()
{
    // Desktops first, so we can filter windows by desktop membership.
    int current = 0;
    auto desks = VirtualDesktops::Enumerate(current);
    m_currentDesktop = current;

    // Keep windows that live on a real virtual desktop (any of them) or on the
    // current desktop / pinned. This includes other-desktop windows while
    // dropping suspended-UWP and hidden system UI that map to no real desktop.
    IVirtualDesktopManager* vdm = nullptr;
    CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&vdm));

    const GUID curId = (current >= 0 && current < static_cast<int>(desks.size())) ? desks[current].id : GUID{};

    auto windows = WindowEnumerator::Enumerate();
    {
        std::vector<WindowInfo> kept;
        kept.reserve(windows.size());
        for (auto& win : windows)
        {
            int cloaked = 0;
            DwmGetWindowAttribute(win.hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            const bool shellCloaked = (cloaked & DWM_CLOAKED_SHELL) != 0;

            bool keep;
            if (!shellCloaked)
            {
                // Visible on the current desktop (or merely app-cloaked) -> real window.
                keep = true;
            }
            else
            {
                // Shell-cloaked: keep only if it's a real window parked on ANOTHER
                // desktop; drop suspended-UWP / hidden system UI (which map to the
                // current desktop id or to no desktop at all).
                keep = false;
                GUID gid{};
                if (vdm && SUCCEEDED(vdm->GetWindowDesktopId(win.hwnd, &gid)) && !IsEqualGUID(gid, curId))
                {
                    for (const auto& d : desks)
                    {
                        if (IsEqualGUID(d.id, gid))
                        {
                            keep = true;
                            break;
                        }
                    }
                }
            }
            if (keep)
            {
                kept.push_back(std::move(win));
            }
        }
        windows = std::move(kept);
    }
    if (vdm)
    {
        vdm->Release();
    }

    m_groups = AppGrouping::Group(std::move(windows));

    m_cells.clear();
    m_thumbs.clear();
    for (int gi = 0; gi < static_cast<int>(m_groups.size()); ++gi)
    {
        m_cells.push_back(Cell{ gi, {} });
        for (int wi = 0; wi < static_cast<int>(m_groups[gi].windows.size()); ++wi)
        {
            m_thumbs.push_back(Thumb{ m_groups[gi].windows[wi].hwnd, gi, wi, nullptr });
        }
    }

    // Desktops + wallpaper previews.
    m_desktops.clear();
    for (int i = 0; i < static_cast<int>(desks.size()); ++i)
    {
        DesktopTile t{};
        t.id = desks[i].id;
        t.name = desks[i].name;
        t.isCurrent = desks[i].isCurrent;
        t.index = i;
        if (!desks[i].wallpaperPath.empty())
        {
            auto* img = Image::FromFile(desks[i].wallpaperPath.c_str());
            if (img && img->GetLastStatus() == Ok)
            {
                t.wallpaper = std::shared_ptr<Image>(img);
            }
            else
            {
                delete img;
            }
        }
        m_desktops.push_back(std::move(t));
    }
    DesktopTile newTile{};
    newTile.name = L"New desktop";
    newTile.isNew = true;
    newTile.index = static_cast<int>(desks.size());
    m_desktops.push_back(std::move(newTile));
}

std::vector<RECT> TaskView::FluidGrid(int count, RECT area, int titleBar) const
{
    std::vector<RECT> rects;
    if (count <= 0) return rects;
    const int aw = area.right - area.left;
    const int ah = area.bottom - area.top;
    const int gap = 18;
    const double aspect = 0.56; // body height relative to width

    int bestCols = 1;
    double bestArea = -1.0;
    for (int cols = 1; cols <= count; ++cols)
    {
        const int rows = (count + cols - 1) / cols;
        double cellW = static_cast<double>(aw - (cols + 1) * gap) / cols;
        if (cellW < 70) continue;
        double cellH = cellW * aspect + titleBar;
        const double needed = rows * cellH + (rows + 1) * gap;
        double scale = 1.0;
        if (needed > ah)
        {
            scale = static_cast<double>(ah - (rows + 1) * gap) / (rows * cellH);
            if (scale <= 0.05) continue;
        }
        const double a = (cellW * scale) * (cellH * scale);
        if (a > bestArea) { bestArea = a; bestCols = cols; }
    }

    const int cols = bestCols;
    const int rows = (count + cols - 1) / cols;
    double cellW = static_cast<double>(aw - (cols + 1) * gap) / cols;
    double cellH = cellW * aspect + titleBar;
    const double needed = rows * cellH + (rows + 1) * gap;
    const double scale = needed > ah ? static_cast<double>(ah - (rows + 1) * gap) / (rows * cellH) : 1.0;
    cellW *= scale;
    cellH *= scale;
    const int cw = static_cast<int>(cellW);
    const int ch = static_cast<int>(cellH);

    const int totalH = rows * ch + (rows + 1) * gap;
    const int startY = area.top + (ah - totalH) / 2 + gap;
    for (int r = 0; r < rows; ++r)
    {
        const int itemsThisRow = std::min(cols, count - r * cols);
        const int rowW = itemsThisRow * cw + (itemsThisRow - 1) * gap;
        const int startX = area.left + (aw - rowW) / 2;
        for (int c = 0; c < itemsThisRow; ++c)
        {
            const int x = startX + c * (cw + gap);
            const int y = startY + r * (ch + gap);
            rects.push_back(RECT{ x, y, x + cw, y + ch });
        }
    }
    return rects;
}

double TaskView::AspectOf(HWND hwnd)
{
    RECT wr{};
    int ww = 0, wh = 0;
    if (IsIconic(hwnd))
    {
        WINDOWPLACEMENT wp{ sizeof(wp) };
        if (GetWindowPlacement(hwnd, &wp))
        {
            ww = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
            wh = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        }
    }
    else if (GetWindowRect(hwnd, &wr))
    {
        ww = wr.right - wr.left;
        wh = wr.bottom - wr.top;
    }
    double a = (ww > 0 && wh > 0) ? static_cast<double>(ww) / wh : 1.6;
    return std::max(0.55, std::min(2.6, a)); // clamp so extreme windows stay sane
}

// Justified row layout: pick the largest uniform row height (body) such that the
// whole set fits in `area`, packing tiles whose width follows their aspect.
std::vector<RECT> TaskView::JustifiedLayout(const std::vector<double>& aspects, RECT area, int titleBar) const
{
    const int n = static_cast<int>(aspects.size());
    std::vector<RECT> rects(n);
    if (n == 0)
    {
        return rects;
    }
    const int aw = area.right - area.left;
    const int ah = area.bottom - area.top;
    const int gap = 18;

    // For a target body height bh, build rows (each scaled to fill width) and
    // return the total stacked height.
    auto buildRows = [&](double bh, std::vector<std::vector<int>>& rows, std::vector<double>& rowH) {
        rows.clear();
        rowH.clear();
        std::vector<int> cur;
        for (int i = 0; i < n; ++i)
        {
            double sum = 0;
            for (int idx : cur) sum += bh * aspects[idx];
            const double w = bh * aspects[i];
            const double withNew = cur.empty() ? w : sum + gap + w;
            if (!cur.empty() && withNew > aw)
            {
                const double s = (aw - static_cast<int>(cur.size() - 1) * gap) / sum;
                rows.push_back(cur);
                rowH.push_back(bh * s + titleBar);
                cur.clear();
            }
            cur.push_back(i);
        }
        if (!cur.empty())
        {
            double sum = 0;
            for (int idx : cur) sum += bh * aspects[idx];
            double s = (aw - static_cast<int>(cur.size() - 1) * gap) / sum;
            if (s > 1.0) s = 1.0; // don't blow up a short final row
            rows.push_back(cur);
            rowH.push_back(bh * s + titleBar);
        }
        double total = gap * (static_cast<int>(rows.size()) - 1);
        for (double r : rowH) total += r;
        return total;
    };

    double lo = 24, hi = ah;
    for (int it = 0; it < 40; ++it)
    {
        const double mid = (lo + hi) / 2;
        std::vector<std::vector<int>> rows;
        std::vector<double> rowH;
        (buildRows(mid, rows, rowH) <= ah) ? lo = mid : hi = mid;
    }

    std::vector<std::vector<int>> rows;
    std::vector<double> rowH;
    const double totalH = buildRows(lo, rows, rowH);
    int y = area.top + std::max(0, static_cast<int>((ah - totalH) / 2));
    for (size_t r = 0; r < rows.size(); ++r)
    {
        const double bodyH = rowH[r] - titleBar;
        double sumW = 0;
        for (int idx : rows[r]) sumW += bodyH * aspects[idx];
        const double rowWidth = sumW + gap * (static_cast<int>(rows[r].size()) - 1);
        int x = area.left + std::max(0, static_cast<int>((aw - rowWidth) / 2));
        for (int idx : rows[r])
        {
            const int wt = static_cast<int>(bodyH * aspects[idx]);
            rects[idx] = RECT{ x, y, x + wt, y + static_cast<int>(rowH[r]) };
            x += wt + gap;
        }
        y += static_cast<int>(rowH[r]) + gap;
    }
    return rects;
}

void TaskView::LayoutGroups()
{
    const int mw = m_monitor.right - m_monitor.left;
    RECT area{ Margin, GridTop, mw - Margin, m_gridBottom };
    std::vector<double> aspects;
    aspects.reserve(m_cells.size());
    for (const auto& c : m_cells)
    {
        aspects.push_back(AspectOf(m_groups[c.group].windows.front().hwnd));
    }
    auto rects = JustifiedLayout(aspects, area, TitleBar);
    for (size_t i = 0; i < m_cells.size() && i < rects.size(); ++i)
    {
        m_cells[i].rect = rects[i];
    }

    // Desktop strip.
    const int stripTop = (m_monitor.bottom - m_monitor.top) - StripH;
    const int total = static_cast<int>(m_desktops.size());
    const int stripWidth = total * DeskW + (total - 1) * DeskGap;
    int dx = std::max(Margin, (mw - stripWidth) / 2);
    const int dy = stripTop + (StripH - (DeskH + 26)) / 2;
    for (auto& d : m_desktops)
    {
        d.rect = { dx, dy, dx + DeskW, dy + DeskH };
        dx += DeskW + DeskGap;
    }
}

void TaskView::LayoutExpanded()
{
    const int mw = m_monitor.right - m_monitor.left;
    RECT grid{ Margin, GridTop, mw - Margin, m_gridBottom };
    const int insetX = (grid.right - grid.left) / 10;
    const int insetY = (grid.bottom - grid.top) / 12;
    m_expandArea = { grid.left + insetX, grid.top + insetY, grid.right - insetX, grid.bottom - insetY };
    std::vector<double> aspects;
    if (m_expandedGroup >= 0)
    {
        for (const auto& win : m_groups[m_expandedGroup].windows)
        {
            aspects.push_back(AspectOf(win.hwnd));
        }
    }
    m_subRects = JustifiedLayout(aspects, m_expandArea, TitleBar);
}

RECT TaskView::CellBody(const RECT& front) const
{
    return { front.left + 4, front.top + TitleBar, front.right - 4, front.bottom - 4 };
}

void TaskView::RegisterThumbnails()
{
    for (auto& t : m_thumbs)
    {
        if (FAILED(DwmRegisterThumbnail(m_hwnd, t.hwnd, &t.thumb)))
        {
            t.thumb = nullptr;
        }
    }
    UpdateThumbnails();
}

void TaskView::UpdateThumbnails()
{
    for (auto& t : m_thumbs)
    {
        if (!t.thumb) continue;
        RECT dest{};
        bool visible = false;
        if (m_expandedGroup < 0)
        {
            // Group view: only each group's front (most-recent) window shows.
            if (t.indexInGroup == 0 && t.group < static_cast<int>(m_cells.size()))
            {
                const RECT cell = m_cells[t.group].rect;
                const int depth = std::min(static_cast<int>(m_groups[t.group].windows.size()) - 1, MaxStack);
                const RECT front{ cell.left, cell.top + depth * StackOff, cell.right - depth * StackOff, cell.bottom };
                dest = CellBody(front);
                visible = true;
            }
        }
        else if (t.group == m_expandedGroup && t.indexInGroup < static_cast<int>(m_subRects.size()))
        {
            dest = CellBody(m_subRects[t.indexInGroup]);
            visible = true;
        }

        DWM_THUMBNAIL_PROPERTIES props{};
        props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        props.fSourceClientAreaOnly = FALSE;
        props.opacity = 255;
        props.fVisible = visible ? TRUE : FALSE;
        props.rcDestination = dest;
        DwmUpdateThumbnailProperties(t.thumb, &props);
    }
}

void TaskView::UnregisterThumbnails()
{
    for (auto& t : m_thumbs)
    {
        if (t.thumb)
        {
            DwmUnregisterThumbnail(t.thumb);
            t.thumb = nullptr;
        }
    }
    for (HTHUMBNAIL h : m_deskThumbs)
    {
        DwmUnregisterThumbnail(h);
    }
    m_deskThumbs.clear();
}

// Composite a live mini-preview of each desktop's open windows into its strip
// tile, on top of the wallpaper, mirroring how the native Task View renders
// desktop previews. Each window gets its own DWM thumbnail scaled to the tile.
void TaskView::BuildDesktopThumbnails()
{
    const int mw = m_monitor.right - m_monitor.left;
    const int mh = m_monitor.bottom - m_monitor.top;
    if (mw <= 0 || mh <= 0)
    {
        return;
    }
    const double scaleX = static_cast<double>(DeskW) / mw;
    const double scaleY = static_cast<double>(DeskH) / mh;

    IVirtualDesktopManager* vdm = nullptr;
    CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&vdm));

    // Bottom-to-top z-order so the topmost window registers last and draws on top.
    auto windows = WindowEnumerator::Enumerate();
    std::reverse(windows.begin(), windows.end());

    for (const auto& win : windows)
    {
        if (IsIconic(win.hwnd))
        {
            continue;
        }

        // Find the desktop tile this window belongs to.
        int deskIndex = -1;
        if (vdm)
        {
            GUID gid{};
            if (SUCCEEDED(vdm->GetWindowDesktopId(win.hwnd, &gid)))
            {
                for (int d = 0; d < static_cast<int>(m_desktops.size()); ++d)
                {
                    if (!m_desktops[d].isNew && IsEqualGUID(m_desktops[d].id, gid))
                    {
                        deskIndex = d;
                        break;
                    }
                }
            }
        }
        if (deskIndex < 0)
        {
            // Fall back to the current desktop when the id can't be resolved.
            deskIndex = m_currentDesktop;
        }
        if (deskIndex < 0 || deskIndex >= static_cast<int>(m_desktops.size()))
        {
            continue;
        }

        RECT wr{};
        if (!GetWindowRect(win.hwnd, &wr))
        {
            continue;
        }
        const RECT tile = m_desktops[deskIndex].rect;
        RECT dest{};
        dest.left = tile.left + static_cast<int>((wr.left - m_monitor.left) * scaleX);
        dest.top = tile.top + static_cast<int>((wr.top - m_monitor.top) * scaleY);
        dest.right = tile.left + static_cast<int>((wr.right - m_monitor.left) * scaleX);
        dest.bottom = tile.top + static_cast<int>((wr.bottom - m_monitor.top) * scaleY);

        // Clamp to the tile so previews never bleed into neighbours.
        dest.left = std::max(dest.left, tile.left);
        dest.top = std::max(dest.top, tile.top);
        dest.right = std::min(dest.right, tile.right);
        dest.bottom = std::min(dest.bottom, tile.bottom);
        if (dest.right - dest.left < 4 || dest.bottom - dest.top < 4)
        {
            continue;
        }

        HTHUMBNAIL th = nullptr;
        if (SUCCEEDED(DwmRegisterThumbnail(m_hwnd, win.hwnd, &th)) && th)
        {
            DWM_THUMBNAIL_PROPERTIES props{};
            props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
            props.fSourceClientAreaOnly = FALSE;
            props.opacity = 255;
            props.fVisible = TRUE;
            props.rcDestination = dest;
            DwmUpdateThumbnailProperties(th, &props);
            m_deskThumbs.push_back(th);
        }
    }

    if (vdm)
    {
        vdm->Release();
    }
}

void TaskView::Render()
{
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    HDC screen = GetDC(m_hwnd);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HBITMAP old = static_cast<HBITMAP>(SelectObject(dc, bmp));

    // Blurred wallpaper as the backdrop, stretched to the client rect so it fills
    // exactly regardless of any DPI scaling (fall back to flat dark).
    if (m_background)
    {
        const int bw = m_monitor.right - m_monitor.left;
        const int bh = m_monitor.bottom - m_monitor.top;
        HDC bgdc = CreateCompatibleDC(screen);
        HBITMAP oldbg = static_cast<HBITMAP>(SelectObject(bgdc, m_background));
        SetStretchBltMode(dc, HALFTONE);
        StretchBlt(dc, 0, 0, w, h, bgdc, 0, 0, bw, bh, SRCCOPY);
        SelectObject(bgdc, oldbg);
        DeleteDC(bgdc);
    }
    else
    {
        HBRUSH fill = CreateSolidBrush(RGB(22, 22, 26));
        RECT full{ 0, 0, w, h };
        FillRect(dc, &full, fill);
        DeleteObject(fill);
    }

    {
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // Title font: match the native Task View (Segoe UI Variable Text, regular),
        // falling back to Segoe UI if the variable family isn't present.
        FontFamily fam(L"Segoe UI");
        FontFamily famVar(L"Segoe UI Variable Text");
        const FontFamily* titleFam = (famVar.GetLastStatus() == Ok) ? &famVar : &fam;
        Font nameFont(titleFam, 14, FontStyleRegular, UnitPixel);
        Font titleFont(titleFam, 13, FontStyleRegular, UnitPixel);
        Font deskFont(titleFam, 13, FontStyleRegular, UnitPixel);
        SolidBrush white(Color(255, 245, 245, 245));
        SolidBrush dim(Color(255, 170, 170, 178));
        SolidBrush cardBack(Color(255, 46, 46, 54));
        SolidBrush cardBack2(Color(255, 38, 38, 45));
        SolidBrush bodyBg(Color(255, 30, 30, 36));
        SolidBrush badge(Color(255, 0, 120, 215));
        SolidBrush closeHot(Color(255, 200, 60, 60));
        Pen accent(Color(255, 0, 120, 215), 2.5f);
        Pen hotPen(Color(255, 150, 150, 162), 2.0f);
        StringFormat sf;
        sf.SetLineAlignment(StringAlignmentCenter);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        sf.SetFormatFlags(StringFormatFlagsNoWrap);
        StringFormat center;
        center.SetAlignment(StringAlignmentCenter);
        center.SetLineAlignment(StringAlignmentCenter);
        center.SetTrimming(StringTrimmingEllipsisCharacter);
        center.SetFormatFlags(StringFormatFlagsNoWrap);

        // Title bars are a darkened slice of the same blurred wallpaper, like the
        // native Task View. Sample it from the backdrop bitmap (scaled to client).
        std::unique_ptr<Bitmap> bgBmp;
        if (m_background)
        {
            bgBmp.reset(Bitmap::FromHBITMAP(m_background, nullptr));
        }
        const int bgW = m_monitor.right - m_monitor.left;
        const int bgH = m_monitor.bottom - m_monitor.top;
        auto acrylicTitle = [&](int x, int y, int tw) {
            int r = 9;
            if (tw < 2 * r) r = tw / 2;
            GraphicsPath clip;
            clip.AddArc(x, y, r, r, 180, 90);
            clip.AddArc(x + tw - r, y, r, r, 270, 90);
            clip.AddLine(static_cast<REAL>(x + tw), static_cast<REAL>(y + TitleBar), static_cast<REAL>(x), static_cast<REAL>(y + TitleBar));
            clip.CloseFigure();
            g.SetClip(&clip);
            if (bgBmp && w > 0 && h > 0)
            {
                const REAL sx = static_cast<REAL>(bgW) / w, sy = static_cast<REAL>(bgH) / h;
                g.DrawImage(bgBmp.get(), RectF(static_cast<REAL>(x), static_cast<REAL>(y), static_cast<REAL>(tw), static_cast<REAL>(TitleBar)),
                            x * sx, y * sy, tw * sx, TitleBar * sy, UnitPixel);
            }
            SolidBrush tint(Color(140, 14, 14, 20));
            g.FillRectangle(&tint, x, y, tw, TitleBar);
            g.ResetClip();
        };

        // ---- Group stacks ----
        for (int i = 0; i < static_cast<int>(m_cells.size()); ++i)
        {
            const AppGroup& grp = m_groups[m_cells[i].group];
            const RECT cell = m_cells[i].rect;
            const int count = static_cast<int>(grp.windows.size());
            const int depth = std::min(count - 1, MaxStack);
            const RECT front{ cell.left, cell.top + depth * StackOff, cell.right - depth * StackOff, cell.bottom };
            const int fw = front.right - front.left;
            const int fh = front.bottom - front.top;

            // Back cards (peek up-right) convey the stack.
            for (int k = depth; k >= 1; --k)
            {
                const int bx = front.left + k * StackOff;
                const int by = front.top - k * StackOff;
                FillRound(g, (k == 1 ? cardBack : cardBack2), bx, by, fw, fh, 9);
            }

            // Body placeholder (DWM draws the thumbnail over it); acrylic title bar.
            FillRoundBottom(g, bodyBg, front.left, front.top + TitleBar, fw, fh - TitleBar, 9);
            acrylicTitle(front.left, front.top, fw);

            if (grp.icon)
            {
                DrawIconEx(dc, front.left + 8, front.top + (TitleBar - GroupIcon) / 2, grp.icon, GroupIcon, GroupIcon, 0, nullptr, DI_NORMAL);
            }
            RectF nameRect(static_cast<REAL>(front.left + 8 + GroupIcon + 8), static_cast<REAL>(front.top),
                           static_cast<REAL>(fw - (8 + GroupIcon + 8) - 36), static_cast<REAL>(TitleBar));
            g.DrawString(grp.name.c_str(), -1, &nameFont, nameRect, &sf, &white);

            if (count > 1)
            {
                const int bw = 24, bh = 18;
                const int bxx = front.right - bw - 6, byy = front.top + (TitleBar - bh) / 2;
                FillRound(g, badge, bxx, byy, bw, bh, 8);
                RectF br(static_cast<REAL>(bxx), static_cast<REAL>(byy), static_cast<REAL>(bw), static_cast<REAL>(bh));
                wchar_t cnt[8];
                swprintf_s(cnt, L"%d", count);
                g.DrawString(cnt, -1, &titleFont, br, &center, &white);
            }

            if (i == m_selCell || i == m_hotCell)
            {
                StrokeRound(g, accent, front.left, front.top, fw, fh, 9);
            }
        }

        // ---- Expanded group overlay ----
        if (m_expandedGroup >= 0)
        {
            SolidBrush scrim(Color(180, 12, 12, 14));
            g.FillRectangle(&scrim, 0, 0, w, m_gridBottom + 4);

            for (int i = 0; i < static_cast<int>(m_subRects.size()); ++i)
            {
                const RECT r = m_subRects[i];
                const int tw = r.right - r.left, th = r.bottom - r.top;
                const WindowInfo& win = m_groups[m_expandedGroup].windows[i];

                FillRoundBottom(g, bodyBg, r.left, r.top + TitleBar, tw, th - TitleBar, 9);
                acrylicTitle(r.left, r.top, tw);
                if (win.icon)
                {
                    DrawIconEx(dc, r.left + 8, r.top + (TitleBar - 18) / 2, win.icon, 18, 18, 0, nullptr, DI_NORMAL);
                }
                RectF tr(static_cast<REAL>(r.left + 32), static_cast<REAL>(r.top), static_cast<REAL>(tw - 32 - CloseSize - 6), static_cast<REAL>(TitleBar));
                g.DrawString(win.title.c_str(), -1, &titleFont, tr, &sf, &white);

                const int cx = r.right - CloseSize - 4, cy = r.top + (TitleBar - CloseSize) / 2;
                if (i == m_hotClose)
                {
                    FillRound(g, closeHot, cx, cy, CloseSize, CloseSize, 5);
                }
                Pen xp(Color(255, 225, 225, 225), 1.6f);
                g.DrawLine(&xp, cx + 7, cy + 7, cx + CloseSize - 7, cy + CloseSize - 7);
                g.DrawLine(&xp, cx + CloseSize - 7, cy + 7, cx + 7, cy + CloseSize - 7);

                if (i == m_selSub || i == m_hotSub)
                {
                    StrokeRound(g, accent, r.left, r.top, tw, th, 9);
                }
            }
        }

        // ---- Desktop strip ----
        for (int i = 0; i < static_cast<int>(m_desktops.size()); ++i)
        {
            const DesktopTile& d = m_desktops[i];
            const RECT r = d.rect;
            GraphicsPath clip;
            AddRound(clip, r.left, r.top, DeskW, DeskH, 9);
            g.SetClip(&clip);
            if (d.wallpaper)
            {
                // Cover-fit (crop to fill) so the wallpaper keeps its aspect ratio.
                const double iw = d.wallpaper->GetWidth(), ih = d.wallpaper->GetHeight();
                if (iw > 0 && ih > 0)
                {
                    const double s = std::max(DeskW / iw, DeskH / ih);
                    const int dw = static_cast<int>(iw * s), dh = static_cast<int>(ih * s);
                    g.DrawImage(d.wallpaper.get(), r.left + (DeskW - dw) / 2, r.top + (DeskH - dh) / 2, dw, dh);
                }
                SolidBrush shade(Color(70, 0, 0, 0));
                g.FillRectangle(&shade, r.left, r.top, DeskW, DeskH);
            }
            else
            {
                SolidBrush deskBg(Color(255, 40, 40, 48));
                g.FillRectangle(&deskBg, r.left, r.top, DeskW, DeskH);
            }
            g.ResetClip();

            if (d.isNew)
            {
                Pen plus(Color(255, 210, 210, 215), 2.0f);
                g.DrawLine(&plus, r.left + DeskW / 2 - 12, r.top + DeskH / 2, r.left + DeskW / 2 + 12, r.top + DeskH / 2);
                g.DrawLine(&plus, r.left + DeskW / 2, r.top + DeskH / 2 - 12, r.left + DeskW / 2, r.top + DeskH / 2 + 12);
            }
            if (d.isCurrent)
            {
                StrokeRound(g, accent, r.left, r.top, DeskW, DeskH, 9);
            }
            else if (i == m_hotDesktop)
            {
                StrokeRound(g, hotPen, r.left, r.top, DeskW, DeskH, 9);
            }
            RectF lbl(static_cast<REAL>(r.left), static_cast<REAL>(r.bottom + 3), static_cast<REAL>(DeskW), 22.0f);
            g.DrawString(d.name.c_str(), -1, &deskFont, lbl, &center, d.isCurrent ? &white : &dim);
        }
    }

    BitBlt(screen, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(m_hwnd, screen);
}

int TaskView::GroupCellAtPoint(POINT pt) const
{
    for (int i = 0; i < static_cast<int>(m_cells.size()); ++i)
    {
        const AppGroup& grp = m_groups[m_cells[i].group];
        const int depth = std::min(static_cast<int>(grp.windows.size()) - 1, MaxStack);
        const RECT cell = m_cells[i].rect;
        const RECT front{ cell.left, cell.top + depth * StackOff, cell.right - depth * StackOff, cell.bottom };
        if (pt.x >= front.left && pt.x < front.right && pt.y >= front.top && pt.y < front.bottom)
        {
            return i;
        }
    }
    return -1;
}

int TaskView::SubTileAtPoint(POINT pt) const
{
    for (int i = 0; i < static_cast<int>(m_subRects.size()); ++i)
    {
        const RECT r = m_subRects[i];
        if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom)
        {
            return i;
        }
    }
    return -1;
}

int TaskView::SubCloseAtPoint(POINT pt) const
{
    const int i = SubTileAtPoint(pt);
    if (i < 0) return -1;
    const RECT r = m_subRects[i];
    const int cx = r.right - CloseSize - 4, cy = r.top + (TitleBar - CloseSize) / 2;
    if (pt.x >= cx && pt.x < cx + CloseSize && pt.y >= cy && pt.y < cy + CloseSize)
    {
        return i;
    }
    return -1;
}

int TaskView::DesktopAtPoint(POINT pt) const
{
    for (int i = 0; i < static_cast<int>(m_desktops.size()); ++i)
    {
        const RECT& r = m_desktops[i].rect;
        if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom + 24)
        {
            return i;
        }
    }
    return -1;
}

void TaskView::MoveSelection(int dx, int dy, bool expandedSpace)
{
    if (expandedSpace)
    {
        if (m_subRects.empty()) return;
        m_selSub = NearestInDirection(m_subRects, m_selSub, dx, dy);
    }
    else
    {
        if (m_cells.empty()) return;
        std::vector<RECT> rects;
        rects.reserve(m_cells.size());
        for (const auto& c : m_cells) rects.push_back(c.rect);
        m_selCell = NearestInDirection(rects, m_selCell, dx, dy);
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TaskView::ActivateGroupOrExpand(int cell)
{
    if (cell < 0 || cell >= static_cast<int>(m_cells.size())) return;
    const int group = m_cells[cell].group;
    if (static_cast<int>(m_groups[group].windows.size()) <= 1)
    {
        CommitWindow(m_groups[group].windows.front().hwnd);
    }
    else
    {
        Expand(group);
    }
}

void TaskView::Expand(int group)
{
    m_expandedGroup = group;
    m_selSub = 0;
    m_hotSub = m_hotClose = -1;
    LayoutExpanded();
    UpdateThumbnails();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TaskView::Collapse()
{
    m_expandedGroup = -1;
    m_subRects.clear();
    UpdateThumbnails();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TaskView::CommitWindow(HWND hwnd)
{
    Close();
    if (hwnd && IsWindow(hwnd))
    {
        ActivateWindow(hwnd);
    }
}

void TaskView::CloseWindowAt(int subIndex)
{
    if (m_expandedGroup < 0 || subIndex < 0 || subIndex >= static_cast<int>(m_groups[m_expandedGroup].windows.size()))
    {
        return;
    }
    HWND hwnd = m_groups[m_expandedGroup].windows[subIndex].hwnd;
    PostMessageW(hwnd, WM_CLOSE, 0, 0);

    // Drop the window from the model and the matching thumbnail.
    for (auto it = m_thumbs.begin(); it != m_thumbs.end(); ++it)
    {
        if (it->hwnd == hwnd)
        {
            if (it->thumb) DwmUnregisterThumbnail(it->thumb);
            m_thumbs.erase(it);
            break;
        }
    }
    auto& windows = m_groups[m_expandedGroup].windows;
    if (subIndex < static_cast<int>(windows.size()))
    {
        windows.erase(windows.begin() + subIndex);
    }

    if (windows.empty())
    {
        Close();
        return;
    }
    // Re-index thumbs for this group and rebuild.
    for (auto& t : m_thumbs)
    {
        if (t.group == m_expandedGroup && t.indexInGroup > subIndex)
        {
            t.indexInGroup--;
        }
    }
    if (static_cast<int>(windows.size()) == 1)
    {
        Collapse();
        return;
    }
    if (m_selSub >= static_cast<int>(windows.size())) m_selSub = static_cast<int>(windows.size()) - 1;
    LayoutExpanded();
    UpdateThumbnails();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

LRESULT TaskView::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Render();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_KEYDOWN:
    {
        const bool expanded = m_expandedGroup >= 0;
        switch (wParam)
        {
        case VK_ESCAPE:
            expanded ? Collapse() : Close();
            return 0;
        case VK_RETURN:
            if (expanded) CommitWindow(m_groups[m_expandedGroup].windows[m_selSub].hwnd);
            else ActivateGroupOrExpand(m_selCell);
            return 0;
        case VK_LEFT: MoveSelection(-1, 0, expanded); return 0;
        case VK_RIGHT: MoveSelection(1, 0, expanded); return 0;
        case VK_UP: MoveSelection(0, -1, expanded); return 0;
        case VK_DOWN: MoveSelection(0, 1, expanded); return 0;
        case VK_TAB: MoveSelection((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1, 0, expanded); return 0;
        default: return 0;
        }
    }

    case WM_MOUSEMOVE:
    {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int desk = DesktopAtPoint(pt);
        if (m_expandedGroup >= 0)
        {
            const int sub = SubTileAtPoint(pt);
            const int close = SubCloseAtPoint(pt);
            if (sub != m_hotSub || close != m_hotClose || desk != m_hotDesktop)
            {
                m_hotSub = sub; m_hotClose = close; m_hotDesktop = desk;
                if (sub >= 0) m_selSub = sub;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        else
        {
            const int cell = GroupCellAtPoint(pt);
            if (cell != m_hotCell || desk != m_hotDesktop)
            {
                m_hotCell = cell; m_hotDesktop = desk;
                if (cell >= 0) m_selCell = cell;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int desk = DesktopAtPoint(pt);
        if (desk >= 0)
        {
            const bool isNew = m_desktops[desk].isNew;
            const int target = m_desktops[desk].index;
            const int current = m_currentDesktop;
            Close();
            isNew ? VirtualDesktops::CreateNew() : VirtualDesktops::SwitchTo(target, current);
            return 0;
        }
        if (m_expandedGroup >= 0)
        {
            const int close = SubCloseAtPoint(pt);
            if (close >= 0) { CloseWindowAt(close); return 0; }
            const int sub = SubTileAtPoint(pt);
            if (sub >= 0) { CommitWindow(m_groups[m_expandedGroup].windows[sub].hwnd); return 0; }
            Collapse(); // click on the dimmed area
            return 0;
        }
        const int cell = GroupCellAtPoint(pt);
        if (cell >= 0) { ActivateGroupOrExpand(cell); }
        return 0;
    }

    case WM_KILLFOCUS:
        Close();
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void TaskView::ActivateWindow(HWND hwnd)
{
    if (IsIconic(hwnd))
    {
        ShowWindow(hwnd, SW_RESTORE);
    }
    HWND foreground = GetForegroundWindow();
    DWORD fgThread = GetWindowThreadProcessId(foreground, nullptr);
    DWORD me = GetCurrentThreadId();
    DWORD target = GetWindowThreadProcessId(hwnd, nullptr);
    if (fgThread != me) AttachThreadInput(me, fgThread, TRUE);
    if (target != me && target != fgThread) AttachThreadInput(me, target, TRUE);
    AllowSetForegroundWindow(ASFW_ANY);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if (target != me && target != fgThread) AttachThreadInput(me, target, FALSE);
    if (fgThread != me) AttachThreadInput(me, fgThread, FALSE);
}
