// vk_lightpoint.h - BSP point lighting sample (for r_lightlevel / AI sight).
#ifndef VK_LIGHTPOINT_H
#define VK_LIGHTPOINT_H

#include "qcommon/q_Typedef.h"

// Parse the lighting-relevant lumps from a raw IBSP buffer. Copies what it
// keeps, so 'buf' may be freed afterwards.
void  VK_LightPoint_Load(const byte* buf);
void  VK_LightPoint_Free(void);

// Sample world lighting at point p; returns the dominant color component
// (0..~1+) matching ref_gl1's shadelight, for feeding r_lightlevel.
float VK_LightPoint_Sample(const float p[3]);

// Full-RGB world lighting sample at p (ref_gl1 R_LightPoint), for RF_TRANS_GHOST
// alpha. Returns false if lighting data isn't loaded.
qboolean VK_LightPoint_SampleRGB(const float p[3], float out[3]);

#endif
