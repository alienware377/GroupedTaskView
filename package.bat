@echo off
rem Build + code-sign both binaries. The installer embeds the app exe as a
rem resource, so the app is signed BEFORE the installer is built (so the copy
rem inside the installer is signed too), then the installer is signed.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0."

set SIGNTOOL="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
set THUMB=82B9ABD72AB2540F6A75D4E24E94C748191CEC23
set TS=http://timestamp.digicert.com

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

echo === Sign: GroupedTaskView.exe ===
%SIGNTOOL% sign /fd SHA256 /sha1 %THUMB% /tr %TS% /td SHA256 build\GroupedTaskView.exe || exit /b 1

echo === Resource: installer (embeds signed app) ===
rc /nologo /fo build\installer.res installer\installer.rc || exit /b 1

echo === Compile: GroupedTaskView-Setup.exe ===
cl /nologo /std:c++17 /EHsc /W3 /O2 /MT /DUNICODE /D_UNICODE /DNDEBUG ^
  installer\Installer.cpp build\installer.res ^
  /Fo:build\ /Fe:build\GroupedTaskView-Setup.exe ^
  /link /SUBSYSTEM:WINDOWS shell32.lib shlwapi.lib ole32.lib user32.lib advapi32.lib || exit /b 1

echo === Sign: GroupedTaskView-Setup.exe ===
%SIGNTOOL% sign /fd SHA256 /sha1 %THUMB% /tr %TS% /td SHA256 build\GroupedTaskView-Setup.exe || exit /b 1

echo.
echo Done (signed):
echo   build\GroupedTaskView.exe
echo   build\GroupedTaskView-Setup.exe
exit /b 0
