//
// vk_pipeline.h - 2D pipeline state.
//

#ifndef VK_PIPELINE_H
#define VK_PIPELINE_H

#include "vk_local.h"

#define VK_MAX_TEXTURES  4096

typedef struct {
    float x, y;          // pixels (0..viddef.width)
    float u, v;          // 0..1
    uint8_t  r, g, b, a; // RGBA8
} vk_vertex2d_t;

typedef struct {
    VkPipeline             pipeline;
    VkPipelineLayout       layout;
    VkDescriptorSetLayout  descriptor_set_layout;
    VkDescriptorPool       descriptor_pool;
    VkSampler              sampler;
} vk_pipeline2d_t;

extern vk_pipeline2d_t vk_pipeline_2d;

qboolean VK_CreatePipeline2D(void);
void     VK_DestroyPipeline2D(void);

VkDescriptorSet VK_AllocDescriptorSetFor(VkImageView view);

#endif
