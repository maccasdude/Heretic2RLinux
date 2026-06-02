//
// vk_engine_model.h - thin tagged wrapper distinguishing model types.
// The engine stores a model_t** pointer; the type tag lets us dispatch.
//

#ifndef VK_ENGINE_MODEL_H_INCLUDED
#define VK_ENGINE_MODEL_H_INCLUDED

#include "vk_local.h"

typedef enum {
    VK_EMODEL_NONE = 0,
    VK_EMODEL_FLEX,
    VK_EMODEL_SPRITE,
    VK_EMODEL_SUBMODEL,     // inline brush model (*1, *2, ...)
} vk_engine_model_type_t;

typedef struct {
    vk_engine_model_type_t type;
    void* impl;             // vk_model_t* / vk_sprite_t* (no ownership)
    int   submodel_index;   // for VK_EMODEL_SUBMODEL
} vk_engine_model_t;

vk_engine_model_t* VK_EModel_Register(const char* name);
void               VK_EModel_BeginRegistration(void);
void               VK_EModel_FreeAll(void);

#endif
