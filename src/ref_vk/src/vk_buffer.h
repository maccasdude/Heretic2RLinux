//
// vk_buffer.h - Buffer and image memory helpers.
//

#ifndef VK_BUFFER_H
#define VK_BUFFER_H

#include "vk_local.h"

typedef struct {
    VkBuffer       buffer;
    VkDeviceMemory memory;
    VkDeviceSize   size;
    void*          mapped;     // NULL unless allocated host-visible and mapped
} vk_buffer_t;

typedef struct {
    VkImage        image;
    VkImageView    view;
    VkDeviceMemory memory;
    uint32_t       width;
    uint32_t       height;
    VkFormat       format;
} vk_texture_t;

uint32_t VK_FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags wanted);

qboolean VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags props, vk_buffer_t* out);
void     VK_DestroyBuffer(vk_buffer_t* buf);
qboolean VK_MapBuffer(vk_buffer_t* buf);

// One-shot command buffer for staging copies/layout transitions.
VkCommandBuffer VK_BeginOneShotCmd(void);
void            VK_EndOneShotCmd(VkCommandBuffer cb);

qboolean VK_CreateTextureRGBA(uint32_t w, uint32_t h, const void* rgba_pixels,
                              vk_texture_t* out);
void     VK_DestroyTexture(vk_texture_t* tex);
qboolean VK_UpdateTextureRGBA(vk_texture_t* tex, uint32_t x, uint32_t y,
                              uint32_t region_w, uint32_t region_h,
                              const void* src, uint32_t src_stride);

// One rectangular region to upload into a texture from a CPU source buffer.
typedef struct {
    uint32_t x, y, w, h;     // destination rect in the texture (texels)
    uint32_t src_x, src_y;   // top-left of this region within the source image
} vk_tex_region_t;

// Batch-upload several rectangular regions from one tightly-packed-by-row CPU
// source image (src, src_w texels wide, RGBA) into an existing texture, using a
// single staging buffer + one command submit. For per-frame animated-lightstyle
// atlas updates. Returns false on allocation failure.
qboolean VK_UpdateTextureRegions(vk_texture_t* tex, const void* src, uint32_t src_w,
                                 const vk_tex_region_t* regions, int num_regions);

// Streaming texture for cinematics. Created with linear tiling + host-visible
// memory so we can memcpy a new frame in each tick without staging copies.
qboolean VK_CreateStreamingTextureRGBA(uint32_t w, uint32_t h, vk_texture_t* out);
qboolean VK_UpdateStreamingTexture(vk_texture_t* tex, const void* rgba_pixels);

#endif // VK_BUFFER_H
