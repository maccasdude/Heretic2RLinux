//
// Hunk.c
//
// Copyright 1998 Raven Software
// Linux port adjustments 2026.
//
// On Windows the renderer's "hunk" allocator uses VirtualAlloc to reserve a
// virtual-memory range up front and then commit physical pages on demand.
// On Linux we get the same effect with mmap(PROT_NONE) + mprotect(): reserve
// the range but leave pages unbacked until first access, then mark them
// readable+writable as we grow the high water mark.
//

#include "Hunk.h"
#include "gl1_Local.h"

#if defined(_WIN32) || defined(WIN32)

#include <windows.h>

static byte* membase;

static int  hunkcount;
static uint hunkmaxsize;
static uint cursize;

void* Hunk_Begin(const int maxsize)
{
    hunkmaxsize = maxsize + sizeof(uint) + 32;
    cursize = 0;

    membase = VirtualAlloc(NULL, maxsize, MEM_RESERVE, PAGE_NOACCESS);
    if (membase == NULL)
        ri.Sys_Error(ERR_DROP, "VirtualAlloc reserve failed");

    return membase;
}

void* Hunk_Alloc(int size)
{
    size = (size + 31) & ~31;

    void* buf = VirtualAlloc(membase, cursize + size, MEM_COMMIT, PAGE_READWRITE);

    if (buf == NULL)
    {
        FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL,
            GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)&buf, 0, NULL);
        ri.Sys_Error(ERR_DROP, "VirtualAlloc commit failed.\n%s", buf);
    }

    cursize += size;

    if (cursize > hunkmaxsize)
        ri.Sys_Error(ERR_DROP, "Hunk_Alloc overflow");

    return membase + cursize - size;
}

int Hunk_End(void)
{
    hunkcount++;
    return (int)cursize;
}

void Hunk_Free(void* buf)
{
    if (buf != NULL)
        VirtualFree(buf, 0, MEM_RELEASE);

    hunkcount--;
}

#else // Linux / POSIX

#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

// We mmap each hunk as one contiguous chunk and store its total size in a
// small header at the start. Hunk_Free recovers the size from there, so we
// don't need a slot table - the engine can have as many hunks as memory
// supports (the H2 renderer can allocate hundreds during a map load).

typedef struct {
    size_t mapped_size;     // total mmap()'d size, for munmap()
    size_t cursize;         // current high-water mark (bytes committed)
} H2R_HunkHeader;

// Cacheline-align the user-visible base so the engine's allocation pattern
// (which rounds requests to 32 bytes) stays aligned.
#define H2R_HUNK_HEADER_SIZE \
    ((sizeof(H2R_HunkHeader) + 31u) & ~31u)

static H2R_HunkHeader* h2r_current = NULL;
static int             hunkcount   = 0;

void* Hunk_Begin(const int maxsize)
{
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;

    // Reserve enough address space for header + payload + slack.
    size_t total = H2R_HUNK_HEADER_SIZE + (size_t)maxsize + sizeof(uint) + 32;
    total = (total + (size_t)ps - 1) & ~((size_t)ps - 1);

    void* p = mmap(NULL, total, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        ri.Sys_Error(ERR_DROP, "Hunk_Begin: mmap reserve failed");
        return NULL;
    }

    // Commit the header page (need write access to record metadata).
    if (mprotect(p, ps, PROT_READ | PROT_WRITE) != 0) {
        munmap(p, total);
        ri.Sys_Error(ERR_DROP, "Hunk_Begin: mprotect header failed");
        return NULL;
    }

    H2R_HunkHeader* hdr = (H2R_HunkHeader*)p;
    hdr->mapped_size = total;
    hdr->cursize     = 0;
    h2r_current      = hdr;

    return (byte*)p + H2R_HUNK_HEADER_SIZE;
}

void* Hunk_Alloc(int size)
{
    if (!h2r_current) {
        ri.Sys_Error(ERR_DROP, "Hunk_Alloc called before Hunk_Begin");
        return NULL;
    }

    size = (size + 31) & ~31;

    size_t new_cursize = h2r_current->cursize + (size_t)size;
    size_t end_offset  = H2R_HUNK_HEADER_SIZE + new_cursize;

    if (end_offset > h2r_current->mapped_size) {
        ri.Sys_Error(ERR_DROP, "Hunk_Alloc overflow");
        return NULL;
    }

    // Commit pages up to the new high-water mark.
    if (mprotect(h2r_current, end_offset, PROT_READ | PROT_WRITE) != 0) {
        ri.Sys_Error(ERR_DROP, "Hunk_Alloc: mprotect commit failed");
        return NULL;
    }

    void* ret = (byte*)h2r_current + H2R_HUNK_HEADER_SIZE + h2r_current->cursize;
    h2r_current->cursize = new_cursize;
    return ret;
}

int Hunk_End(void)
{
    hunkcount++;
    int sz = h2r_current ? (int)h2r_current->cursize : 0;
    h2r_current = NULL;
    return sz;
}

void Hunk_Free(void* buf)
{
    if (buf == NULL) return;
    // Recover the header that sits just before the user-visible base.
    H2R_HunkHeader* hdr = (H2R_HunkHeader*)((byte*)buf - H2R_HUNK_HEADER_SIZE);
    size_t total = hdr->mapped_size;
    munmap(hdr, total);
    hunkcount--;
}

#endif
