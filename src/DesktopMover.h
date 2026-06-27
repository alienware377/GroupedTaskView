#pragma once

#include <Windows.h>
#include <vector>

// Moves the given top-level windows to the virtual desktop identified by
// `desktopId`, using the shell's internal COM interfaces (the only way to move
// another process's window between desktops). Returns the number moved.
//
// Designed to be safe on every Windows build, including future ones: it probes a
// table of known interface IIDs, calls methods via raw vtable slots validated by
// trial, and wraps the calls in structured exception handling. On an unknown or
// changed build it simply moves nothing instead of crashing.
int MoveWindowsToVirtualDesktop(const std::vector<HWND>& windows, const GUID& desktopId);
