//
// vk_particles.h - particle rendering (diamond-quad billboards).
//

#ifndef VK_PARTICLES_H_INCLUDED
#define VK_PARTICLES_H_INCLUDED

#include "vk_local.h"

struct refdef_s;

qboolean VK_Particles_Init(void);
void     VK_Particles_Shutdown(void);

// Reset per-frame VBO at the top of every frame.
void     VK_Particles_BeginFrame(void);

// Render normal-blend particles (translucent) and alpha-add particles
// (additive, glow). Pass camera up/right and the cached view-projection.
void     VK_Particles_Render(const struct refdef_s* fd,
                             const float vup[3], const float vright[3],
                             const float* mvp);

#endif
