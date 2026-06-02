//
// vk_model.h - H2 flex model (.fm) loading + rendering.
//
// Session 3d: non-skeletal frame-animated models (monsters, items, props).
// Skeletal models (player) come later.
//

#ifndef VK_MODEL_H_INCLUDED
#define VK_MODEL_H_INCLUDED

#include "vk_local.h"

// Opaque handle returned by VK_Model_Register. The engine stores it as a
// struct model_s*.
typedef struct vk_model_s vk_model_t;

// Load (or fetch cached) a flex model by path. Returns NULL on failure.
vk_model_t* VK_Model_Register(const char* name);

// Free all loaded models (called at renderer shutdown / map change).
void        VK_Model_FreeAll(void);

// Reference type for this model (REF_CORVUS etc.), or -1 if it has none.
// Used by re.GetReferencedID so the client allocates entity->referenceInfo.
int         VK_Model_ReferenceType(const vk_model_t* m);

// Draw one entity's model. Called from R_RenderFrame's entity loop.
// 'e' is the engine's entity_t (origin, angles, frame, oldframe, backlerp...).
struct entity_s;
void        VK_Model_DrawEntity(const struct entity_s* e, const float* mvp_view_proj);

#endif
