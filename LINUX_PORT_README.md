# Heretic2R - Linux Port

This is a port of Heretic2R (the Raven Software Heretic II reverse-engineered
source) from Windows/MSVC to Linux/GCC.

## What's included

- **A working Linux build of all seven targets**: the `heretic2r` launcher
  executable plus six shared libraries (`libquake2`, `libH2Common`,
  `libgame`, `libplayer`, `libclient_effects`, `libref_gl1`, `libsnd_sdl3`).
- **A Linux platform layer** at `src/quake2/src/linux/`:
  - `sys_linux.c` -- Sys_Error/Sys_Quit, Sys_LoadGameDll (dlopen), console I/O,
    the main loop entry point Quake2Main_Linux.
  - `q_shlinux.c` -- POSIX filesystem helpers, clock_gettime timing, XDG
    user-dir resolution.
  - `net_linux.c` -- BSD-sockets UDP backend; IPX path stubbed out.
- **A cross-platform compatibility header** at `include/compat/win_compat.h`:
  - Win32 typedefs (HINSTANCE, DWORD, BOOL, ...).
  - MSVC "safe" function stand-ins (sprintf_s, strcpy_s, strncpy_s, strcat_s,
    memcpy_s, memmove_s, fopen_s, sscanf_s, localtime_s, fread_s, strtok_s).
  - C++ template overloads for the 2-arg array forms (strcpy_s(arr, src) etc).
  - POSIX-ish aliases (_strdup, _stricmp, _strnicmp, _strlwr, _strupr,
    _findfirst/_findnext/_findclose, _mkdir, _chdir, _getcwd).
  - DLL loading (LoadLibrary -> dlopen, GetProcAddress -> dlsym,
    FreeLibrary -> dlclose).
- **CMake build system** (`CMakeLists.txt`) that builds everything with rpath
  set to `$ORIGIN` so the launcher finds its sibling .so files at runtime.

## How to build

```sh
sudo apt install build-essential cmake libgl1-mesa-dev libglx-dev
# SDL3 isn't in stock distro repos yet (as of mid 2026) - build from source:
git clone --depth=1 --branch release-3.2.0 https://github.com/libsdl-org/SDL.git sdl3
cd sdl3 && cmake -B build -DCMAKE_BUILD_TYPE=Release && \
  cmake --build build && sudo cmake --install build

# Build H2R
cd /path/to/Heretic2R-main
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Run (will need actual Heretic II game data in a sibling ./base/ folder)
cd build
./heretic2r
```

## How DLL loading was mapped

H2R's Sys_LoadGameDll receives names like `"gamex86"`, `"Client Effects"`,
`"ref_gl1.dll"`, `"snd_sdl3.dll"`. On Linux these are mapped to:

  gamex86           -> libgame.so
  Client Effects    -> libclient_effects.so
  Player            -> libplayer.so
  ref_<id>.dll      -> libref_<id>.so   (so ref_gl1.dll -> libref_gl1.so)
  snd_<id>.dll      -> libsnd_<id>.so   (so snd_sdl3.dll -> libsnd_sdl3.so)

The mapping happens in sys_linux.c's H2R_NameToSoPath helper.

## What I had to patch in the upstream source

The MSVC->GCC porting touched these source files (changes are marked with
"Linux port" comments or are wrapped in `#if defined(_WIN32) || defined(WIN32)`
blocks):

  - src/qcommon/H2Common.h         -- declspec wrappers, max/min, limits.h
  - src/qcommon/Heretic2.h         -- Q2DLL_DECLSPEC wrapper
  - src/qcommon/q_Typedef.h        -- _inline define, file-scope struct fwd
                                      declarations (fixes C99 6.7.2.3p6
                                      parameter-list scoping vs MSVC)
  - src/qcommon/p_dll.c            -- conditional windows.h include
  - src/H2Common/src/DllMain.c     -- GCC __attribute__((constructor))
                                      replaces DllMain for .so lifecycle
  - src/H2Common/src/ResourceManager.c -- conditional windows.h include
  - src/H2Common/src/SurfaceProps.c -- void* prototype fix
  - src/game/src/q_Shared.h        -- H2R_NORETURN cross-platform, conditional
                                      direct.h, win_compat.h pull-in
  - src/game/src/Player.h          -- platform-aware PLAYER_API, extern fix
                                      for function pointer declarations
  - src/game/src/g_Local.h         -- FOFS/STOFS/LLOFS/CLOFS/BYOFS now use
                                      offsetof (was UB pointer cast)
  - src/game/src/g_Edict.h         -- classID changed from `enum ClassID_e`
                                      back to `int` (avoids incomplete-type
                                      error from circular include)
  - src/game/src/g_Message.h       -- file-scope fwd decl of G_Message_s
  - src/game/src/g_Main.c          -- platform-aware GAME_DECLSPEC
  - src/game/src/GameObjects/g_Obj.h -- include g_Message.h for G_Message_t
  - src/client effects/src/ce_Main.c -- platform-aware CLFX_DECLSPEC
  - src/client effects/src/ce_Message.h -- file-scope fwd decl of
                                            client_entity_s
  - src/client effects/src/ce_DefaultMessageHandler.c -- signature harmonized
  - src/ref_gl1/src/Hunk.c         -- mmap/mprotect replaces VirtualAlloc on
                                      Linux (kept Windows path intact)
  - src/ref_gl1/src/gl1_Main.c     -- platform-aware REF_DECLSPEC
  - src/ref_gl1/src/gl1_FlexModel.c -- one variable renamed from non-ASCII
                                       to ASCII (GCC treats source as UTF-8)
  - src/snd_sdl3/src/snd_main.c    -- platform-aware SNDLIB_DECLSPEC
  - src/quake2/src/win32/Quake2Main.h     -- adds Quake2Main_Linux declaration
  - src/quake2/src/win32/dll_io/dll_io.h  -- conditional includes
  - src/quake2/src/win32/dll_io/clfx_dll.h-- conditional windows.h
  - src/quake2/src/win32/dll_io/snd_dll.c -- conditional windows.h
  - src/quake2/src/win32/dll_io/vid_dll.c -- conditional windows.h,
                                             SDL_ShowSimpleMessageBox for
                                             PRINT_ALERT on Linux
  - src/quake2/src/client/client.h        -- include "console.h" (lowercase)
                                             instead of "Console.h" so we
                                             get the local console with the
                                             `con` variable, not H2Common's
  - src/quake2/src/client/cl_input.c      -- pred_pm_flags now extern
                                             (was duplicate definition)
  - src/quake2/src/cs_shared/Debug.c      -- OutputDebugString -> stderr
                                             on Linux

131 source files had `#pragma region` / `#pragma endregion` comments stripped
(MSVC IDE folding hints that GCC chokes on mid-statement).

~66 source files had #include statements case-corrected for case-sensitive
Linux filesystems.

## What still needs work

This port gets the code to **compile, link, and run**, but it is NOT
play-tested. Likely items needing attention before it's actually playable:

1. **Path separator handling.** The engine uses `/` consistently but H2R may
   serialize Windows-style paths to save files; check FS_BuildPath and the
   savegame paths.

2. **Save game compatibility.** Saves include raw struct dumps which are
   architecture-sensitive. Windows H2R is 32-bit; this Linux build is
   64-bit by default. Save files will not be cross-compatible. Pointer
   sizes in saved structs may also cause crashes when loading.

3. **Skeletal animation.** No code changes were made here; the existing
   logic should work since it's all data-driven, but worth verifying.

4. **Input.** SDL3 input was the H2R team's already-cross-platform code path
   so this should work, but gamepad mapping has not been tested on Linux.

5. **Audio.** The SDL3 audio module is also cross-platform from the H2R
   side. Should work.

6. **The IPX network code path** is stubbed; multiplayer over IPX won't
   work, but UDP/IP does. (IPX is unusable on modern Linux anyway.)

7. **The CD audio / changeCDtrack functions.** No physical CD on modern
   systems. Mostly stubbed in the engine already (the H2 source notes this).
