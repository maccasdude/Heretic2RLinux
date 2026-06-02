//
// vk_sprite.h - billboard sprite (.sp2) loading + rendering.
//

#ifndef VK_SPRITE_H_INCLUDED
#define VK_SPRITE_H_INCLUDED

#include "vk_local.h"

typedef struct vk_sprite_s vk_sprite_t;

// Tries to load 'name' as an .sp2. Returns NULL if it's not a sprite (e.g.
// the file starts with "header" so it's a flex model).
vk_sprite_t* VK_Sprite_TryLoad(const char* name);

// Draw one sprite entity at e->origin/e->frame, billboarded toward the
// camera. Pass the view's up and right vectors (already computed by the
// renderer) and the cached view-projection matrix.
struct entity_s;
void VK_Sprite_DrawEntity(const struct entity_s* e, const vk_sprite_t* s,
                          const float vup[3], const float vright[3],
                          const float vfwd[3], const float* mvp);

void VK_Sprite_FreeAll(void);

#endif
