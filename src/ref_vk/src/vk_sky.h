//
// vk_sky.h - skybox rendering.
//

#ifndef VK_SKY_H_INCLUDED
#define VK_SKY_H_INCLUDED

#include "vk_local.h"

struct refdef_s;

void VK_Sky_Set(const char* name, float rotate, const float axis[3]);
void VK_Sky_Render(const struct refdef_s* fd, const float vieworg[3], const float* mvp);
void VK_Sky_Shutdown(void);

#endif
