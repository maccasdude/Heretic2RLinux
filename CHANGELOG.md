# Heretic II R - Linux Port Changelog

A port of the H2R (Heretic II reverse-engineered) Windows source to native
64-bit Linux. Starting from a Windows/MSVC codebase that built a `.exe` plus
a set of `.dll` files, the port produces a Linux launcher binary and a set
of matching `.so` shared libraries that load via `dlopen`.

Listed in roughly the order changes were made.

## v1 - Platform layer and build system

- New `src/launcher/src/main_linux.c` wrapping `Quake2Main_Linux`.
- New `src/quake2/src/linux/sys_linux.c` mirroring the Windows main loop
  (busywait < 5us, `Sys_Microseconds` via `CLOCK_MONOTONIC`).
- New `src/quake2/src/linux/q_shlinux.c` providing POSIX file system
  helpers and the case-insensitive path resolver `H2R_ResolvePathCI`.
- New `src/quake2/src/linux/net_linux.c` for BSD-socket UDP networking
  (IPX is stubbed; LAN/internet UDP works).
- New `include/compat/win_compat.h` mapping Win32 typedefs, `_s` function
  shims, C++ template overloads, and a `LoadLibrary` -> `dlopen` shim that
  understands both `gamex86.dll` and `libgame.so` style names.
- CMake build system producing `heretic2r` (launcher), `libquake2.so`
  (engine), `libH2Common.so`, `libgame.so`, `libplayer.so`,
  `libclient_effects.so`, `libref_gl1.so` (OpenGL 1.3 renderer), and
  `libsnd_sdl3.so`. All `.so` files share `$ORIGIN` rpath so the launcher
  picks them up next to itself.

## v2 - Source portability fixes

- Case-insensitive `#include` mismatches (32 mismatches across 66 files)
  corrected to match the on-disk filenames.
- MSVC-specific keywords/builtins (`_inline`, `_findfirst`, `_s` variant
  functions) gated behind the new compat header.
- `#pragma region`/`endregion` stripped from 131 files (unknown to GCC,
  noisy with `-Wall`).
- File-scope forward declarations added in `q_Typedef.h` for several
  structs to satisfy C99 6.7.2.3p6 block-scoping rules that MSVC did not
  enforce.
- `__declspec(dllexport)` translated to `__attribute__((visibility(...)))`.
  H2Common built with `-fvisibility=default` to keep its symbols visible
  across .so boundaries.

## v3 - 32-bit assumptions and STL_NODE size

- `SLL_NODE_SIZE` was hardcoded to `8` (32-bit struct size). Replaced
  with `sizeof(SinglyLinkedListNode_t)` so it adapts to the 64-bit layout.
  Struct moved to the header to make `sizeof` visible at the call sites.
- `STRUCT_FROM_LINK` in `sv_world.c` replaced with `offsetof()` based
  pointer arithmetic that's correct on either pointer width.

## v4 - Case-insensitive filesystem layer

- `Sys_FindFirst` now uses `FNM_CASEFOLD` for the glob match.
- New helper `FS_TryOpenCaseInsensitive` walks each path component via
  `opendir`/`readdir` to find a case-insensitive match when the exact
  path misses on the case-sensitive Linux filesystem.
- libsmacker (used for `.smk` cinematic playback) was reading files via
  raw `fopen`; routed through the same case-insensitive resolver so it
  picks up `Bumper.smk` vs `bumper.smk` style mismatches.
- Default config file load corrected to handle both `default.cfg` and
  `Default.cfg`.

## v5 - va_arg ABI fix for variadic message functions

- `ParseEffectToSizeBuf` (netmsg_write.c) and `MSG_SetParms` (Message.c)
  used `va_arg(marker, byte)` and `va_arg(marker, short)`. Per the C
  standard (7.16.1.1) these types are promoted to `int` by default
  argument promotion, so reading them as `byte`/`short` is undefined
  behaviour. On 32-bit x86 the resulting 4-byte stack slot read happened
  to work; on 64-bit System V Linux the va_list register save area is
  structured by type and the wrong type misaligns subsequent reads.
- Fixed to read `int` and cast down. Audited every other `va_arg` in the
  codebase to confirm no other instances of the pattern.

## v6 - Hunk allocator: size-in-header instead of slot table

- The original `Hunk_*` (used by the renderer for model/texture chunks)
  is backed by `VirtualAlloc`/`VirtualFree` on Windows. `VirtualFree`
  takes only a pointer; the kernel knows the size. `munmap` on Linux
  needs the size too.
- Initial port stashed sizes in a 32-slot fixed array, which was enough
  for engine startup but the renderer allocates many more hunks during
  a map load.
- Rewrote to store an aligned header at the start of each mapping
  containing the total size; `Hunk_Free` recovers the size from the
  header. No slot table, unlimited hunks.

## v7 - CL_DeltaEntity NULL guard for missing base player model

- When the player-specific model failed to load (`cl.clientinfo[N].model
  == NULL`), the code unconditionally dereferenced `cl.baseclientinfo.model`.
  If THAT also failed (missing `players/male/...` in the install), the
  game crashed.
- The H2R authors had added a NULL check at one similar code path but
  missed this one. Mirrored their fix: if both are NULL, set model to
  NULL and skip entity rendering for that frame.

## v8 - Three-mode fullscreen and SDL3 window flags

- The H2R SDL3 code never wired up real fullscreen. The window-creation
  code took the renderer's flags (just `SDL_WINDOW_OPENGL`) and relied on
  the WM happening to render as borderless when the requested resolution
  matched the desktop.
- Registered new cvar `vid_fullscreen` (archived to config). Three modes:
  - `0` = windowed (resizable, with title bar)
  - `1` = exclusive fullscreen (`SDL_WINDOW_FULLSCREEN`, may change display
    mode)
  - `2` = borderless fullscreen (matches desktop, no decoration)
- Modifying `vid_fullscreen` auto-triggers `vid_restart` so changes take
  effect immediately.

## v9-v12 - Window mode menu item

- Added a "Window mode" spincontrol to Options -> Video.
- Iterated on placement and layout. The H2R menu code uses two-line spin
  controls (label + value below) with 40px vertical pitch. Inserting a
  second two-line item next to the existing two-line items caused
  big-font ascenders to overlap (clipping `F` and `B` to look like `/`
  and `+`).
- Final form: single-line layout (like Target FPS) reading
  `Window mode : Fullscreen`, which keeps the original 40px rhythm and
  has no glyph collisions.

## v13 - Hide empty quicksave slot in Load Game

- The Load Game menu always reserved the first row for the quicksave
  slot, even when no quicksave existed yet. That left a non-selectable
  blank `<*****>` row at the top of the list.
- The row is now hidden when the quicksave is empty; it reappears once a
  quicksave exists.

## v14 - Save-load: stale GroundSurface pointer (initial fix)

- Loading a mid-level save crashed in `ClientEndServerFrame` at offset
  0x1b28 of `gclient_t` (= `playerinfo.GroundSurface`). The pointer value
  was `0x73b700000000` - a stale 8-byte pointer with the low 4 bytes zero.
- Diagnostic build confirmed the field is `playerinfo.GroundSurface`
  (offset 472 inside `playerinfo`, which itself is at offset 6472 in
  `gclient_t`).
- Initial fix: explicit `cl->client->playerinfo.GroundSurface = NULL` in
  the save-load loop. Made loading possible; superseded by v15.

## v15 - Save-load: F_CLEAR pointer corruption (proper fix)

- The H2R authors had already identified `playerinfo.GroundSurface` as a
  field that must not be restored from save - they marked it `F_CLEAR`
  in `clientfields`. But their handler wrote
  ```c
  *(int*)p = 0;
  ```
  which on 64-bit only zeros the low 4 bytes of an 8-byte pointer field.
  The high 4 bytes retained whatever the saved struct dump contained,
  yielding garbage pointers like `0x73b700000000`.
- Same class of bug as v5: a 32-bit-pointer-size assumption that broke
  silently on x86_64 because `sizeof(int) != sizeof(void*)`.
- Fixed F_CLEAR handler to write a full pointer-sized NULL:
  ```c
  *(void**)p = NULL;
  ```
- This also explained "stutter on new games": the engine writes an
  autosave during map transitions, then reloads it. Every map change ran
  the broken F_CLEAR path, leaving the player with a garbage
  GroundSurface that produced physics instability.
- The v14 workaround was removed since v15 fixes the root cause.

## v16 - Prediction lerp clamp

- The client-side movement prediction (`CL_PredictMovement`) lerps
  between packet frames using `origin_lerp` that accumulates by
  `1/((vid_maxfps/cl_maxfps) + 1)` per render frame, expected to hit 1.0
  exactly when the next packet frame arrives.
- The math assumes a deterministic ratio of render frames to packet
  frames. On Linux with compositor-managed timing it occasionally fits
  one extra render frame between packet frames, sending `origin_lerp`
  past 1.0 - the predicted origin overshoots and then snaps back on the
  next packet.
- Clamped `origin_lerp` and `pred_playerLerp` to `[0, 1]`. Real fix for
  a real overshoot bug, even though it wasn't the source of the visible
  stutter (v18 was).

## v17 - Build with -O2 optimization

- CMake build had no `CMAKE_BUILD_TYPE` set and no explicit `-O` flag,
  so GCC was building everything at `-O0`. Added `-O2` to the global
  compile options for substantially faster physics, rendering, and
  audio mixing code paths.

## v18 - Framerate-independent camera smoothing

- The visible "vibration while moving" was traced to the camera-smoothing
  logic, not to the player physics. Disabling camera position/rotation
  smoothing eliminated the jitter; turning it up made it worse.
- Both smoothing paths (`CL_InterpolateCameraOrigin` for position,
  `CL_UpdateCameraOrientation` for rotation) applied a fixed retention
  fraction per render call. Works well when render timing is regular
  (Windows VBLANK); on Linux render frames jitter by 1-2ms which makes
  the effective decay rate fluctuate visibly.
- Reworked the smoothing as exponential decay scaled by actual frame
  time:
  ```c
  retention = (1 - smoothing)^(frametime / nominal_frametime)
  ```
  At nominal-rate ticks the result matches the original. At irregular
  ticks the effective time-domain smoothing is constant.

## Files added or substantially rewritten

- `src/launcher/src/main_linux.c`
- `src/quake2/src/linux/sys_linux.c`
- `src/quake2/src/linux/q_shlinux.c`
- `src/quake2/src/linux/net_linux.c`
- `include/compat/win_compat.h`
- `src/ref_gl1/src/Hunk.c` (Linux implementation)
- `CMakeLists.txt`

## Files modified for porting and bug fixes

- `src/qcommon/netmsg_write.c` (va_arg fix)
- `src/qcommon/Message.c` (va_arg fix)
- `src/quake2/src/cs_shared/files.c` (case-insensitive FS layer)
- `src/quake2/src/client/cl_smk.c` (libsmacker case-insensitive routing)
- `src/quake2/src/client/glimp_sdl3.c` (three-mode fullscreen)
- `src/quake2/src/win32/dll_io/vid_dll.c` (vid_fullscreen cvar, restart
  on change)
- `src/quake2/src/client/menus/menu_video.c` (Window Mode menu item)
- `src/quake2/src/client/menus/menu_loadgame.c` (hide empty quicksave)
- `src/quake2/src/client/cl_entities.c` (CL_DeltaEntity NULL guard)
- `src/quake2/src/client/cl_prediction.c` (lerp clamps)
- `src/quake2/src/client/cl_camera.c` (framerate-independent smoothing)
- `src/game/src/Saving/g_Save.c` (F_CLEAR 64-bit fix)
- Plus ~130 files with `#pragma region` stripped and case-insensitive
  include corrections.

## Known runtime issues (not blockers)

- A few cosmetic R_LoadM8 errors for decoration textures from optional
  "update files" content; appears to be a content issue with the
  mod/update pack, not the engine.
- One or two `OGG_PlayTrack` errors for specific music tracks that look
  malformed in the install (`Track02.ogg`, `Track14.ogg`).
- `S_LoadSound: skipping 'weapons/FireballPowerImpact.wav'` - missing
  RIFF/WAVE chunks in that single file.
- Multiplayer is untested. UDP networking is in; IPX is stubbed (no one
  uses it on modern systems).

## Things that were NOT changed

- The game-content paks (`Htic2-0.pak`, `Htic2-1.pak`) are used as-is.
- The save-game format is **NOT** backward-compatible with Windows H2R
  saves. Saves made by the Linux build are only loadable by the Linux
  build.
- Original H2R behaviour is preserved everywhere; the changes here are
  either pure portability or genuine bug fixes for issues that affected
  the Windows build too but happened not to manifest.
