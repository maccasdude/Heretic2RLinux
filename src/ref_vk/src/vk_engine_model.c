//
// vk_engine_model.c - dispatch table between flex models and sprites.
//

#include "vk_engine_model.h"
#include "vk_model.h"
#include "vk_sprite.h"
#include "vk_world.h"

#include <string.h>
#include <stdlib.h>

#define VK_MAX_ENGINE_MODELS 1024

static vk_engine_model_t s_emodels[VK_MAX_ENGINE_MODELS];
static int               s_num_emodels = 0;

vk_engine_model_t* VK_EModel_Register(const char* name)
{
    if (!name || !*name) return NULL;

    // Inline brush submodels: "*1", "*2", ... These index into the world's
    // submodel table (doors, lifts, breakables).
    if (name[0] == '*') {
        const int idx = atoi(name + 1);
        if (idx >= 1 && idx <= VK_World_NumSubmodels()) {
            if (s_num_emodels >= VK_MAX_ENGINE_MODELS) return NULL;
            vk_engine_model_t* w = &s_emodels[s_num_emodels++];
            w->type = VK_EMODEL_SUBMODEL;
            w->impl = NULL;
            w->submodel_index = idx;
            return w;
        }
        return NULL;
    }

    // Try flex model first (the "header" magic check is cheap and rejects
    // sprites). If that returns non-NULL we wrap and return.
    vk_model_t* fm = VK_Model_Register(name);
    if (fm) {
        if (s_num_emodels >= VK_MAX_ENGINE_MODELS) return NULL;
        vk_engine_model_t* w = &s_emodels[s_num_emodels++];
        w->type = VK_EMODEL_FLEX;
        w->impl = fm;
        w->submodel_index = 0;
        return w;
    }

    // Otherwise try as a sprite (.sp2 / IDS2 magic).
    vk_sprite_t* sp = VK_Sprite_TryLoad(name);
    if (sp) {
        if (s_num_emodels >= VK_MAX_ENGINE_MODELS) return NULL;
        vk_engine_model_t* w = &s_emodels[s_num_emodels++];
        w->type = VK_EMODEL_SPRITE;
        w->impl = sp;
        w->submodel_index = 0;
        return w;
    }

    return NULL;
}

void VK_EModel_FreeAll(void)
{
    memset(s_emodels, 0, sizeof(s_emodels));
    s_num_emodels = 0;
}

// Reset the engine-model wrapper list at the start of a map registration. The
// wrappers are lightweight handles into the (name-deduped) flex/sprite caches
// and the world submodel table; they MUST be rebuilt each map because submodel
// indices point into the freshly loaded world. Without this reset the list grew
// by the full model count on every map load and eventually overflowed
// VK_MAX_ENGINE_MODELS, after which VK_EModel_Register returned NULL and no
// models rendered (seen as "no models on a second load").
void VK_EModel_BeginRegistration(void)
{
    memset(s_emodels, 0, sizeof(s_emodels));
    s_num_emodels = 0;
}
