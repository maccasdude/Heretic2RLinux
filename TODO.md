# Vulkan Renderer (ref_vk) - Outstanding Work

Status: at full visual + performance parity with ref_gl1 for everything audited
(textured + lightmapped world, flex/skeletal models, sprites, sky, particles,
reflections, ghost alpha, animated lightstyles, fog, dynamic lights incl. on
inline submodels, PVS + frustum culling, alpha sorting, gamma/brightness/
contrast, gl_minlight). The full per-version development history lives in
CHANGELOG.md.

Build/run: Ubuntu 24.04, Intel Iris Xe / Mesa / Wayland, Vulkan validation on.
Configure + build from Heretic2R-main/build (cmake + make -j4); SDL3 in
/usr/local. Run: `SDL_VIDEODRIVER=wayland ./heretic2r +set vid_ref vk`.

## TODO

(none outstanding - all audited features at parity; see non-gaps below)

## Documented non-gaps (do NOT re-flag as bugs)

- R_FindSurface is a stub returning 0. The engine marks the import //TODO unused
  (client.h) and never calls it, so GL's BSP-walk version is dead code too.
- Cosmetic: implicit-declaration warning for VectorCopy at vk_world.c:628.

Note: developer debug-draw (boxes/lines/arrows/markers/labels) IS now implemented
(vk_debug.c), wired up only under _DEBUG exactly like ref_gl1 - dormant in retail
NDEBUG builds. The game call sites (cs_shared/Debug.c) are also _DEBUG-only and use
MSVC secure-CRT, so this path is exercised on a _DEBUG build (e.g. a Windows
backport), not the Linux retail build.

## Invariants worth preserving (regression traps)

- Eviction safety: a cached VkDescriptorSet built from an image view is safe only
  if it is image-owned (freed with the image) OR rebuilt per registration OR
  sourced from a permanent image. Any reuse-without-reload path (sky, world
  same-map reload) MUST re-stamp its images (VK_Image_Touch) so eviction spares
  them. Live re-bakes (gamma, gl_minlight) update images/atlas in place via
  VK_UpdateTextureRGBA / region upload, keeping views + descriptors stable.

Portability: push-constant usage is now within the 128-byte guaranteed minimum
(world 64, warp 96, entity 128), so the renderer initializes on any conformant
Vulkan device, not just ones reporting 256. (Resolved v118.)

## Known limitations (non-blocking, carried from early design)

- Memory: one VkDeviceMemory allocation per resource (no sub-allocator). Fine at
  H2's asset scale; a suballocator would reduce allocation count if ever needed.
- Descriptor pools are fixed size (4096 2D / 1024 world). Comfortably sufficient
  for current content; would need raising only for far larger maps.
(The old "world draws every surface every frame" shortcut no longer applies -
PVS + frustum culling landed in v71/v73.)
