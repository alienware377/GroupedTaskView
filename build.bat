@echo off
rem Build Grouped Task View and its installer. Run from a Visual Studio
rem Developer Command Prompt (so cl.exe and rc.exe are on PATH).
setlocal

where cl >nul 2>nul || (echo [ERROR] cl.exe not found. Run this from a Visual Studio Developer Command Prompt. & exit /b 1)

if not exist build mkdir build

echo === Resource: app ===
rc /nologo /fo build\app.res src\app.rc || exit /b 1

echo === Compile: GroupedTaskView.exe ===
cl /nologo /std:c++17 /EHsc /W3 /O2 /MT /DUNICODE /D_UNICODE /DNDEBUG /DGDIPVER=0x0110 ^
  src\main.cpp src\AltTabGrouped.cpp src\TaskView.cpp src\WindowEnumerator.cpp ^
  src\AppGrouping.cpp src\VirtualDesktops.cpp src\TrayIcon.cpp src\DesktopMover.cpp ^
  src\Settings.cpp src\SettingsWindow.cpp build\app.res ^
  /Fo:build\ /Fe:build\GroupedTaskView.exe ^
  /link /SUBSYSTEM:WINDOWS gdiplus.lib dwmapi.lib user32.lib gdi32.lib shell32.lib ^
  shlwapi.lib ole32.lib oleaut32.lib version.lib uxtheme.lib winmm.lib advapi32.lib comctl32.lib || exit /b 1

echo === Resource: installer (embeds app) ===
rc /nologo /fo build\installer.res installer\installer.rc || exit /b 1

echo === Compile: GroupedTaskView-Setup.exe ===
cl /nologo /std:c++17 /EHsc /W3 /O2 /MT /DUNICODE /D_UNICODE /DNDEBUG ^
  installer\Installer.cpp build\installer.res ^
  /Fo:build\ /Fe:build\GroupedTaskView-Setup.exe ^
  /link /SUBSYSTEM:WINDOWS shell32.lib shlwapi.lib ole32.lib user32.lib advapi32.lib || exit /b 1

echo.
echo Done:
echo   build\GroupedTaskView.exe
echo   build\GroupedTaskView-Setup.exe
