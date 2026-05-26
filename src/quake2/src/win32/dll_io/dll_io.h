//
// dll_io.h -- DLL loading/unloading logic.
//
// Copyright 1998 Raven Software
// Linux port adjustments 2026.
//

#pragma once

#if defined(_WIN32) || defined(WIN32)
#include <windows.h>
#else
#include "compat/win_compat.h"
// LoadLibrary/GetProcAddress/FreeLibrary wrappers live in win_compat.h.
#endif

extern void Sys_LoadGameDll(const char* dll_name, HINSTANCE* hinst, DWORD* checksum);
extern void Sys_UnloadGameDll(const char* name, HINSTANCE* hinst);
