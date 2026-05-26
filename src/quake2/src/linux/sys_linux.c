//
// sys_linux.c
//
// Copyright 1998 Raven Software
// Linux port 2026.
//
// POSIX implementation of the Sys_* layer that lives on Windows in sys_win.c.
// Covers:
//   - Sys_Init / Sys_Error / Sys_Quit
//   - Sys_LoadGameDll / Sys_UnloadGameDll  (dlopen / dlsym / dlclose)
//   - Sys_ConsoleInput / Sys_ConsoleOutput for dedicated-server mode
//   - The Quake2Main_Linux entry point and main loop driver
//

#include "win32/Quake2Main.h"
#include "qcommon.h"
#include "client/input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dlfcn.h>

#include <SDL3/SDL.h>

#define MAX_NUM_ARGVS	128
static int   argc_g;
static char* argv_g[MAX_NUM_ARGVS];

static char console_text[256];
static int  console_textlen;

// ============================================================================
// SYSTEM I/O
// ============================================================================

H2R_NORETURN void Sys_Error(const char* error, ...)
{
    va_list argptr;
    char text[1024];

    va_start(argptr, error);
    vsnprintf(text, sizeof(text), error, argptr);
    va_end(argptr);

    const qboolean is_dedicated = (dedicated != NULL && (int)dedicated->value);

    CL_Shutdown();

    // SDL can show a native error dialog. For dedicated servers (no GUI) we
    // just write to stderr.
    if (!is_dedicated) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Heretic2R Error", text, NULL);
    }
    fprintf(stderr, "FATAL: %s\n", text);

    exit(1);
}

H2R_NORETURN void Sys_Quit(void)
{
    const qboolean is_dedicated = (dedicated != NULL && (int)dedicated->value);

    CL_Shutdown();
    (void)is_dedicated;     // nothing to do on Linux; no FreeConsole equivalent.

    exit(0);
}

// ============================================================================
// DLL HANDLING
// ============================================================================
//
// On Linux we use dlopen() to load shared objects. The H2R source hard-codes
// .dll filenames in several places ("gamex86", "Client Effects",
// "ref_*.dll", "snd_*.dll"). To avoid patching every call site we accept
// either form here:
//
//   - If the name already ends in ".so", use as-is.
//   - If the name ends in ".dll", swap the extension to ".so".
//   - Otherwise treat as a base name and append ".so".
//
// We also map a couple of Windows naming conventions to friendlier Linux
// equivalents so the data layout looks natural on disk:
//
//   "gamex86"          -> libgame.so
//   "Client Effects"   -> libclient_effects.so
//   "ref_<id>.dll"     -> libref_<id>.so
//   "snd_<id>.dll"     -> libsnd_<id>.so
//   "Player"           -> libplayer.so
//
// (gamex86 in particular has to go - "x86" is meaningless on a Linux build
// that might be x86_64, aarch64, etc.)
//

static void H2R_NameToSoPath(const char* in, char* out, size_t outsz)
{
    if (!in || !out || outsz == 0) {
        if (out && outsz) out[0] = '\0';
        return;
    }

    // Strip directory prefix from `in` for the special-name lookups, but keep
    // the prefix so the resulting path stays inside the search-path entry
    // that found it.
    const char* slash = strrchr(in, '/');
    const char* base  = slash ? slash + 1 : in;
    size_t      plen  = (size_t)(base - in);

    char prefix[1024];
    if (plen >= sizeof(prefix)) plen = sizeof(prefix) - 1;
    memcpy(prefix, in, plen);
    prefix[plen] = '\0';

    char fname[512];

    // Special-case the Windows fixed names.
    if (strcasecmp(base, "gamex86") == 0 || strcasecmp(base, "gamex86.dll") == 0) {
        snprintf(fname, sizeof(fname), "libgame.so");
    } else if (strcasecmp(base, "Client Effects") == 0 || strcasecmp(base, "Client Effects.dll") == 0) {
        snprintf(fname, sizeof(fname), "libclient_effects.so");
    } else if (strcasecmp(base, "Player") == 0 || strcasecmp(base, "Player.dll") == 0) {
        snprintf(fname, sizeof(fname), "libplayer.so");
    } else {
        // Generic rule: ref_gl1.dll -> libref_gl1.so, snd_sdl3.dll -> libsnd_sdl3.so.
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s", base);

        // Strip trailing ".dll" if present.
        size_t tl = strlen(tmp);
        if (tl >= 4 && strcasecmp(tmp + tl - 4, ".dll") == 0) {
            tmp[tl - 4] = '\0';
        }
        // Strip trailing ".so" too so we don't double-append.
        tl = strlen(tmp);
        if (tl >= 3 && strcasecmp(tmp + tl - 3, ".so") == 0) {
            tmp[tl - 3] = '\0';
        }

        snprintf(fname, sizeof(fname), "lib%s.so", tmp);
    }

    if (prefix[0])
        snprintf(out, outsz, "%s%s", prefix, fname);
    else
        snprintf(out, outsz, "%s", fname);
}

void Sys_LoadGameDll(const char* dll_name, HINSTANCE* hinst, DWORD* checksum)
{
    if (!dll_name || !hinst) return;
    *hinst    = NULL;
    if (checksum) *checksum = 0;

    char name[MAX_OSPATH];
    char so_path[MAX_OSPATH];

    // Walk the FS search paths just like the Windows side.
    char* path = NULL;
    while (1) {
        path = FS_NextPath(path);
        if (path == NULL) break;

        snprintf(name, sizeof(name), "%s/%s", path, dll_name);
        H2R_NameToSoPath(name, so_path, sizeof(so_path));

        *hinst = dlopen(so_path, RTLD_NOW | RTLD_GLOBAL);
        if (*hinst != NULL) {
            Com_DDPrintf(2, "dlopen (%s)\n", so_path);
            break;
        } else {
            // useful for debugging missing-lib chains.
            Com_DDPrintf(2, "dlopen(%s) failed: %s\n", so_path, dlerror());
        }
    }

    // Fallback: try the system loader path (LD_LIBRARY_PATH, /usr/lib, ...).
    if (*hinst == NULL) {
        H2R_NameToSoPath(dll_name, so_path, sizeof(so_path));
        *hinst = dlopen(so_path, RTLD_NOW | RTLD_GLOBAL);
        if (*hinst != NULL) {
            Com_DDPrintf(2, "dlopen system path (%s)\n", so_path);
        }
    }

    if (*hinst == NULL) {
        Sys_Error("Failed to load %s (%s)", dll_name, dlerror());
    }

    // The checksum that came from the PE header on Windows is never actually
    // used by the engine (the source itself notes "result never used"). Just
    // hand back 0 on Linux.
    if (checksum) *checksum = 0;
}

void Sys_UnloadGameDll(const char* name, HINSTANCE* hinst)
{
    if (!hinst || !*hinst) return;
    if (dlclose(*hinst) != 0)
        Sys_Error("Failed to unload %s (%s)", name ? name : "?", dlerror());
    *hinst = NULL;
}

// ============================================================================
// Sys_Init / console I/O for dedicated server
// ============================================================================

static struct termios saved_tios;
static int            tty_dirty = 0;

static void H2R_RestoreTty(void)
{
    if (tty_dirty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tios);
        tty_dirty = 0;
    }
}

void Sys_Init(void)
{
    Set_Com_Printf(Com_Printf); // H2

    // No timeBeginPeriod equivalent needed on Linux - CLOCK_MONOTONIC has
    // microsecond resolution out of the box.

    if ((int)dedicated->value) {
        // Switch stdin to non-canonical (no line buffering, no echo) so we
        // can do char-at-a-time input like the Windows dedicated console.
        struct termios tios;
        if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &tios) == 0) {
            saved_tios = tios;
            tty_dirty  = 1;
            atexit(H2R_RestoreTty);

            tios.c_lflag &= ~(ICANON | ECHO);
            tios.c_cc[VMIN]  = 0;
            tios.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &tios);
        }
        // Non-blocking reads so Sys_ConsoleInput can poll cheaply.
        int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (fl != -1) fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
    }
}

char* Sys_ConsoleInput(void)
{
    if (dedicated == NULL || !(int)dedicated->value)
        return NULL;

    char ch;
    while (read(STDIN_FILENO, &ch, 1) == 1) {
        switch (ch) {
        case '\n':
        case '\r':
            // echo CRLF
            { ssize_t r = write(STDOUT_FILENO, "\n", 1); (void)r; }
            if (console_textlen > 0) {
                console_text[console_textlen] = 0;
                console_textlen = 0;
                return console_text;
            }
            break;

        case 8:    // BS
        case 127:  // DEL
            if (console_textlen > 0) {
                console_textlen--;
                ssize_t r = write(STDOUT_FILENO, "\b \b", 3); (void)r;
            }
            break;

        default:
            if (ch >= ' ' && ch < 0x7f && console_textlen < (int)sizeof(console_text) - 2) {
                ssize_t r = write(STDOUT_FILENO, &ch, 1); (void)r;
                console_text[console_textlen++] = ch;
            }
            break;
        }
    }
    return NULL;
}

void Sys_ConsoleOutput(const char* string)
{
    if (dedicated == NULL || !(int)dedicated->value)
        return;

    // If the user is mid-input, blank the in-progress line first so the
    // server output doesn't smear over their typing.
    if (console_textlen > 0) {
        char blank[260];
        blank[0] = '\r';
        memset(&blank[1], ' ', console_textlen);
        blank[console_textlen + 1] = '\r';
        blank[console_textlen + 2] = 0;
        ssize_t r = write(STDOUT_FILENO, blank, console_textlen + 2); (void)r;
    }

    size_t len = strlen(string);
    ssize_t r = write(STDOUT_FILENO, string, len); (void)r;

    if (console_textlen > 0) {
        r = write(STDOUT_FILENO, console_text, console_textlen); (void)r;
    }
}

// ============================================================================
// Main loop / entry point
// ============================================================================
//
// On Windows this is Quake2Main(HINSTANCE, ..., LPSTR cmdline, ...). The
// Linux launcher passes us a normal argc/argv via Quake2Main_Linux(), and we
// run the same loop.
//

extern void Sys_CpuPause(void);     // input_sdl3.c

Q2DLL_DECLSPEC int Quake2Main_Linux(int argc, char** argv)
{
    if (argc > MAX_NUM_ARGVS) argc = MAX_NUM_ARGVS;
    argc_g = argc;
    for (int i = 0; i < argc; i++) argv_g[i] = argv[i];

    Qcommon_Init(argc_g, argv_g);

    long long oldtime = Sys_Microseconds();

    while (1) {
        const long long spintime = Sys_Microseconds();

        // YQ2 busywait logic - same on both platforms.
        while (Sys_Microseconds() - spintime < 5)
            Sys_CpuPause();

        const long long newtime = Sys_Microseconds();
        curtime = (int)(newtime / 1000ll);

        Qcommon_Frame((int)(newtime - oldtime));
        oldtime = newtime;
    }
    // unreachable
}
