//
// Quake2Main.h -- Exposes the only function needed by the launcher.
//
// Copyright 1998 Raven Software
// Linux port adjustments 2026.
//

#pragma once

#if defined(_WIN32) || defined(WIN32)
#include <windows.h>
#include "Heretic2.h"
Q2DLL_DECLSPEC int Quake2Main(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd);
#else
#include "compat/win_compat.h"
#include "Heretic2.h"
// On Linux there's no separate launcher exe - the engine library exposes a
// POSIX-style entry point that just takes argc/argv. The shim launcher
// binary calls this with the user's command line.
Q2DLL_DECLSPEC int Quake2Main_Linux(int argc, char** argv);
#endif
