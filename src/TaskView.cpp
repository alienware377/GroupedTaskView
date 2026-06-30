#include "pch.h"
#include "TaskView.h"
#include "WindowEnumerator.h"
#include "DesktopMover.h"
#include "SettingsWindow.h"

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
void TaskView::InvalidateBackdropCache()
{
    if (m_backdropClient)
    {
        DeleteObject(m_backdropClient);
        m_backdropClient = nullptr;
    }
    m_backdropW = 0;
    m_backdropH = 0;
    delete m_bgBmpCache;
    m_bgBmpCache = nullptr;
}

void TaskView::CaptureBlurredBackground()
{
    InvalidateBackdropCache();
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
    // Note: an empty current desktop still opens (showing the desktop strip), so
    // Win+Tab works even on a desktop with no windows.

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
    InvalidateBackdropCache();
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

    // Show only windows on the CURRENT virtual desktop, like the native Task
    // View. A window on another desktop is shell-cloaked (DWM_CLOAKED_SHELL), as
    // is suspended-UWP / hidden system UI; an app-cloaked but on-screen window
    // (e.g. a background browser window) is not, so it stays.
    auto windows = WindowEnumerator::Enumerate();
    {
        std::vector<WindowInfo> kept;
        kept.reserve(windows.size());
        for (auto& win : windows)
        {
            int cloaked = 0;
            DwmGetWindowAttribute(win.hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if ((cloaked & DWM_CLOAKED_SHELL) == 0)
            {
                kept.push_back(std::move(win));
            }
        }
        windows = std::move(kept);
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
    // Cap how large a thumbnail can get (like the native Task View), so a desktop
    // with only one or two windows shows them at a sensible size, not full-screen.
    const double maxBodyH = ah * 0.36;

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
                rowH.push_back(std::min(bh * s, maxBodyH) + titleBar);
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
            rowH.push_back(std::min(bh * s, maxBodyH) + titleBar);
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

        // While dragging, the dragged window's thumbnail floats with the cursor.
        if (m_dragging && m_dragHwnd && t.hwnd == m_dragHwnd)
        {
            dest = DragFloatRect();
            visible = true;
        }

        // DWM stretches the source to fill rcDestination, which squishes windows
        // whose aspect doesn't exactly match the tile. Fit the real source size
        // (aspect-preserving, centered) so thumbnails are never distorted.
        if (visible)
        {
            SIZE src{};
            const int bw = dest.right - dest.left, bh = dest.bottom - dest.top;
            if (bw > 0 && bh > 0 && SUCCEEDED(DwmQueryThumbnailSourceSize(t.thumb, &src)) && src.cx > 0 && src.cy > 0)
            {
                const double srcAspect = static_cast<double>(src.cx) / src.cy;
                const double boxAspect = static_cast<double>(bw) / bh;
                int fw = bw, fh = bh;
                if (srcAspect > boxAspect)
                {
                    fh = static_cast<int>(bw / srcAspect + 0.5);
                }
                else
                {
                    fw = static_cast<int>(bh * srcAspect + 0.5);
                }
                const int cx = dest.left + (bw - fw) / 2;
                const int cy = dest.top + (bh - fh) / 2;
                dest = { cx, cy, cx + fw, cy + fh };
            }
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
    // exactly regardless of any DPI scaling (fall back to flat dark). The costly
    // HALFTONE stretch of the full-res wallpaper is done once into a client-sized
    // cache; every paint after that is a plain (fast) blit, so dragging stays
    // smooth instead of re-resampling 8 megapixels per mouse move.
    if (m_background)
    {
        if (!m_backdropClient || m_backdropW != w || m_backdropH != h)
        {
            InvalidateBackdropCache();
            const int bw = m_monitor.right - m_monitor.left;
            const int bh = m_monitor.bottom - m_monitor.top;
            m_backdropClient = CreateCompatibleBitmap(screen, w, h);
            HDC cdc = CreateCompatibleDC(screen);
            HBITMAP oc = static_cast<HBITMAP>(SelectObject(cdc, m_backdropClient));
            HDC bgdc = CreateCompatibleDC(screen);
            HBITMAP oldbg = static_cast<HBITMAP>(SelectObject(bgdc, m_background));
            SetStretchBltMode(cdc, HALFTONE);
            StretchBlt(cdc, 0, 0, w, h, bgdc, 0, 0, bw, bh, SRCCOPY);
            SelectObject(bgdc, oldbg);
            DeleteDC(bgdc);
            SelectObject(cdc, oc);
            DeleteDC(cdc);
            m_backdropW = w;
            m_backdropH = h;
        }
        HDC cdc = CreateCompatibleDC(screen);
        HBITMAP oc = static_cast<HBITMAP>(SelectObject(cdc, m_backdropClient));
        BitBlt(dc, 0, 0, w, h, cdc, 0, 0, SRCCOPY);
        SelectObject(cdc, oc);
        DeleteDC(cdc);
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
        // Cached once (FromHBITMAP copies the whole bitmap, far too costly to do
        // on every paint while dragging). Lives until the backdrop changes.
        if (m_background && !m_bgBmpCache)
        {
            m_bgBmpCache = Bitmap::FromHBITMAP(m_background, nullptr);
        }
        Bitmap* bgBmp = m_bgBmpCache;
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
                g.DrawImage(bgBmp, RectF(static_cast<REAL>(x), static_cast<REAL>(y), static_cast<REAL>(tw), static_cast<REAL>(TitleBar)),
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

            // The dragged tile lifts off: leave its space empty (faint placeholder).
            if (m_dragging && !m_dragIsSub && i == m_dragIndex)
            {
                SolidBrush hole(Color(60, 0, 0, 0));
                FillRound(g, hole, front.left, front.top, fw, fh, 9);
                Pen dash(Color(90, 150, 150, 160), 1.5f);
                dash.SetDashStyle(DashStyleDash);
                StrokeRound(g, dash, front.left, front.top, fw, fh, 9);
                continue;
            }

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

                // The dragged sub-tile lifts off: leave its space empty.
                if (m_dragging && m_dragIsSub && i == m_dragIndex)
                {
                    SolidBrush hole(Color(70, 0, 0, 0));
                    FillRound(g, hole, r.left, r.top, tw, th, 9);
                    Pen dash(Color(90, 150, 150, 160), 1.5f);
                    dash.SetDashStyle(DashStyleDash);
                    StrokeRound(g, dash, r.left, r.top, tw, th, 9);
                    continue;
                }

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
            // While dragging, the hovered desktop is a drop target.
            if (m_dragging && i == m_hotDesktop && !d.isNew)
            {
                SolidBrush dropFill(Color(70, 0, 120, 215));
                g.FillRectangle(&dropFill, r.left, r.top, DeskW, DeskH);
                Pen dropPen(Color(255, 0, 120, 215), 3.5f);
                StrokeRound(g, dropPen, r.left, r.top, DeskW, DeskH, 9);
            }
            else if (d.isCurrent)
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

void TaskView::MoveWindowsToDesktop(const std::vector<HWND>& windows, int desktopIndex)
{
    if (desktopIndex < 0 || desktopIndex >= static_cast<int>(m_desktops.size()) || m_desktops[desktopIndex].isNew)
    {
        return;
    }
    // The public IVirtualDesktopManager::MoveWindowToDesktop is blocked for other
    // processes' windows (E_ACCESSDENIED), so use the shell's internal interfaces.
    MoveWindowsToVirtualDesktop(windows, m_desktops[desktopIndex].id);
    RefreshAfterMove();
}

// Rebuild the model after a move so the desktop-strip previews and grouping
// reflect the new state, keeping the overlay open.
void TaskView::RefreshAfterMove()
{
    UnregisterThumbnails();
    m_expandedGroup = -1;
    m_subRects.clear();
    BuildModel();
    if (m_selCell >= static_cast<int>(m_cells.size()))
    {
        m_selCell = static_cast<int>(m_cells.size()) - 1; // -1 when empty; nav guards it
    }
    m_hotDesktop = m_hotCell = -1;
    LayoutGroups();
    RegisterThumbnails();
    BuildDesktopThumbnails();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void TaskView::BeginDrag()
{
    m_dragAnim = 0.0;
    m_dragHwnd = nullptr;
    if (m_dragIsSub)
    {
        if (m_expandedGroup >= 0 && m_dragIndex >= 0 && m_dragIndex < static_cast<int>(m_groups[m_expandedGroup].windows.size()))
        {
            m_dragHwnd = m_groups[m_expandedGroup].windows[m_dragIndex].hwnd;
            if (m_dragIndex < static_cast<int>(m_subRects.size())) m_dragOrigBody = CellBody(m_subRects[m_dragIndex]);
        }
    }
    else if (m_dragIndex >= 0 && m_dragIndex < static_cast<int>(m_cells.size()))
    {
        const int g = m_cells[m_dragIndex].group;
        m_dragHwnd = m_groups[g].windows.front().hwnd;
        const RECT cell = m_cells[m_dragIndex].rect;
        const int depth = std::min(static_cast<int>(m_groups[g].windows.size()) - 1, MaxStack);
        const RECT front{ cell.left, cell.top + depth * StackOff, cell.right - depth * StackOff, cell.bottom };
        m_dragOrigBody = CellBody(front);
    }
    // Bring the dragged thumbnail to the top by re-registering it last.
    for (auto& t : m_thumbs)
    {
        if (t.hwnd == m_dragHwnd && t.thumb)
        {
            DwmUnregisterThumbnail(t.thumb);
            if (FAILED(DwmRegisterThumbnail(m_hwnd, t.hwnd, &t.thumb))) t.thumb = nullptr;
            break;
        }
    }
    SetTimer(m_hwnd, 1, 12, nullptr); // lift animation tick
    UpdateThumbnails();
}

void TaskView::EndDrag()
{
    KillTimer(m_hwnd, 1);
    m_dragHwnd = nullptr;
    m_dragAnim = 0.0;
}

RECT TaskView::DragFloatRect() const
{
    const int ow = m_dragOrigBody.right - m_dragOrigBody.left;
    const int oh = m_dragOrigBody.bottom - m_dragOrigBody.top;
    const double sc = 0.55; // floating thumbnail is a shrunk copy
    const int fw = std::max(40, static_cast<int>(ow * sc));
    const int fh = std::max(30, static_cast<int>(oh * sc));
    const RECT f{ m_dragPos.x - fw / 2, m_dragPos.y - fh / 2, m_dragPos.x + fw / 2, m_dragPos.y + fh / 2 };
    const double a = m_dragAnim;
    RECT d;
    d.left = m_dragOrigBody.left + static_cast<LONG>((f.left - m_dragOrigBody.left) * a);
    d.top = m_dragOrigBody.top + static_cast<LONG>((f.top - m_dragOrigBody.top) * a);
    d.right = m_dragOrigBody.right + static_cast<LONG>((f.right - m_dragOrigBody.right) * a);
    d.bottom = m_dragOrigBody.bottom + static_cast<LONG>((f.bottom - m_dragOrigBody.bottom) * a);
    return d;
}

LRESULT TaskView::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TIMER:
        if (wParam == 1 && m_dragging)
        {
            m_dragAnim += 0.18;
            if (m_dragAnim >= 1.0) { m_dragAnim = 1.0; KillTimer(hwnd, 1); }
            UpdateThumbnails();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

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

    case WM_LBUTTONDOWN:
    {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        m_mouseDown = false;
        m_dragging = false;
        m_dragIndex = -1;
        if (m_expandedGroup >= 0)
        {
            const int sub = SubTileAtPoint(pt);
            if (sub >= 0 && SubCloseAtPoint(pt) < 0) // don't drag from the close button
            {
                m_mouseDown = true; m_dragIsSub = true; m_dragIndex = sub;
                m_dragStart = pt; m_dragPos = pt; SetCapture(hwnd);
            }
        }
        else
        {
            const int cell = GroupCellAtPoint(pt);
            if (cell >= 0)
            {
                m_mouseDown = true; m_dragIsSub = false; m_dragIndex = cell;
                m_dragStart = pt; m_dragPos = pt; SetCapture(hwnd);
            }
        }
        return 0;
    }

    case WM_SETCURSOR:
        if (m_dragging)
        {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_CAPTURECHANGED:
        if (m_dragging) { EndDrag(); UpdateThumbnails(); InvalidateRect(hwnd, nullptr, FALSE); }
        m_mouseDown = false;
        m_dragging = false;
        m_dragIndex = -1;
        return 0;

    case WM_MOUSEMOVE:
    {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int desk = DesktopAtPoint(pt);

        // Drag in progress: track the cursor and the desktop drop target.
        if (m_mouseDown)
        {
            if (!m_dragging)
            {
                const int dx = pt.x - m_dragStart.x, dy = pt.y - m_dragStart.y;
                if (dx * dx + dy * dy > 100) // ~10px threshold
                {
                    m_dragging = true;
                    m_dragPos = pt;
                    BeginDrag();
                }
            }
            if (m_dragging)
            {
                m_dragPos = pt;
                m_hotDesktop = desk;
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                UpdateThumbnails();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }

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
        const bool wasDragging = m_dragging;
        const bool dragIsSub = m_dragIsSub;
        const int dragIndex = m_dragIndex;
        if (m_mouseDown) ReleaseCapture();
        m_mouseDown = false;
        m_dragging = false;
        m_dragIndex = -1;
        if (wasDragging) EndDrag();

        const int desk = DesktopAtPoint(pt);

        // A completed drag = drop the window(s) onto the target desktop.
        if (wasDragging)
        {
            if (desk >= 0 && !m_desktops[desk].isNew)
            {
                std::vector<HWND> toMove;
                if (dragIsSub && m_expandedGroup >= 0)
                {
                    if (dragIndex >= 0 && dragIndex < static_cast<int>(m_groups[m_expandedGroup].windows.size()))
                        toMove.push_back(m_groups[m_expandedGroup].windows[dragIndex].hwnd);
                }
                else if (!dragIsSub && dragIndex >= 0 && dragIndex < static_cast<int>(m_cells.size()))
                {
                    for (const auto& w : m_groups[m_cells[dragIndex].group].windows)
                        toMove.push_back(w.hwnd);
                }
                if (!toMove.empty())
                {
                    MoveWindowsToDesktop(toMove, desk);
                    return 0;
                }
            }
            // Cancelled drop: restore the floated thumbnail to its tile.
            m_hotDesktop = -1;
            UpdateThumbnails();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        // Otherwise it's a plain click.
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

    case WM_RBUTTONUP:
    {
        if (m_expandedGroup < 0)
        {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const int cell = GroupCellAtPoint(pt);
            if (cell >= 0)
            {
                const AppGroup& grp = m_groups[m_cells[cell].group];
                const std::wstring name = grp.name;
                const std::wstring match = grp.matchHint.empty() ? grp.name : grp.matchHint;
                const std::wstring renameLabel = L"Rename “" + name + L"”…";
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, renameLabel.c_str());
                AppendMenuW(menu, MF_STRING, 2, L"Settings…");
                POINT scr = pt;
                ClientToScreen(hwnd, &scr);
                SetForegroundWindow(hwnd);
                const int sel = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, scr.x, scr.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                if (sel == 1)
                {
                    Close();
                    ShowSettingsWindow(m_hinstance, match.c_str(), name.c_str());
                }
                else if (sel == 2)
                {
                    Close();
                    ShowSettingsWindow(m_hinstance);
                }
            }
        }
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
