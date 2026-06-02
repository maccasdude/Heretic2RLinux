//
// vk_world.h - BSP loading + world rendering.
//
// Session 3a: draws all world surfaces with no culling, no textures,
// no lightmaps. Just to see geometry on screen.
//

#ifndef VK_WORLD_H_INCLUDED
#define VK_WORLD_H_INCLUDED

#include "vk_local.h"
#include "vk_buffer.h"

// Lifecycle - called from R_BeginRegistration / R_EndRegistration.
// Returns true on success; on failure World stays in unloaded state.
qboolean VK_World_LoadMap(const char* name);
void     VK_World_Free(void);

// Per-frame rendering. The refdef gives us camera origin/angles, FOV, viewport.
void     VK_World_Render(const refdef_t* fd);
void     VK_World_FillSkyFog(float* pc /* >=32 floats */);  // sky fog tail (pc[20..31])
void     VK_World_FillEntityFog(float* pc /* >=32 floats */); // sprite/decal fog tail
void     VK_World_UpdateLightstyles(const refdef_t* fd);
// Translucent/warp world surfaces (water, lava, glass). Call AFTER all opaque
// geometry (world + entities + submodels), before particles.
// Draws translucent world surfaces (water/glass/forcefields) interleaved with
// the translucent ENTITY billboards/sprites by depth, back-to-front, matching
// ref_gl1 R_SortAndDrawAlphaSurfaces (which merges alpha entities + alpha
// surfaces into one depth-sorted list). The caller passes the already-back-to-
// front-sorted alpha entity array and a callback that draws one entity; this
// function calls it at the correct depth boundaries so e.g. a crosshair or
// splash in front of opaque swamp water is drawn AFTER (on top of) the water.
typedef void (*VK_DrawAlphaEntityFn)(struct entity_s* e, void* user);
void     VK_World_RenderWater(const refdef_t* fd,
                              struct entity_s** alpha_ents, int num_alpha_ents,
                              VK_DrawAlphaEntityFn draw_entity, void* user);

// Render inline submodel 'index' (1-based: *1, *2, ...) with the given MVP and
// a world-space translation (the entity origin, so doors/lifts can move).
// Returns false if the index is out of range.
qboolean VK_World_RenderSubmodel(int index, const float* mvp,
                                 const float origin[3], const float angles[3],
                                 int ent_frame);

// Number of inline submodels (excludes the world itself). *N is valid for
// 1 <= N <= count.
int      VK_World_NumSubmodels(void);

// True if a valid world is currently loaded.
qboolean VK_World_IsLoaded(void);

#endif // VK_WORLD_H_INCLUDED
