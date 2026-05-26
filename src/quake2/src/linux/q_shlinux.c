//
// q_shlinux.c
//
// Copyright 1998 Raven Software
// Linux port 2026.
//
// POSIX implementation of the platform-specific filesystem and timing
// utilities. Windows counterpart: src/quake2/src/win32/q_shwin.c.
//

#define _GNU_SOURCE   // for FNM_CASEFOLD in fnmatch()

#include "qcommon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>
#include <errno.h>
#include <fnmatch.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

int curtime;

// State for Sys_FindFirst / Sys_FindNext / Sys_FindClose.
static char findbase[MAX_OSPATH];
static char findpath[MAX_OSPATH];
static char findpattern[256];
static DIR* finddir = NULL;

// ============================================================================
// High-resolution timing
// ============================================================================

long long Sys_Microseconds(void)
{
    static long long base = 0;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    long long now = (long long)ts.tv_sec * 1000000ll + (long long)ts.tv_nsec / 1000ll;
    if (base == 0) base = now - 1001;
    return now - base;
}

void Sys_Nanosleep(const int nanosec)
{
    struct timespec req = { 0, nanosec };
    while (nanosleep(&req, &req) == -1 && errno == EINTR) { /* retry */ }
}

// ============================================================================
// Filesystem
// ============================================================================

void Sys_Mkdir(const char* path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        // Match Q2 behaviour of silently failing - the caller may already
        // have ensured the directory exists.
    }
}

qboolean Sys_IsDir(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode) ? true : false;
}

qboolean Sys_IsFile(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode) ? true : false;
}

// ============================================================================
// Sys_FindFirst / Sys_FindNext / Sys_FindClose
//
// These iterate over files in a directory matching a glob like "ref_*.so".
// They return a malloced-but-persistent path each call (callers do not free).
// ============================================================================

// Resolve a path's case-insensitively, component by component.
// e.g. "/home/aaron/heretic2r/base/video" might actually be ".../base/VIDEO"
// on disk; we walk each path component and find a case-folded match.
// Returns 1 on success and writes the actual on-disk path to `out`.
int H2R_ResolvePathCI(const char* path, char* out, size_t outsz)
{
    if (!path || !out || outsz == 0) return 0;
    if (path[0] != '/') {
        // Relative path - just take it as-is (we won't navigate to ".").
        strncpy(out, path, outsz - 1);
        out[outsz - 1] = '\0';
        return 1;
    }
    out[0] = '\0';
    const char* p = path + 1;
    while (*p) {
        const char* slash = strchr(p, '/');
        size_t n;
        if (slash) n = (size_t)(slash - p);
        else       n = strlen(p);

        char part[256];
        if (n >= sizeof(part)) return 0;
        memcpy(part, p, n);
        part[n] = '\0';

        // Try opening "<out>/<part>" first as exact match
        char trial[MAX_OSPATH];
        size_t outlen = strlen(out);
        if (outlen + 1 + n + 1 >= sizeof(trial)) return 0;
        snprintf(trial, sizeof(trial), "%s/%s", out, part);
        struct stat st;
        if (stat(trial, &st) == 0) {
            // Exact match. Append and continue.
            if (strlen(out) + 1 + strlen(part) + 1 >= outsz) return 0;
            strcat(out, "/");
            strcat(out, part);
        } else {
            // Try case-insensitive directory enumeration of `out`.
            DIR* d = opendir(out[0] ? out : "/");
            if (!d) return 0;
            struct dirent* de;
            char found[256] = {0};
            while ((de = readdir(d)) != NULL) {
                if (strcasecmp(de->d_name, part) == 0) {
                    strncpy(found, de->d_name, sizeof(found) - 1);
                    found[sizeof(found) - 1] = '\0';
                    if (strcmp(de->d_name, part) == 0) break; // exact wins
                }
            }
            closedir(d);
            if (!found[0]) return 0;
            if (strlen(out) + 1 + strlen(found) + 1 >= outsz) return 0;
            strcat(out, "/");
            strcat(out, found);
        }
        if (!slash) break;
        p = slash + 1;
    }
    return 1;
}

static qboolean H2R_FindFillNext(void)
{
    if (!finddir) return false;
    struct dirent* de;
    while ((de = readdir(finddir)) != NULL) {
        // Case-insensitive match to mirror Windows behaviour. H2R has many
        // hardcoded filenames in lowercase (default.cfg, intro.smk, etc.)
        // while user files often capitalise differently.
        if (fnmatch(findpattern, de->d_name, FNM_CASEFOLD) != 0) continue;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        Com_sprintf(findpath, sizeof(findpath), "%s/%s", findbase, de->d_name);
        return true;
    }
    return false;
}

char* Sys_FindFirst(const char* path, const uint musthave, const uint canthave)
{
    if (finddir) {
        Sys_Error("Sys_FindFirst called without close");
    }
    (void)musthave;
    (void)canthave;

    // Split path into base directory and glob pattern.
    char buf[MAX_OSPATH];
    Com_sprintf(buf, sizeof(buf), "%s", path);

    char* slash = strrchr(buf, '/');
    if (slash) {
        *slash = '\0';
        // Resolve the base directory's case-insensitive on-disk path.
        char resolved[MAX_OSPATH];
        if (H2R_ResolvePathCI(buf, resolved, sizeof(resolved)))
            strcpy_s(findbase, sizeof(findbase), resolved);
        else
            strcpy_s(findbase, sizeof(findbase), buf);
        strcpy_s(findpattern, sizeof(findpattern), slash + 1);
    } else {
        strcpy_s(findbase, sizeof(findbase), ".");
        strcpy_s(findpattern, sizeof(findpattern), buf);
    }

    finddir = opendir(findbase);
    if (!finddir) return NULL;

    if (H2R_FindFillNext()) return findpath;

    closedir(finddir);
    finddir = NULL;
    return NULL;
}

char* Sys_FindNext(const uint musthave, const uint canthave)
{
    (void)musthave;
    (void)canthave;
    if (!finddir) return NULL;
    if (H2R_FindFillNext()) return findpath;
    return NULL;
}

void Sys_FindClose(void)
{
    if (finddir) {
        closedir(finddir);
        finddir = NULL;
    }
}

qboolean Sys_GetWorkingDir(char* buffer, const size_t len)
{
    if (getcwd(buffer, len) != NULL) return true;
    buffer[0] = '\0';
    return false;
}

// ============================================================================
// User directory
//
// Windows: %USERPROFILE%\Saved Games\Heretic2R (via SHGetKnownFolderPath).
// Linux:   $XDG_DATA_HOME/Heretic2R or $HOME/.local/share/Heretic2R.
//
// The caller (FS_Userdir, FS_BuildPath) appends "/Heretic2R" itself; this
// function only returns the parent directory. We mirror that contract by
// returning the equivalent root.
// ============================================================================

qboolean Sys_GetOSUserDir(char* buffer, const size_t len)
{
    if (!buffer || len == 0) return false;
    buffer[0] = '\0';

    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] == '/') {
        snprintf(buffer, len, "%s", xdg);
        return true;
    }

    const char* home = getenv("HOME");
    if (!home || !home[0]) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home || !home[0]) return false;

    snprintf(buffer, len, "%s/.local/share", home);
    return true;
}

// (No external helper declarations needed - we use strcpy_s from the
//  cross-platform compat layer.)
