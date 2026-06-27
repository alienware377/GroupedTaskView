#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // GDI+ takes min/max from <algorithm>; suppress the Windows macros

#include <Windows.h>
#include <Unknwn.h>

// GDI+ needs min/max; pull them in before gdiplus.h.
#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h> // GDIPVER=0x0110 is set project-wide to enable 1.1 effects
#include <gdipluseffects.h>

#include <string>
#include <vector>
#include <memory>
