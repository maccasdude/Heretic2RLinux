//
// Heretic2.h
//
// Copyright 1998 Raven Software
// Linux port adjustments 2026.
//

#pragma once

#if defined(_WIN32) || defined(WIN32)
    #ifdef QUAKE2_DLL
        #define Q2DLL_DECLSPEC __declspec(dllexport)
    #else
        #define Q2DLL_DECLSPEC __declspec(dllimport)
    #endif
#else
    // On Linux we control symbol visibility via GCC attributes and rely on
    // the dynamic linker to resolve imports - so the "import" form is empty.
    #ifdef QUAKE2_DLL
        #define Q2DLL_DECLSPEC __attribute__((visibility("default")))
    #else
        #define Q2DLL_DECLSPEC
    #endif
#endif
