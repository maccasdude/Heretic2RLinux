//
// vk_debug.h - developer debug-draw primitives (boxes/lines/arrows/markers/
// labels), Vulkan port of ref_gl1 gl1_Debug.c. Like GL, the implementation is
// compiled in all builds but only wired up (assigned to the refexport and
// invoked each frame) under _DEBUG - see vk_main.c.
//
#ifndef VK_DEBUG_H
#define VK_DEBUG_H

#include "qcommon/q_Typedef.h"

struct edict_s;
struct refdef_s;

// refexport entry points (game-facing), matching the gl1 RI_AddDebug* set.
void RI_AddDebugBox(const vec3_t center, float size, paletteRGBA_t color, float lifetime);
void RI_AddDebugBbox(const vec3_t mins, const vec3_t maxs, paletteRGBA_t color, float lifetime);
void RI_AddDebugEntityBbox(const struct edict_s* ent, paletteRGBA_t color);
void RI_AddDebugLabel(const vec3_t origin, paletteRGBA_t color, float lifetime, const char* label);
void RI_AddDebugEntityLabel(const struct edict_s* ent, paletteRGBA_t color, const char* label);
void RI_AddDebugLine(const vec3_t start, const vec3_t end, paletteRGBA_t color, float lifetime);
void RI_AddDebugArrow(const vec3_t start, const vec3_t end, paletteRGBA_t color, float lifetime);
void RI_AddDebugDirection(const vec3_t start, const vec3_t direction, float size, paletteRGBA_t color, float lifetime);
void RI_AddDebugAngles(const vec3_t start, const vec3_t angles, float size, paletteRGBA_t color, float lifetime);
void RI_AddDebugAnglesRad(const vec3_t start, const vec3_t angles, float size, paletteRGBA_t color, float lifetime);
void RI_AddDebugMarker(const vec3_t center, float size, paletteRGBA_t color, float lifetime);

// Per-frame: set the current refdef time (used for primitive lifetimes), draw
// the accumulated 3D primitives (depth test off), and draw the 2D labels. mvp
// is the world view-projection (float[16], column-major) built in R_RenderFrame.
void VK_Debug_SetTime(float time);
void VK_Debug_DrawPrimitives(const struct refdef_s* fd, const float mvp[16]);
void VK_Debug_DrawLabels(const struct refdef_s* fd, const float mvp[16]);

// Clear all primitives/labels (ref_gl1 R_FreeDebugPrimitives, called on map load).
void VK_Debug_Free(void);
// Tear down the Vulkan pipeline + line buffers (renderer shutdown).
void VK_Debug_Shutdown(void);

#endif
