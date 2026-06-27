#pragma once

#include "AppGrouping.h"
#include "VirtualDesktops.h"

#include <Windows.h>
#include <dwmapi.h>

#include <memory>
#include <string>
#include <vector>

namespace Gdiplus
{
    class Image;
}

// A full-screen, Task-View-style overlay (Win+Tab) that lays every window out as
// a fluid grid of per-app *stacks*: one tile per application, sized so the whole
// set fits on a single screen with no scrolling. Clicking a multi-window stack
// expands it into its individual windows. A virtual-desktop strip with live
// wallpaper previews runs along the bottom.
class TaskView
{
public:
    bool Initialize(HINSTANCE hinstance);
    void Toggle();
    void Close();
    bool Visible() const { return m_visible; }

private:
    // One tile in the top-level grid = one application group.
    struct Cell
    {
        int group = 0;
        RECT rect{}; // whole tile
    };
    // A registered live thumbnail for one window.
    struct Thumb
    {
        HWND hwnd = nullptr;
        int group = 0;
        int indexInGroup = 0;
        HTHUMBNAIL thumb = nullptr;
    };
    struct DesktopTile
    {
        GUID id{};
        std::wstring name;
        bool isCurrent = false;
        bool isNew = false;
        int index = 0;
        RECT rect{};
        std::shared_ptr<Gdiplus::Image> wallpaper;
    };

    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void CreateOverlayWindow(); // (re)create on the active desktop
    void CaptureBlurredBackground(); // snapshot the desktop, blur + darken it
    void Open();
    void BuildModel();
    void LayoutGroups();
    void LayoutExpanded();
    void RegisterThumbnails();
    void UpdateThumbnails();
    void UnregisterThumbnails();
    void BuildDesktopThumbnails(); // live mini-previews of each desktop's windows
    void Render();

    // Fit `count` tiles of body aspect into `area`, returning per-tile rects
    // (rows centered). Never overflows: shrinks tiles until everything fits.
    std::vector<RECT> FluidGrid(int count, RECT area, int titleBar) const;
    // Like the native Task View: rows of uniform height, each tile's width set by
    // its window aspect ratio (so thumbnails are never stretched), scaled to fill.
    std::vector<RECT> JustifiedLayout(const std::vector<double>& aspects, RECT area, int titleBar) const;
    static double AspectOf(HWND hwnd);

    RECT CellBody(const RECT& cell) const;

    int GroupCellAtPoint(POINT pt) const;
    int SubTileAtPoint(POINT pt) const;
    int SubCloseAtPoint(POINT pt) const;
    int DesktopAtPoint(POINT pt) const;

    void MoveSelection(int dx, int dy, bool expandedSpace);
    void ActivateGroupOrExpand(int cell);
    void Expand(int group);
    void Collapse();
    void CommitWindow(HWND hwnd);
    void CloseWindowAt(int subIndex);

    static void ActivateWindow(HWND hwnd);

    HINSTANCE m_hinstance = nullptr;
    HWND m_hwnd = nullptr;
    bool m_visible = false;

    RECT m_monitor{};
    int m_gridBottom = 0;
    HBITMAP m_background = nullptr; // blurred desktop snapshot drawn behind the grid

    std::vector<AppGroup> m_groups;
    std::vector<Cell> m_cells;
    std::vector<Thumb> m_thumbs;
    std::vector<DesktopTile> m_desktops;
    std::vector<HTHUMBNAIL> m_deskThumbs; // composited window previews in the strip
    int m_currentDesktop = 0;

    int m_expandedGroup = -1;
    std::vector<RECT> m_subRects; // expanded group's window tiles
    RECT m_expandArea{};

    int m_selCell = 0; // selection in group view
    int m_selSub = 0; // selection in expanded view
    int m_hotCell = -1;
    int m_hotSub = -1;
    int m_hotClose = -1;
    int m_hotDesktop = -1;
};
