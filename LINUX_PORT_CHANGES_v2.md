# Heretic2R Linux Port - v2 (64-bit data-model fixes)

This update fixes the SIGSEGV in `ResMngr_AllocateResource` that v1 hit on
real Heretic II game data (the bug crashed any actual map load).

## Root cause

H2R was written as 32-bit code. On 32-bit Windows the union `GenericUnion4_t`
is 4 bytes wide (the largest member is a 4-byte pointer or int) and the
linked-list node `SinglyLinkedListNode_t` is exactly 8 bytes (4-byte union +
4-byte next pointer).

The H2R devs hardcoded that size as `#define SLL_NODE_SIZE 8`.

On 64-bit Linux pointers are 8 bytes. `GenericUnion4_t` is now 8 bytes
(it has `void*` members), and `SinglyLinkedListNode_t` is 16 bytes. But the
resource manager was still allocating 8-byte slots based on the hardcoded
SLL_NODE_SIZE. Every list-node write past the first 8 bytes was scribbling
into the freelist's next-pointer of the adjacent slot, corrupting the
allocator's state. The crash happened in `ResMngr_AllocateResource` when
the corrupt freelist eventually dereferenced a garbage address.

## What was changed

### src/qcommon/SinglyLinkedList.h
The `SinglyLinkedListNode_s` struct definition was moved from
SinglyLinkedList.c into this header so the SLL_NODE_SIZE macro could be
defined in terms of `sizeof()` instead of a hardcoded literal.

  - SLL_NODE_SIZE is now `(sizeof(SinglyLinkedListNode_t))`. Evaluates to 8
    on 32-bit (matching MSVC behaviour) and 16 on 64-bit (correct for Linux).

### src/H2Common/src/SinglyLinkedList.c
The duplicate `typedef struct SinglyLinkedListNode_s` was removed (now in
the header).

### src/quake2/src/server/sv_world.c
The macro `STRUCT_FROM_LINK(l,t,m)` used the old Q2 pattern
`((t*)((byte*)(l) - (int)&(((t*)0)->m)))`. The cast to `int` truncates the
offset on any 64-bit struct that crosses the 2-GB boundary (won't happen in
practice) but more importantly it's undefined behaviour. Replaced with
`offsetof(t, m)`. Same fix that was already applied to FOFS/STOFS/LLOFS/
CLOFS/BYOFS in g_Local.h in v1.

## What was AUDITED and confirmed safe

  - **All other ResMngr_Con / ResMngr_AllocateResource call sites** use
    `sizeof(SomeType)` correctly; only the SLL one was hardcoded.
  - **Save game format** uses `F_EDICT` etc. (index on disk, pointer in
    memory) so saves convert pointer fields automatically; no embedded
    raw struct dumps with raw pointers.
  - **Pak/BSP/sprite file formats** use only `int` and `float` and `char[]`
    fields (which are fixed-size on any 32 or 64-bit GCC build).
  - **GenericUnion4_t** is used in-memory only - never serialised to net
    or disk - so its size becoming 8 bytes on 64-bit doesn't break anything.
  - No other `(int)&((T*)0)->member` patterns exist after fixing FOFS and
    STRUCT_FROM_LINK.

## Cross-architecture save compatibility

Saves from a 32-bit Windows H2R build will NOT load in this 64-bit Linux
build (different field offsets). Saves made in this Linux build will load
fine in later runs of this same Linux build.
