//
// win_compat.h
//
// Compatibility shims for porting MSVC/Win32 code to GCC/Linux.
// Provides:
//   - Win32 integer / handle typedefs (DWORD, BOOL, HINSTANCE, HMODULE, ...).
//   - Stand-ins for MSVC's "safe" string functions (sprintf_s, strcpy_s, ...).
//   - Stand-ins for MSVC's _-prefixed POSIX-ish functions (_findfirst, _mkdir, ...).
//   - DLL export/import qualifier and noreturn macros.
//
// This header is included automatically from q_Typedef.h on non-Windows
// platforms so the rest of the engine source stays unchanged.
//
// (c) 2026 Heretic2R Linux port.
//

#pragma once

#if defined(_WIN32) || defined(WIN32)
// On Windows this header is a no-op; the real Win32 headers are pulled in
// the usual way by the platform-specific .c files.
#else

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>     // strcasecmp, strncasecmp
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>      // INT_MAX, etc. (windows.h pulls these in transitively)
#include <float.h>       // FLT_MAX, DBL_MAX
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <dlfcn.h>

// ---------------------------------------------------------------------------
// Win32 integer typedefs
// ---------------------------------------------------------------------------
typedef int32_t        BOOL;
typedef uint8_t        BYTE;
typedef uint16_t       WORD;
typedef uint32_t       DWORD;
typedef int32_t        INT;
typedef uint32_t       UINT;
typedef int32_t        LONG;
typedef uint32_t       ULONG;
typedef int64_t        LONGLONG;
typedef uint64_t       ULONGLONG;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

// HINSTANCE / HMODULE etc are just opaque void* handles in our Linux port.
typedef void*          HINSTANCE;
typedef void*          HMODULE;
typedef void*          HANDLE;
typedef void*          HWND;
typedef char*          LPSTR;
typedef const char*    LPCSTR;
typedef DWORD*         LPDWORD;

#define WINAPI
#define APIENTRY

// Windows DLL loading API -> POSIX dlopen on Linux.
static inline void* H2R_GetProcAddress(void* h, const char* sym) {
    return h ? dlsym(h, sym) : NULL;
}
static inline int H2R_FreeLibrary(void* h) {
    return (h && dlclose(h) == 0) ? 1 : 0;   // win32 returns nonzero on success
}
// LoadLibrary on Windows accepts a bare name like "snd_sdl3" and auto-appends
// ".dll", searching the PATH. On Linux dlopen needs an explicit ".so" suffix
// and won't auto-find anything. We try a few common transforms so callers
// don't have to know about the extension difference.
static inline void* H2R_LoadLibrary(const char* path) {
    if (!path || !*path) return NULL;

    // 1. Try the literal name first. Allows callers to pass an exact path.
    void* h = dlopen(path, 2 | 0x100);  // RTLD_NOW | RTLD_GLOBAL
    if (h) return h;

    char buf[1024];

    // 2. If the name ends in ".dll" - swap that suffix for ".so".
    size_t len = strlen(path);
    if (len >= 4 && strcasecmp(path + len - 4, ".dll") == 0) {
        if (len + 1 < sizeof(buf)) {
            memcpy(buf, path, len - 4);
            memcpy(buf + len - 4, ".so", 4);   // includes NUL
            h = dlopen(buf, 2 | 0x100);
            if (h) return h;
            // also try lib<name>.so
            if (len + 3 < sizeof(buf)) {
                memcpy(buf, "lib", 3);
                memcpy(buf + 3, path, len - 4);
                memcpy(buf + 3 + len - 4, ".so", 4);
                h = dlopen(buf, 2 | 0x100);
                if (h) return h;
                // and ./lib<name>.so for binary cwd searches
                if (len + 5 < sizeof(buf)) {
                    memcpy(buf, "./lib", 5);
                    memcpy(buf + 5, path, len - 4);
                    memcpy(buf + 5 + len - 4, ".so", 4);
                    h = dlopen(buf, 2 | 0x100);
                    if (h) return h;
                }
            }
        }
        return NULL;
    }

    // 3. Bare name (no extension): try "<name>.so", "lib<name>.so", and
    //    "./lib<name>.so" so it works from the cwd of the binary too.
    if (len + 4 < sizeof(buf)) {
        snprintf(buf, sizeof(buf), "%s.so", path);
        h = dlopen(buf, 2 | 0x100);
        if (h) return h;
    }
    if (len + 7 < sizeof(buf)) {
        snprintf(buf, sizeof(buf), "lib%s.so", path);
        h = dlopen(buf, 2 | 0x100);
        if (h) return h;
    }
    if (len + 9 < sizeof(buf)) {
        snprintf(buf, sizeof(buf), "./lib%s.so", path);
        h = dlopen(buf, 2 | 0x100);
        if (h) return h;
    }

    return NULL;
}
#define GetProcAddress(h, s)            H2R_GetProcAddress((h), (s))
#define FreeLibrary(h)                  H2R_FreeLibrary((h))
#define LoadLibrary(p)                  H2R_LoadLibrary((p))

// SAL-like annotations used in launcher/main.c - just no-ops.
#define _In_
#define _Out_
#define _Inout_
#define _In_opt_
#define _Out_opt_
#define _In_bytecount_(x)
#define _Out_bytecap_(x)
#define _Out_cap_(x)
#define _Out_z_bytecap_(x)
#define _Out_z_cap_(x)

// MSVC-style noreturn / forceinline / declspec equivalents.
#ifndef H2R_NORETURN
#  if defined(__GNUC__) || defined(__clang__)
#    define H2R_NORETURN __attribute__((noreturn))
#  else
#    define H2R_NORETURN
#  endif
#endif

#ifndef __forceinline
#  define __forceinline static inline __attribute__((always_inline))
#endif

// MSVC's _inline is just inline. Use 'static inline' so each TU gets a
// private copy and no external symbol is required.
#ifndef _inline
#  define _inline static inline
#endif

// ---------------------------------------------------------------------------
// DLL export / import qualifiers
//
// The H2R codebase decorates every cross-DLL symbol with Q2DLL_DECLSPEC /
// H2COMMON_API which expand to __declspec(dllexport|dllimport). On Linux we
// use GCC visibility attributes: exported symbols get default visibility, the
// rest stay hidden (so we'd compile the libs with -fvisibility=hidden).
// We don't need an "import" form on ELF - the dynamic linker resolves at
// runtime - so the import branch is empty.
// ---------------------------------------------------------------------------
#if defined(__GNUC__) && __GNUC__ >= 4
#  define H2R_DLL_EXPORT __attribute__((visibility("default")))
#  define H2R_DLL_IMPORT
#else
#  define H2R_DLL_EXPORT
#  define H2R_DLL_IMPORT
#endif

// The H2Common.h / Heretic2.h headers redefine these macros themselves based
// on the H2COMMON / QUAKE2_DLL build flags. To override their Windows-only
// __declspec(...) definitions on Linux we set a sentinel here, then patch
// those two headers to honour it.
#define H2R_USE_GCC_VISIBILITY 1

// ---------------------------------------------------------------------------
// MSVC "safe" string / IO function stand-ins
//
// MSVC's _s variants take an explicit destination-size argument. Our shims
// drop the size argument and call the plain POSIX function - which is safe in
// practice because the calling code already passes sizeof(buf) as the size
// argument, matching the buffer it's writing into. The return values are also
// compatible with how H2R uses them (it almost never checks them; when it
// does, 0 = success and non-zero = failure for fopen_s).
// ---------------------------------------------------------------------------
#define sprintf_s(buf, sz, ...)         snprintf((buf), (sz), __VA_ARGS__)
#define _snprintf_s(buf, sz, cnt, ...)  snprintf((buf), (sz), __VA_ARGS__)
#define vsprintf_s(buf, sz, fmt, ap)    vsnprintf((buf), (sz), (fmt), (ap))
#define _vsnprintf_s(buf, sz, cnt, fmt, ap) vsnprintf((buf), (sz), (fmt), (ap))
#define sscanf_s                        sscanf

static inline int H2R_strcpy_s(char* dst, size_t sz, const char* src) {
    if (!dst || sz == 0) return EINVAL;
    if (!src) { dst[0] = '\0'; return EINVAL; }
    size_t n = strlen(src);
    if (n >= sz) { dst[0] = '\0'; return ERANGE; }
    memcpy(dst, src, n + 1);
    return 0;
}
static inline int H2R_strncpy_s(char* dst, size_t sz, const char* src, size_t cnt) {
    if (!dst || sz == 0) return EINVAL;
    if (!src) { dst[0] = '\0'; return EINVAL; }
    size_t n = strnlen(src, cnt);
    if (n >= sz) { dst[0] = '\0'; return ERANGE; }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return 0;
}
static inline int H2R_strcat_s(char* dst, size_t sz, const char* src) {
    if (!dst || sz == 0 || !src) return EINVAL;
    size_t dl = strnlen(dst, sz);
    if (dl == sz) return EINVAL;
    size_t sl = strlen(src);
    if (dl + sl + 1 > sz) { dst[0] = '\0'; return ERANGE; }
    memcpy(dst + dl, src, sl + 1);
    return 0;
}
static inline int H2R_strncat_s(char* dst, size_t sz, const char* src, size_t cnt) {
    if (!dst || sz == 0 || !src) return EINVAL;
    size_t dl = strnlen(dst, sz);
    if (dl == sz) return EINVAL;
    size_t sl = strnlen(src, cnt);
    if (dl + sl + 1 > sz) { dst[0] = '\0'; return ERANGE; }
    memcpy(dst + dl, src, sl);
    dst[dl + sl] = '\0';
    return 0;
}
static inline int H2R_memcpy_s(void* dst, size_t sz, const void* src, size_t cnt) {
    if (!dst || (!src && cnt > 0) || cnt > sz) return EINVAL;
    memcpy(dst, src, cnt);
    return 0;
}
static inline int H2R_fopen_s(FILE** pf, const char* path, const char* mode) {
    if (!pf) return EINVAL;
    *pf = fopen(path, mode);
    return (*pf == NULL) ? errno : 0;
}
static inline int H2R_localtime_s(struct tm* out, const time_t* t) {
    if (!out || !t) return EINVAL;
    return (localtime_r(t, out) == NULL) ? errno : 0;
}
static inline int H2R_memmove_s(void* dst, size_t sz, const void* src, size_t cnt) {
    if (!dst || (!src && cnt > 0) || cnt > sz) return EINVAL;
    memmove(dst, src, cnt);
    return 0;
}
static inline size_t H2R_fread_s(void* dst, size_t sz, size_t esz, size_t cnt, FILE* fp) {
    if (!dst || !fp || cnt * esz > sz) return 0;
    return fread(dst, esz, cnt, fp);
}
static inline char* H2R_strtok_s(char* str, const char* delim, char** ctx) {
    return strtok_r(str, delim, ctx);
}

#define strcpy_s(d, s, src)             H2R_strcpy_s((d), (s), (src))
#define strncpy_s(d, s, src, c)         H2R_strncpy_s((d), (s), (src), (c))
#define strcat_s(d, s, src)             H2R_strcat_s((d), (s), (src))
#define strncat_s(d, s, src, c)         H2R_strncat_s((d), (s), (src), (c))
#define memcpy_s(d, s, src, c)          H2R_memcpy_s((d), (s), (src), (c))
#define memmove_s(d, s, src, c)         H2R_memmove_s((d), (s), (src), (c))
#define fopen_s(p, path, mode)          H2R_fopen_s((p), (path), (mode))
#define localtime_s(out, t)             H2R_localtime_s((out), (t))
#define fread_s(d, sz, esz, cnt, fp)    H2R_fread_s((d), (sz), (esz), (cnt), (fp))
#define strtok_s(s, d, c)               H2R_strtok_s((s), (d), (c))

// MSVC provides template overloads of these functions in C++ that take a
// char array reference and infer the size. Some H2R C++ code uses those
// 2-argument forms. Undefine the C-style macros on the C++ side and provide
// proper overloads that accept either form.
#ifdef __cplusplus
#undef strcpy_s
#undef strncpy_s
#undef strcat_s
#undef sprintf_s
#undef vsprintf_s
extern "C++" {
    // 3-arg form (matches the C macro). Templates to allow any pointer type.
    inline int strcpy_s(char* dst, size_t sz, const char* src) {
        return H2R_strcpy_s(dst, sz, src);
    }
    inline int strncpy_s(char* dst, size_t sz, const char* src, size_t cnt) {
        return H2R_strncpy_s(dst, sz, src, cnt);
    }
    inline int strcat_s(char* dst, size_t sz, const char* src) {
        return H2R_strcat_s(dst, sz, src);
    }

    // 2-arg array form (MSVC C++ template overload).
    template <size_t N>
    inline int strcpy_s(char (&dst)[N], const char* src) {
        return H2R_strcpy_s(dst, N, src);
    }
    template <size_t N>
    inline int strcat_s(char (&dst)[N], const char* src) {
        return H2R_strcat_s(dst, N, src);
    }
    template <size_t N>
    inline int strncpy_s(char (&dst)[N], const char* src, size_t cnt) {
        return H2R_strncpy_s(dst, N, src, cnt);
    }

    // vsprintf_s: MSVC has both vsprintf_s(buf, sz, fmt, ap) [C-style] and
    // a 3-arg C++ array overload vsprintf_s(arr, fmt, ap).
    inline int vsprintf_s(char* dst, size_t sz, const char* fmt, va_list ap) {
        return vsnprintf(dst, sz, fmt, ap);
    }
    template <size_t N>
    inline int vsprintf_s(char (&dst)[N], const char* fmt, va_list ap) {
        return vsnprintf(dst, N, fmt, ap);
    }

    // sprintf_s: 3-arg (buf, sz, fmt, ...) and 2-arg array form (arr, fmt, ...).
    inline int sprintf_s(char* dst, size_t sz, const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        int r = vsnprintf(dst, sz, fmt, ap);
        va_end(ap);
        return r;
    }
    template <size_t N>
    inline int sprintf_s(char (&dst)[N], const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        int r = vsnprintf(dst, N, fmt, ap);
        va_end(ap);
        return r;
    }
}
#endif

// ---------------------------------------------------------------------------
// MSVC POSIX-ish underscore aliases
// ---------------------------------------------------------------------------
#define _stricmp                        strcasecmp
#define _strnicmp                       strncasecmp
#define _strdup                         strdup
#define _strlwr                         H2R_strlwr_inplace
#define _strupr                         H2R_strupr_inplace
#define _unlink                         unlink
#define _chdir                          chdir
#define _getcwd                         getcwd

static inline char* H2R_strlwr_inplace(char* s) {
    if (!s) return NULL;
    for (char* p = s; *p; ++p) *p = (char)tolower((unsigned char)*p);
    return s;
}
static inline char* H2R_strupr_inplace(char* s) {
    if (!s) return NULL;
    for (char* p = s; *p; ++p) *p = (char)toupper((unsigned char)*p);
    return s;
}

// _mkdir on Windows takes one arg and creates with default ACLs. We use 0755.
static inline int H2R_mkdir1(const char* path) {
    return mkdir(path, 0755);
}
#define _mkdir(path)                    H2R_mkdir1(path)

// ---------------------------------------------------------------------------
// _findfirst / _findnext / _findclose family
//
// MSVC's I/O find functions wrap the Windows directory enumeration APIs.
// H2R uses them in q_shwin.c (filesystem scan helpers) and also indirectly
// via Sys_FindFirst/Sys_FindNext. On Linux we wrap opendir/readdir to give
// the same iterator semantics.
// ---------------------------------------------------------------------------
#define _A_RDONLY  0x01
#define _A_HIDDEN  0x02
#define _A_SYSTEM  0x04
#define _A_SUBDIR  0x10
#define _A_ARCH    0x20
#define _A_NORMAL  0x00

struct _finddata_t {
    unsigned attrib;
    long     size;
    char     name[260];
};

typedef intptr_t H2R_find_handle_t;

H2R_find_handle_t H2R_findfirst(const char* pattern, struct _finddata_t* out);
int               H2R_findnext (H2R_find_handle_t h,  struct _finddata_t* out);
int               H2R_findclose(H2R_find_handle_t h);

#define _findfirst(p, d)                H2R_findfirst((p), (d))
#define _findnext(h, d)                 H2R_findnext((h), (d))
#define _findclose(h)                   H2R_findclose((h))

// ---------------------------------------------------------------------------
// Misc helpers
// ---------------------------------------------------------------------------
// max / min are Win32 macros; the H2R code uses them in a couple of places.
// Gate to C only - C++ has std::min/std::max templates.
#ifndef __cplusplus
#  ifndef max
#    define max(a, b) (((a) > (b)) ? (a) : (b))
#  endif
#  ifndef min
#    define min(a, b) (((a) < (b)) ? (a) : (b))
#  endif
#endif

#endif // !_WIN32
