//
// vk_pipeline_world.h - dedicated 3D pipeline for world geometry with
// diffuse * lightmap shading.
//

#ifndef VK_PIPELINE_WORLD_H_INCLUDED
#define VK_PIPELINE_WORLD_H_INCLUDED

#include "vk_local.h"
#include "vk_buffer.h"

typedef struct {
    VkPipeline            pipeline;
    VkPipelineLayout      layout;
    VkDescriptorSetLayout descriptor_set_layout;   // 2 combined-image-samplers (diffuse, lightmap)
    VkDescriptorPool      descriptor_pool;
    VkSampler             sampler_diffuse;          // REPEAT for tiling
    VkSampler             sampler_lightmap;         // CLAMP_TO_EDGE for atlas
    // Warp/translucent variant: turbsin water warp + undulate + alpha blend,
    // depth-test on / depth-write off. Shares the descriptor layout above but
    // has its own layout (larger push constant) and pipeline.
    VkPipeline            warp_pipeline;
    VkPipelineLayout      warp_layout;
    // Dynamic-light UBO (set = 1, binding 0). One per frame in flight. Holds the
    // active dlights; the world/warp fragment shaders add their contribution
    // per-pixel (faithful to ref_gl1 R_AddDynamicLights, done per-pixel here).
    VkDescriptorSetLayout dlight_set_layout;
    VkDescriptorPool      dlight_pool;
    VkDescriptorSet       dlight_set[MAX_FRAMES_IN_FLIGHT];
    vk_buffer_t           dlight_ubo[MAX_FRAMES_IN_FLIGHT];
} vk_pipeline_world_t;

extern vk_pipeline_world_t vk_pipeline_world;

qboolean VK_CreatePipelineWorld(void);
void     VK_DestroyPipelineWorld(void);
VkDescriptorSet VK_World_UpdateDlights(int num_dlights, const float* origins,
                                       const float* intensities, const float* colors_rgb,
                                       float modulate, const float viewproj[16],
                                       const float fog12[12]);
VkDescriptorSet VK_World_CurrentDlightSet(void);
qboolean VK_World_CullWorldCorners(const float corners[8][3]);

// Allocate a descriptor set bound to (diffuse_view, lightmap_view).
VkDescriptorSet VK_AllocWorldPairDescriptor(VkImageView diffuse, VkImageView lightmap);
void VK_ResetWorldPairDescriptors(void);

#endif
