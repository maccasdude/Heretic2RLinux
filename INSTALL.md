# Heretic II R - Linux Install Guide

This is a native 64-bit Linux build of the H2R (reverse-engineered Heretic II)
source. It runs as a normal user binary - no setuid, no kernel modules, no
flatpak / snap weirdness.

## Requirements

- 64-bit Linux. Tested on Ubuntu 24.04.
- An x86_64 CPU with SSE2 (everything from ~2004 onward).
- OpenGL 1.3 capable graphics. Mesa is fine. Intel integrated, AMD, NVIDIA
  proprietary - all work.
- SDL3 (release 3.2.0 or later) for audio/input/window management.
- PulseAudio or PipeWire (PipeWire's PulseAudio shim is what most modern
  distros use by default).
- A legal copy of the original Heretic II game data (the `.pak` files from
  the CD or the GOG release).

## Installing SDL3

Most distros don't have SDL3 in their stable repos yet - it's new. If you
don't have it, build from source:

```bash
sudo apt install build-essential cmake git pkg-config \
    libwayland-dev libxkbcommon-dev libudev-dev libdrm-dev \
    libgbm-dev libxext-dev libxrandr-dev libxinerama-dev \
    libxcursor-dev libxi-dev libxss-dev libpulse-dev

git clone https://github.com/libsdl-org/SDL.git
cd SDL
git checkout release-3.2.0
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
sudo make install
sudo ldconfig
```

This puts `libSDL3.so.0` in `/usr/local/lib`. If your distro doesn't pick
that up automatically, add `/usr/local/lib` to `/etc/ld.so.conf.d/local.conf`
and re-run `sudo ldconfig`.

## Installing the game

1. Download the latest `Heretic2R-linux-binaries-vXX.tar.gz` from the
   releases.

2. Make an install folder. Anywhere is fine; `~/heretic2r` is a reasonable
   default.

   ```bash
   mkdir -p ~/heretic2r
   cd ~/heretic2r
   ```

3. Extract the binaries:

   ```bash
   tar xzf /path/to/Heretic2R-linux-binaries-vXX.tar.gz
   ```

   You'll get:
   - `heretic2r` (launcher)
   - `libquake2.so`, `libH2Common.so`, `libref_gl1.so`, `libsnd_sdl3.so`
     (engine + renderer + sound, loaded by the launcher)
   - `libgame.so`, `libplayer.so`, `libclient_effects.so` (game logic)

4. Make a `base` folder and populate it with the original Heretic II data:

   ```bash
   mkdir -p base
   ```

   Copy the following from a legitimate Heretic II install into `~/heretic2r/base/`:
   - `Htic2-0.pak` (the big one, ~200 MB)
   - `Htic2-1.pak`
   - `Default.cfg`
   - The `video/`, `sound/`, `music/`, `Art/`, `config/`, `skins/` folders
     if they're present

   The filesystem layer is case-insensitive at the file level, so it doesn't
   matter whether the source files are `htic2-0.pak` or `Htic2-0.pak`.

   The `libgame.so`, `libplayer.so`, and `libclient_effects.so` files
   should already be in `~/heretic2r/base/` from the binary extraction -
   the game looks for the game logic libs there, not in the launcher's
   directory.

5. (Optional) If you have music tracks (`Track02.ogg` etc.) install them
   in `base/music/`. The game runs fine without music.

6. Run it:

   ```bash
   ./heretic2r
   ```

## First-run setup

Once the game starts:

- The default resolution is windowed.
- **Window mode**: Options -> Video -> Window mode lets you pick:
  - Windowed (with title bar, resizable)
  - Fullscreen (exclusive)
  - Borderless (matches desktop)
- **Resolution**: Options -> Video -> Video Resolution. Pick your monitor's
  native resolution.
- **Mouse sensitivity**: Options -> Controls. Defaults are reasonable.
- **Camera smoothing**: Options -> Camera config -> Position/Rotation
  Smoothing. Defaults are good; if movement feels too "floaty", lower them.

Your settings persist in `~/.local/share/Heretic2R/base/config.cfg`.

## Save games

Saves go to `~/.local/share/Heretic2R/base/save/`. They are **not**
backward-compatible with Windows H2R saves (different struct layouts on 64
vs 32 bit).

You can backup `~/.local/share/Heretic2R/` to preserve config + saves.

## Troubleshooting

### "error while loading shared libraries: libSDL3.so.0"

SDL3 isn't where the dynamic linker expects it. Try:

```bash
sudo ldconfig /usr/local/lib
```

Or run with the path explicitly:

```bash
LD_LIBRARY_PATH=/usr/local/lib ./heretic2r
```

### Game window flashes and exits immediately

Run from a terminal with `developer 1` to see what's going wrong:

```bash
./heretic2r +set developer 1
```

Common causes:
- Missing pak files in `base/`. The game needs at least `Htic2-0.pak`.
- OpenGL context creation failed - check that you have working 3D drivers
  (`glxinfo | grep "OpenGL version"`).

### "Mana Shrine" / "ERROR: ..." stops the game

Open the console (`~` key) and look at the last few lines. Engine errors
usually have enough context to be self-explanatory; for crashes, you can
get a backtrace with `gdb`:

```bash
gdb ./heretic2r
(gdb) run
... reproduce the crash ...
(gdb) bt full
```

### Movement feels stuttery or jittery

Camera smoothing settings: Options -> Camera config. Lower position and
rotation smoothing to ~0.3 if you're seeing jitter. The defaults (0.5 and
0.9) are tuned for the original Windows build's frame timing.

### Mouse is grabbed and I can't get out

`Ctrl + G` ungrabs the mouse in development builds. If that doesn't work,
use the window manager shortcut to bring up another window (Alt+Tab, etc).

### No sound

Make sure PulseAudio or PipeWire is running:

```bash
pactl info
```

The audio driver is SDL3-based and picks up whatever the system default
is.

## Uninstalling

```bash
rm -rf ~/heretic2r
rm -rf ~/.local/share/Heretic2R
```

That's it. The build is fully self-contained and doesn't install anything
system-wide.

## Building from source

If you want to build the Linux port yourself:

```bash
# Dependencies on Ubuntu/Debian
sudo apt install build-essential cmake libsdl3-dev libgl1-mesa-dev

# Get the source
tar xzf Heretic2R-linux-source-vXX.tar.gz
cd Heretic2R-main

# Build
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# The binaries land in the build dir. Copy them to your install:
cp heretic2r libquake2.so libH2Common.so libref_gl1.so libsnd_sdl3.so \
   ~/heretic2r/
cp libgame.so libplayer.so libclient_effects.so ~/heretic2r/base/
```

See `CHANGELOG.md` for the full list of changes.
