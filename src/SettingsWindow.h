#pragma once

#include <Windows.h>

// Opens (or focuses) the settings window for managing grouping / rename rules.
// Optionally pre-fills the Match / Group-name fields (used by "rename group").
void ShowSettingsWindow(HINSTANCE hinstance, const wchar_t* prefillMatch = nullptr, const wchar_t* prefillName = nullptr);
