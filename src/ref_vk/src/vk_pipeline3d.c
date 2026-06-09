//
// vk_pipeline3d.c - 3D pipeline for world geometry.
//

#include "vk_pipeline3d.h"
#include "vk_pipeline_world.h"
#include "vk_local.h"

#include <string.h>

vk_pipeline3d_t vk_pipeline_3d = {0};

extern const uint32_t spirv_entity_vert_data[];
extern const uint32_t spirv_entity_vert_size;
extern const uint32_t spirv_entity_frag_data[];
extern const uint32_t spirv_entity_frag_size;
extern const uint32_t spirv_entity_reflect_vert_data[];
extern const uint32_t spirv_entity_reflect_vert_size;
extern const uint32_t spirv_entity_reflect_frag_data[];
extern const uint32_t spirv_entity_reflect_frag_size;

// NB: this is the ENTITY vertex layout (mirrors vkm_vert_t in vk_model.c), not
// the world vert. Position + diffuse UV + world-space normal (for reflection).
typedef struct {
    float x, y, z;
    float u, v;
    float nx, ny, nz;
} entity_vert_t;

static VkShaderModule MakeShader(const uint32_t* code, size_t code_size)
{
    VkShaderModuleCreateInfo ci = {0};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code_size;
    ci.pCode    = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(vk_state.device, &ci, NULL, &m);
    return m;
}

#define VK_MAX_WORLD_TEXTURES 1024

qboolean VK_CreatePipeline3D(void)
{
    // Sampler - repeat addressing (world textures tile), linear filtering.
    VkSamplerCreateInfo sci = {0};
    sci.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter     = VK_FILTER_LINEAR;
    sci.minFilter     = VK_FILTER_LINEAR;
    sci.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxAnisotropy = 1.0f;
    sci.maxLod        = 1.0f;
    if (vkCreateSampler(vk_state.device, &sci, NULL, &vk_pipeline_3d.sampler) != VK_SUCCESS)
        return false;

    // Descriptor set layout: one combined image-sampler at binding 0.
    VkDescriptorSetLayoutBinding b = {0};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dsl = {0};
    dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = 1;
    dsl.pBindings    = &b;
    if (vkCreateDescriptorSetLayout(vk_state.device, &dsl, NULL, &vk_pipeline_3d.descriptor_set_layout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize pool_size = {0};
    pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = VK_MAX_WORLD_TEXTURES;

    VkDescriptorPoolCreateInfo pool_ci = {0};
    pool_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    // FREE bit lets us release individual world/skin sets when evicting stale
    // per-level models/textures at level load (see VK_FreeUnusedImages / model
    // eviction in R_EndRegistration).
    pool_ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_ci.maxSets       = VK_MAX_WORLD_TEXTURES;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes    = &pool_size;
    if (vkCreateDescriptorPool(vk_state.device, &pool_ci, NULL, &vk_pipeline_3d.descriptor_pool) != VK_SUCCESS)
        return false;

    // Pipeline layout: descriptor set 0 + 80-byte push constant
    // (mat4 mvp + vec4 tint + 3 vec4 fog params).
    VkPushConstantRange pcr = {0};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset     = 0;
    pcr.size       = 128;    // sizeof(float) * (16 + 4 + 12)

    // set 0 = diffuse sampler; set 1 = shared dynamic-light UBO (created by the
    // world pipeline, which is initialized first).
    VkDescriptorSetLayout ent_sets[2] = {
        vk_pipeline_3d.descriptor_set_layout,
        vk_pipeline_world.dlight_set_layout,
    };
    VkPipelineLayoutCreateInfo pl = {0};
    pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount         = 2;
    pl.pSetLayouts            = ent_sets;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(vk_state.device, &pl, NULL, &vk_pipeline_3d.layout) != VK_SUCCESS)
        return false;

    VkShaderModule vs = MakeShader(spirv_entity_vert_data, spirv_entity_vert_size);
    VkShaderModule fs = MakeShader(spirv_entity_frag_data, spirv_entity_frag_size);
    if (!vs || !fs) {
        ri.Con_Printf(PRINT_ALL, "vk: 3D shader module creation failed\n");
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription vb = {0};
    vb.binding   = 0;
    vb.stride    = sizeof(entity_vert_t);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription va[3] = {0};
    va[0].location = 0;
    va[0].binding  = 0;
    va[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    va[0].offset   = 0;
    va[1].location = 1;
    va[1].binding  = 0;
    va[1].format   = VK_FORMAT_R32G32_SFLOAT;
    va[1].offset   = sizeof(float) * 3;
    va[2].location = 2;                              // world-space normal (reflection)
    va[2].binding  = 0;
    va[2].format   = VK_FORMAT_R32G32B32_SFLOAT;
    va[2].offset   = sizeof(float) * 5;

    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &vb;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions    = va;

    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = {0};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // No backface culling for Session 3a - we don't have winding-order info
    // yet and the BSP face 'side' field controls plane direction differently
    // per polygon; safer to draw both sides.
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType           = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;  // match GL1 GL_LEQUAL

    VkPipelineColorBlendAttachmentState cba = {0};
    cba.blendEnable    = VK_FALSE;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb = {0};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {0};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkGraphicsPipelineCreateInfo gp = {0};
    gp.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount          = 2;
    gp.pStages             = stages;
    gp.pVertexInputState   = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState      = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState   = &ms;
    gp.pDepthStencilState  = &ds;
    gp.pColorBlendState    = &cb;
    gp.pDynamicState       = &dyn;
    gp.layout              = vk_pipeline_3d.layout;
    gp.renderPass          = vk_state.render_pass;
    gp.subpass             = 0;

    VkResult r = vkCreateGraphicsPipelines(vk_state.device, VK_NULL_HANDLE, 1, &gp, NULL, &vk_pipeline_3d.pipeline);
    if (r != VK_SUCCESS) {
        vkDestroyShaderModule(vk_state.device, vs, NULL);
        vkDestroyShaderModule(vk_state.device, fs, NULL);
        ri.Con_Printf(PRINT_ALL, "vk: vkCreateGraphicsPipelines (3D opaque) failed (%d)\n", r);
        return false;
    }

    // Translucent variant: same everything, except enable alpha blending and
    // disable depth-write (still depth-test). For models / sprites where
    // the texture has alpha (transparent edges) or the entity has alpha tint.
    VkPipelineDepthStencilStateCreateInfo ds_t = ds;
    ds_t.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cba_t = cba;
    cba_t.blendEnable         = VK_TRUE;
    cba_t.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba_t.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba_t.colorBlendOp        = VK_BLEND_OP_ADD;
    cba_t.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba_t.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba_t.alphaBlendOp        = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo cb_t = cb;
    cb_t.pAttachments = &cba_t;

    gp.pDepthStencilState = &ds_t;
    gp.pColorBlendState   = &cb_t;

    r = vkCreateGraphicsPipelines(vk_state.device, VK_NULL_HANDLE, 1, &gp, NULL, &vk_pipeline_3d.pipeline_translucent);
    if (r != VK_SUCCESS) {
        vkDestroyShaderModule(vk_state.device, vs, NULL);
        vkDestroyShaderModule(vk_state.device, fs, NULL);
        ri.Con_Printf(PRINT_ALL, "vk: vkCreateGraphicsPipelines (3D translucent) failed (%d)\n", r);
        return false;
    }

    // Additive variant: ONE + ONE blend (for RF_TRANS_ADD: glows, coronas,
    // crosshair, many spell effects). Depth-test on, depth-write off.
    VkPipelineColorBlendAttachmentState cba_a = cba_t;
    cba_a.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba_a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba_a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba_a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;

    VkPipelineColorBlendStateCreateInfo cb_a = cb_t;
    cb_a.pAttachments = &cba_a;
    gp.pColorBlendState = &cb_a;

    r = vkCreateGraphicsPipelines(vk_state.device, VK_NULL_HANDLE, 1, &gp, NULL, &vk_pipeline_3d.pipeline_additive);

    if (r != VK_SUCCESS) {
        vkDestroyShaderModule(vk_state.device, vs, NULL);
        vkDestroyShaderModule(vk_state.device, fs, NULL);
        ri.Con_Printf(PRINT_ALL, "vk: vkCreateGraphicsPipelines (3D additive) failed (%d)\n", r);
        return false;
    }

    // Additive + no depth test (RF_NODEPTHTEST: crosshair, always-on-top fx).
    VkPipelineDepthStencilStateCreateInfo ds_nd = ds;
    ds_nd.depthTestEnable  = VK_FALSE;
    ds_nd.depthWriteEnable = VK_FALSE;
    gp.pDepthStencilState  = &ds_nd;
    // color blend stays additive (cb_a from above)
    r = vkCreateGraphicsPipelines(vk_state.device, VK_NULL_HANDLE, 1, &gp, NULL, &vk_pipeline_3d.pipeline_additive_nodepth);
    if (r != VK_SUCCESS) {
        vkDestroyShaderModule(vk_state.device, vs, NULL);
        vkDestroyShaderModule(vk_state.device, fs, NULL);
        ri.Con_Printf(PRINT_ALL, "vk: vkCreateGraphicsPipelines (3D additive-nodepth) failed (%d)\n", r);
        return false;
    }

    // Translucent (alpha blend) + no depth test (RF_NODEPTHTEST decals/fx).
    gp.pColorBlendState   = &cb_t;       // alpha-blend attachment
    gp.pDepthStencilState = &ds_nd;      // depth test off
    r = vkCreateGraphicsPipelines(vk_state.device, VK_NULL_HANDLE, 1, &gp, NULL, &vk_pipeline_3d.pipeline_translucent_nodepth);
    if (r != VK_SUCCESS) {
        vkDestroyShaderModule(vk_state.device, vs, NULL);
        vkDestroyShaderModule(vk_state.device, fs, NULL);
        ri.Con_Printf(PRINT_ALL, "vk: vkCreateGraphicsPipelines (3D translucent-nodepth) failed (%d)\n", r);
        return false;
    }

    // Translucent (alpha blend) + depth-WRITE on. ref_gl1 draws the entity pass
    // (sprites, alpha models) with glDepthMask(GL_TRUE) and only disables depth
    // writes for the alpha-surface (water) + particle passes that follow. So
    // entity-pass sprites/models must write depth, otherwise the later
    // depth-write-off water paints over swing-vines, projectiles and other
    // effect billboards that are physically in front of it. The v91 alpha-test
    // discard means cutout sprites only write depth on their solid texels, so
    // this matches GL without causing translucent self-occlusion artifacts.
    {
        VkPipelineDepthStencilStateCreateInfo ds_tw = ds;  // depth test + WRITE on
        gp.pDepthStencilState = &ds_tw;
        gp.pColorBlendState   = &cb_t;   // alpha-blend attachment
        r = vkCreateGraphicsPipelines(vk_state.device, VK_NULL_HANDLE, 1, &gp, NULL, &vk_pipeline_3d.pipeline_translucent_zwrite);
        if (r != VK_SUCCESS) {
            vkDestroyShaderModule(vk_state.device, vs, NULL);
            vkDestroyShaderModule(vk_state.device, fs, NULL);
            ri.Con_Printf(PRINT_ALL, "vk: vkCreateGraphicsPipelines (3D translucent-zwrite) failed (%d)\n", r);
            return false;
        }
    }

    // Reflection variant (RF_REFLECTION / FMNI_USE_REFLECT): sphere-map env
    // mapping. Same layout, but uses the reflection shader pair (computes a
    // sphere-map UV from the world normal and samples the reflect texture bound
    // at set 0). Opaque depth state (depth test + write on, like the base).
    VkShaderModule rvs = MakeShader(spirv_entity_reflect_vert_data, spirv_entity_reflect_vert_size);
    VkShaderModule rfs = MakeShader(spirv_entity_reflect_frag_data, spirv_entity_reflect_frag_size);
    if (rvs && rfs) {
        VkPipelineShaderStageCreateInfo rstages[2] = { stages[0], stages[1] };
        rstages[0].module = rvs;
        rstages[1].module = rfs;
        gp.pStages            = rstages;
        gp.pDepthStencilState = &ds;     // opaque depth (test + write)
        gp.pColorBlendState   = &cb;     // opaque (no blend)
        r = vkCreateGraphicsPipelines(vk_state.device, VK_NULL_HANDLE, 1, &gp, NULL, &vk_pipeline_3d.pipeline_reflect);
        vkDestroyShaderModule(vk_state.device, rvs, NULL);
        vkDestroyShaderModule(vk_state.device, rfs, NULL);
        if (r != VK_SUCCESS) {
            vkDestroyShaderModule(vk_state.device, vs, NULL);
            vkDestroyShaderModule(vk_state.device, fs, NULL);
            ri.Con_Printf(PRINT_ALL, "vk: vkCreateGraphicsPipelines (3D reflect) failed (%d)\n", r);
            return false;
        }
    }

    vkDestroyShaderModule(vk_state.device, vs, NULL);
    vkDestroyShaderModule(vk_state.device, fs, NULL);

    ri.Con_Printf(PRINT_ALL, "vk: 3D pipelines created (opaque=%p translucent=%p additive=%p)\n",
                  (void*)vk_pipeline_3d.pipeline, (void*)vk_pipeline_3d.pipeline_translucent,
                  (void*)vk_pipeline_3d.pipeline_additive);
    return true;
}

void VK_DestroyPipeline3D(void)
{
    if (vk_pipeline_3d.pipeline)              vkDestroyPipeline      (vk_state.device, vk_pipeline_3d.pipeline, NULL);
    if (vk_pipeline_3d.pipeline_translucent)  vkDestroyPipeline      (vk_state.device, vk_pipeline_3d.pipeline_translucent, NULL);
    if (vk_pipeline_3d.pipeline_translucent_zwrite) vkDestroyPipeline (vk_state.device, vk_pipeline_3d.pipeline_translucent_zwrite, NULL);
    if (vk_pipeline_3d.pipeline_additive)     vkDestroyPipeline      (vk_state.device, vk_pipeline_3d.pipeline_additive, NULL);
    if (vk_pipeline_3d.pipeline_additive_nodepth) vkDestroyPipeline  (vk_state.device, vk_pipeline_3d.pipeline_additive_nodepth, NULL);
    if (vk_pipeline_3d.pipeline_translucent_nodepth) vkDestroyPipeline(vk_state.device, vk_pipeline_3d.pipeline_translucent_nodepth, NULL);
    if (vk_pipeline_3d.pipeline_reflect)      vkDestroyPipeline      (vk_state.device, vk_pipeline_3d.pipeline_reflect, NULL);
    if (vk_pipeline_3d.layout)                vkDestroyPipelineLayout(vk_state.device, vk_pipeline_3d.layout,   NULL);
    if (vk_pipeline_3d.descriptor_pool)       vkDestroyDescriptorPool(vk_state.device, vk_pipeline_3d.descriptor_pool, NULL);
    if (vk_pipeline_3d.descriptor_set_layout) vkDestroyDescriptorSetLayout(vk_state.device, vk_pipeline_3d.descriptor_set_layout, NULL);
    if (vk_pipeline_3d.sampler)               vkDestroySampler(vk_state.device, vk_pipeline_3d.sampler, NULL);
    memset(&vk_pipeline_3d, 0, sizeof(vk_pipeline_3d));
}

VkDescriptorSet VK_AllocWorldDescriptor(VkImageView view)
{
    VkDescriptorSetAllocateInfo ai = {0};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = vk_pipeline_3d.descriptor_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &vk_pipeline_3d.descriptor_set_layout;

    VkDescriptorSet ds = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vk_state.device, &ai, &ds) != VK_SUCCESS) {
        ri.Con_Printf(PRINT_ALL, "vk: world descriptor pool exhausted\n");
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo dii = {0};
    dii.sampler     = vk_pipeline_3d.sampler;
    dii.imageView   = view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet w = {0};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = ds;
    w.dstBinding      = 0;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.descriptorCount = 1;
    w.pImageInfo      = &dii;
    vkUpdateDescriptorSets(vk_state.device, 1, &w, 0, NULL);
    return ds;
}

// Free a descriptor set allocated from the world pool. Valid because the pool
// uses VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT. Caller must ensure the
// set is not referenced by any in-flight frame (we evict only at level load,
// behind vkDeviceWaitIdle).
void VK_FreeWorldDescriptor(VkDescriptorSet ds)
{
    if (ds != VK_NULL_HANDLE)
        vkFreeDescriptorSets(vk_state.device, vk_pipeline_3d.descriptor_pool, 1, &ds);
}
