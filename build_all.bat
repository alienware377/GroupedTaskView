@echo off
rem Self-locating build wrapper: initializes MSVC, then runs build.bat from this
rem script's own directory regardless of the caller's current directory.
rem (Invoke build.bat by full path: the pCloud P: drive doesn't resolve a bare
rem relative batch name as a launchable command.)
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0."
call "%~dp0build.bat"
exit /b %ERRORLEVEL%
