//
// win_compat.c
//
// Implementations of the stateful pieces of win_compat.h.
// (Inline shims live in the header; this file only holds things that need
// persistent state, like the directory-iteration handles.)
//
// (c) 2026 Heretic2R Linux port.
//

#ifndef _WIN32

#define _GNU_SOURCE
#include "compat/win_compat.h"

#include <dirent.h>
#include <fnmatch.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// One open enumerator per _findfirst() call.
typedef struct H2R_FindCtx_s {
    DIR* dir;
    char base[1024];
    char pattern[256];
} H2R_FindCtx;

static int H2R_apply_attrs(const char* fullpath, struct _finddata_t* out) {
    struct stat st;
    if (stat(fullpath, &st) != 0) {
        out->attrib = _A_NORMAL;
        out->size   = 0;
        return 0;
    }
    out->size = (long)st.st_size;
    out->attrib = S_ISDIR(st.st_mode) ? _A_SUBDIR : _A_NORMAL;
    // Treat dot-files as hidden, mirroring Windows convention loosely.
    const char* base = strrchr(fullpath, '/');
    base = base ? base + 1 : fullpath;
    if (base[0] == '.' && strcmp(base, ".") != 0 && strcmp(base, "..") != 0) {
        out->attrib |= _A_HIDDEN;
    }
    return 0;
}

static int H2R_match_one(H2R_FindCtx* ctx, struct _finddata_t* out) {
    struct dirent* de;
    while ((de = readdir(ctx->dir)) != NULL) {
        if (fnmatch(ctx->pattern, de->d_name, 0) == 0) {
            strncpy(out->name, de->d_name, sizeof(out->name) - 1);
            out->name[sizeof(out->name) - 1] = '\0';

            char full[2048];
            if (ctx->base[0] != '\0') {
                snprintf(full, sizeof(full), "%s/%s", ctx->base, de->d_name);
            } else {
                snprintf(full, sizeof(full), "%s", de->d_name);
            }
            H2R_apply_attrs(full, out);
            return 0;
        }
    }
    return -1;
}

H2R_find_handle_t H2R_findfirst(const char* pattern, struct _finddata_t* out) {
    if (!pattern || !out) return -1;

    H2R_FindCtx* ctx = (H2R_FindCtx*)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;

    // Split "dir/segment/*.dll" -> base="dir/segment", pattern="*.dll".
    const char* slash = strrchr(pattern, '/');
    if (slash) {
        size_t blen = (size_t)(slash - pattern);
        if (blen >= sizeof(ctx->base)) blen = sizeof(ctx->base) - 1;
        memcpy(ctx->base, pattern, blen);
        ctx->base[blen] = '\0';
        strncpy(ctx->pattern, slash + 1, sizeof(ctx->pattern) - 1);
    } else {
        ctx->base[0] = '\0';                       // current directory
        strncpy(ctx->pattern, pattern, sizeof(ctx->pattern) - 1);
    }

    ctx->dir = opendir(ctx->base[0] ? ctx->base : ".");
    if (!ctx->dir) {
        free(ctx);
        return -1;
    }

    if (H2R_match_one(ctx, out) != 0) {
        closedir(ctx->dir);
        free(ctx);
        return -1;
    }

    return (H2R_find_handle_t)ctx;
}

int H2R_findnext(H2R_find_handle_t h, struct _finddata_t* out) {
    if (h == 0 || h == -1 || !out) return -1;
    H2R_FindCtx* ctx = (H2R_FindCtx*)h;
    return H2R_match_one(ctx, out);
}

int H2R_findclose(H2R_find_handle_t h) {
    if (h == 0 || h == -1) return 0;
    H2R_FindCtx* ctx = (H2R_FindCtx*)h;
    if (ctx->dir) closedir(ctx->dir);
    free(ctx);
    return 0;
}

#endif // !_WIN32
