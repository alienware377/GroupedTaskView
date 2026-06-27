<div align="center">

<img src="assets/icon.png" width="120" alt="Grouped Task View logo" />

# Grouped Task View

**A Win+Tab replacement for Windows 10 & 11 that groups your open windows by application.**

Press <kbd>Win</kbd>+<kbd>Tab</kbd> and get a Task-View-style overlay where every window is clustered into a per-app stack — so 8 File Explorer windows and 6 Chrome windows don't drown out everything else. Live thumbnails, a blurred acrylic backdrop, and your virtual desktops, all on one screen.

[![Build](https://github.com/alienware377/GroupedTaskView/actions/workflows/build.yml/badge.svg)](https://github.com/alienware377/GroupedTaskView/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/alienware377/GroupedTaskView)](https://github.com/alienware377/GroupedTaskView/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-0078D7)

</div>

## What it does

Windows' built-in **Task View** (<kbd>Win</kbd>+<kbd>Tab</kbd>) shows every window in a flat grid. When you keep a lot of windows open, that grid gets noisy fast. **Grouped Task View** replaces it with the same fast, full-screen switcher — but windows are **grouped by app into stacks**, sized to fit on a single screen with no scrolling.

## Features

- 🗂️ **Group windows by application** — one tile per app, multi-window apps become a stack with a count badge.
- 🖼️ **Live window thumbnails** — real-time DWM previews, sized to each window's aspect ratio (no stretching).
- 🌫️ **Acrylic blurred backdrop** — a smooth, saturated, frosted blur of your wallpaper.
- 🪟 **All virtual desktops** — switch to any window on any desktop; a desktop strip with live previews runs along the bottom.
- ↪️ **Drag to another desktop** — drag a window, or a whole app group, onto a desktop in the strip to move it there (the live thumbnail lifts off and follows the cursor).
- ✏️ **Rename groups & custom grouping** — right-click a group to rename it, or open **Settings** to define your own rules (merge several apps into one group, or rename any group). Rules persist across restarts.
- ⌨️🖱️ **Keyboard and mouse** — arrow keys / Tab to move, Enter or click to switch, click the ✕ to close a window, click a desktop to jump to it.
- 🪶 **Tiny and self-contained** — a single ~250 KB executable, no runtime or dependencies, no telemetry.
- 🔔 **System tray icon** — quit anytime from the notification area.

## Install

1. Download **`GroupedTaskView-Setup.exe`** from the [latest release](https://github.com/alienware377/GroupedTaskView/releases/latest).
2. Run it. It installs to your user profile (no admin rights), adds a Start Menu shortcut, starts at sign-in, and launches immediately.
3. Press <kbd>Win</kbd>+<kbd>Tab</kbd>.

> The installer is **self-signed**, so Windows SmartScreen may show a "Windows protected your PC" prompt. Click **More info → Run anyway**. (A self-signed certificate is not chained to a commercial CA — the binaries are signed, just not by a paid authority.)

## Controls

| Input | Action |
|---|---|
| <kbd>Win</kbd>+<kbd>Tab</kbd> | Open / close the overlay |
| Arrow keys / <kbd>Tab</kbd> | Move the selection |
| <kbd>Enter</kbd> or click | Switch to the window (or expand a multi-window app) |
| Click the **✕** | Close that window |
| Click a desktop | Switch to it ( **＋** creates a new one ) |
| Drag a tile onto a desktop | Move that window / app group to that desktop |
| Right-click a group | Rename it, or open Settings |
| <kbd>Esc</kbd> | Close without switching |

## Customize grouping

Right-click any group and choose **Rename**, or open **Settings** from the tray icon. A rule matches any window whose app name or `.exe` **contains** the *Match* text and places it in the named group. Use one rule to rename a single app's group, or give several rules the **same Group name** to merge those apps into one group. Rules are saved to `%LOCALAPPDATA%\GroupedTaskView\settings.ini`.

## How it works

A low-level keyboard hook (on its own thread, so it's never starved) intercepts <kbd>Win</kbd>+<kbd>Tab</kbd> before the shell. It enumerates the alt-tab-eligible windows across all virtual desktops, groups them by application identity (executable path, or AppUserModelID for packaged apps), and renders a full-screen overlay with live [DWM thumbnails](https://learn.microsoft.com/windows/win32/dwm/thumbnail-ovw). Virtual-desktop membership comes from the public `IVirtualDesktopManager`; the desktop list/names/wallpapers are read from the registry.

## Build from source

Requires Visual Studio Build Tools (MSVC) and the Windows SDK. From a **Developer Command Prompt**:

```bat
build.bat
```

Outputs `build\GroupedTaskView.exe` and the all-in-one `build\GroupedTaskView-Setup.exe`.

## Uninstall

Use **Settings → Apps → Installed apps → Grouped Task View**, or run `uninstall.exe` from the install folder (`%LOCALAPPDATA%\GroupedTaskView`). Quit the running app first from its tray icon.

## License

[MIT](LICENSE)

---

<sub>Keywords: Windows Win+Tab replacement, group windows by app, Task View alternative, window switcher, alt-tab grouping, virtual desktops, productivity.</sub>
