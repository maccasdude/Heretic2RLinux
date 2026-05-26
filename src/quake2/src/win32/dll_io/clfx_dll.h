//
// clfx_dll.h -- Client Effects library interface.
//
// Copyright 1998 Raven Software
// Linux port adjustments 2026.
//

#pragma once

#if defined(_WIN32) || defined(WIN32)
#include <windows.h>
#else
#include "compat/win_compat.h"
#endif
#include "client/client.h"

extern client_fx_import_t fxi;
extern GetfxAPI_t GetfxAPI;
extern HINSTANCE clfx_library;
extern qboolean fxapi_initialized;

extern void CLFX_Init(void);
extern void CLFX_LoadDll(void);
