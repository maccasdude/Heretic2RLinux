//
// H2Common.h
//
// Copyright 1998 Raven Software
// Linux port adjustments 2026.
//

#pragma once

#if defined(_WIN32) || defined(WIN32)
    #ifdef H2COMMON
        #define H2COMMON_API __declspec(dllexport)
    #else
        #define H2COMMON_API __declspec(dllimport)
    #endif
#else
    #ifdef H2COMMON
        #define H2COMMON_API __attribute__((visibility("default")))
    #else
        #define H2COMMON_API
    #endif

    // MSVC's <windows.h> pulls in <limits.h> and <float.h> transitively
    // for nearly every translation unit. GCC does not - so any H2 source
    // that uses INT_MAX, FLT_MAX, etc. fails to compile. Pull them here.
    #include <limits.h>
    #include <float.h>

    // MSVC's <windows.h> provides these as macros; many H2 source files
    // expect them to exist unconditionally. Define here so any file that
    // includes H2Common.h (most everything) gets them. C++ has its own
    // std::min/std::max templates in <algorithm>, so don't macro-clobber
    // them there.
    #ifndef __cplusplus
        #ifndef max
        #  define max(a, b) (((a) > (b)) ? (a) : (b))
        #endif
        #ifndef min
        #  define min(a, b) (((a) < (b)) ? (a) : (b))
        #endif
    #endif
#endif
