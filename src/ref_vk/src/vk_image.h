//
// vk_image.h - Texture API.
//

#ifndef VK_IMAGE_H
#define VK_IMAGE_H

#include "vk_local.h"
#include "vk_buffer.h"

// Engine sees this as opaque struct image_s*.
typedef struct image_s image_t;

qboolean VK_InitImages(void);
void     VK_ShutdownImages(void);

// Used by Draw_Fill / Draw_Char which want a solid colour quad.
image_t* VK_GetWhiteTexture(void);

// Find or load a pic by its short name (e.g. "menu/banner_video"). Adds the
// "pics/" prefix automatically unless the name begins with '/' or '\'.
image_t* VK_FindPic(const char* name);

// Direct path lookup (used internally, no prefix munging).
image_t* VK_FindImage(const char* name);

// Accessors so other modules don't need to know the struct layout.
int             VK_ImageWidth(const image_t* img);
int             VK_ImageHeight(const image_t* img);
VkDescriptorSet VK_ImageDescriptor(const image_t* img);
qboolean VK_ImageHasAlpha(const image_t* img);
VkImageView     VK_ImageView(const image_t* img);

#endif
