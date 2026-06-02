//
// vk_draw.h - 2D draw API.
//

#ifndef VK_DRAW_H
#define VK_DRAW_H

#include "vk_local.h"

qboolean VK_DrawInit(void);
void     VK_DrawShutdown(void);

void     VK_Draw_BeginFrame(void);   // Reset per-frame VBO, bind pipeline
void     VK_Draw_EndFrame(void);     // Flush pending batch

void VK_Draw_GetPicSize  (int* w, int* h, const char* name);
void VK_Draw_Pic         (int x, int y, int scale, const char* name, float alpha);
void VK_Draw_StretchPic  (int x, int y, int w, int h, const char* name, float alpha, DrawStretchPicScaleMode_t mode);
void VK_Draw_TileClear   (int x, int y, int w, int h, const char* name);
void VK_Draw_Fill        (int x, int y, int w, int h, paletteRGBA_t color);
void VK_Draw_FadeScreen  (paletteRGBA_t color);
void VK_Draw_Char        (int x, int y, int scale, int c, paletteRGBA_t color, qboolean shadow);
void VK_Draw_BigFont     (int x, int y, const char* text, float alpha);
int  VK_BF_Strlen        (const char* text);
void VK_BookDrawPic      (const char* name, float scale, float alpha);

#endif
