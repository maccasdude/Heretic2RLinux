//
// vk_pipeline3d.h - 3D world pipeline.
//

#ifndef VK_PIPELINE3D_H_INCLUDED
#define VK_PIPELINE3D_H_INCLUDED

#include "vk_local.h"

typedef struct {
    VkPipeline       pipeline;             // opaque
    VkPipeline       pipeline_translucent; // depth-test on, depth-write off, alpha blend
    VkPipeline       pipeline_translucent_zwrite; // depth-test+WRITE on, alpha blend (entity-pass sprites/models)
    VkPipeline       pipeline_additive;    // depth-test on, depth-write off, ONE+ONE blend
    VkPipeline       pipeline_additive_nodepth; // depth-test OFF, ONE+ONE (RF_NODEPTHTEST)
    VkPipeline       pipeline_translucent_nodepth; // depth-test OFF, alpha blend (RF_NODEPTHTEST)
    VkPipeline       pipeline_reflect;     // opaque, sphere-map env reflection (RF_REFLECTION)
    VkPipelineLayout layout;
    VkRenderPass     render_pass;   // separate pass with depth buffer

    // Texture sampling for world surfaces.
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorPool      descriptor_pool;
    VkSampler             sampler;
} vk_pipeline3d_t;

extern vk_pipeline3d_t vk_pipeline_3d;

qboolean VK_CreatePipeline3D(void);
void     VK_DestroyPipeline3D(void);

// Allocate a descriptor set bound to a given image view (world textures).
VkDescriptorSet VK_AllocWorldDescriptor(VkImageView view);
void VK_FreeWorldDescriptor(VkDescriptorSet ds);

#endif
