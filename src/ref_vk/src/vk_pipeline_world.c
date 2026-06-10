//
// vk_pipeline_world.c
//
// World rendering pipeline: vertex (pos + diffuse_uv + lightmap_uv) with
// two combined-image-samplers in set 0 (binding 0 = diffuse, binding 1 =
// lightmap atlas). Fragment = diffuse * (lightmap * 2.0) so the engine's
// pre-divided lightmap values map back to ~unity at white.
//

#include "vk_pipeline_world.h"
#include "vk_local.h"

#include <string.h>

vk_pipeline_world_t vk_pipeline_world = {0};

// Dynamic-light UBO layout (std140). Mirrors the 'DLights' block in world.frag /
// world_warp.frag. Each light: xyz = origin, w = intensity; color xyz, w unused.
// MAX_DLIGHTS (32) matches the engine's refdef cap.
#define VK_MAX_DLIGHTS 32
typedef struct {
    int   count;
    float modulate;
    float _pad0, _pad1;
    float pos[VK_MAX_DLIGHTS][4];    // xyz origin, w intensity
    float color[VK_MAX_DLIGHTS][4];  // xyz color (0..1), w unused
} dlight_ubo_t;

extern const uint32_t spirv_world_vert_data[];
extern const uint32_t spirv_world_vert_size;
extern const uint32_t spirv_world_frag_data[];
extern const uint32_t spirv_world_frag_size;
extern const uint32_t spirv_world_warp_vert_data[];
extern const uint32_t spirv_world_warp_vert_size;
extern const uint32_t spirv_world_warp_frag_data[];
extern const uint32_t spirv_world_warp_frag_size;

typedef struct { float x,y,z; float du,dv; float lu,lv; } world_vert_t;

#define VK_MAX_WORLD_SETS 1024

static VkShaderModule MakeSM(const uint32_t* code, size_t code_size)
{
    VkShaderModuleCreateInfo ci = {0};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code_size;
    ci.pCode    = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(vk_state.device, &ci, NULL, &m);
    return m;
}

qboolean VK_CreatePipelineWorld(void)
{
    // Two samplers: REPEAT for tiling diffuse, CLAMP for lightmap atlas.
    VkSamplerCreateInfo sd = {0};
    sd.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sd.magFilter     = VK_FILTER_LINEAR;
    sd.minFilter     = VK_FILTER_LINEAR;
    sd.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sd.addressModeU  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sd.addressModeV  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sd.addressModeW  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sd.maxAnisotropy = 1.0f;
    sd.maxLod        = 1.0f;
    if (vkCreateSampler(vk_state.device, &sd, NULL, &vk_pipeline_world.sampler_diffuse) != VK_SUCCESS)
        return false;

    VkSamplerCreateInfo sl = sd;
    sl.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sl.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sl.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(vk_state.device, &sl, NULL, &vk_pipeline_world.sampler_lightmap) != VK_SUCCESS)
        return false;

    // Descriptor set layout: 2 combined image samplers.
    VkDescriptorSetLayoutBinding b[2] = {0};
    b[0].binding         = 0;
    b[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1;
    b[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding         = 1;
    b[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].descriptorCount = 1;
    b[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dsl = {0};
    dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = 2;
    dsl.pBindings    = b;
    if (vkCreateDescriptorSetLayout(vk_state.device, &dsl, NULL, &vk_pipeline_world.descriptor_set_layout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize pool_size = {0};
    pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = VK_MAX_WORLD_SETS * 2;

    VkDescriptorPoolCreateInfo pool_ci = {0};
    pool_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets       = VK_MAX_WORLD_SETS;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes    = &pool_size;
    if (vkCreateDescriptorPool(vk_state.device, &pool_ci, NULL, &vk_pipeline_world.descriptor_pool) != VK_SUCCESS)
        return false;

    // --- Dynamic-light UBO: set = 1, binding 0 (fragment) ---
    {
        VkDescriptorSetLayoutBinding ub = {0};
        ub.binding         = 0;
        ub.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ub.descriptorCount = 1;
        ub.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo udsl = {0};
        udsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        udsl.bindingCount = 1;
        udsl.pBindings    = &ub;
        if (vkCreateDescriptorSetLayout(vk_state.device, &udsl, NULL,
                                        &vk_pipeline_world.dlight_set_layout) != VK_SUCCESS)
            return false;

        VkDescriptorPoolSize ups = {0};
        ups.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ups.descriptorCount = MAX_FRAMES_IN_FLIGHT;

        VkDescriptorPoolCreateInfo upool = {0};
        upool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        upool.maxSets       = MAX_FRAMES_IN_FLIGHT;
        upool.poolSizeCount = 1;
        upool.pPoolSizes    = &ups;
        if (vkCreateDescriptorPool(vk_state.device, &upool, NULL,
                                   &vk_pipeline_world.dlight_pool) != VK_SUCCESS)
            return false;

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            if (!VK_CreateBuffer(sizeof(dlight_ubo_t),
                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 &vk_pipeline_world.dlight_ubo[i]))
                return false;
            if (!VK_MapBuffer(&vk_pipeline_world.dlight_ubo[i]))
                return false;
            // zero it so the first frames before any update are well-defined
            memset(vk_pipeline_world.dlight_ubo[i].mapped, 0, sizeof(dlight_ubo_t));

            VkDescriptorSetAllocateInfo dai = {0};
            dai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dai.descriptorPool     = vk_pipeline_world.dlight_pool;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts        = &vk_pipeline_world.dlight_set_layout;
            if (vkAllocateDescriptorSets(vk_state.device, &dai,
                                         &vk_pipeline_world.dlight_set[i]) != VK_SUCCESS)
                return false;

            VkDescriptorBufferInfo bi = {0};
            bi.buffer = vk_pipeline_world.dlight_ubo[i].buffer;
            bi.offset = 0;
            bi.range  = sizeof(dlight_ubo_t);

            VkWriteDescriptorSet wr = {0};
            wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr.dstSet          = vk_pipeline_world.dlight_set[i];
            wr.dstBinding      = 0;
            wr.descriptorCount = 1;
            wr.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wr.pBufferInfo     = &bi;
            vkUpdateDescriptorSets(vk_state.device, 1, &wr, 0, NULL);
        }
    }

    // Both the world layout and the warp layout include set 0 (samplers) and
    // set 1 (dlight UBO).
    VkDescriptorSetLayout set_layouts[2] = {
        vk_pipeline_world.descriptor_set_layout,
        vk_pipeline_world.dlight_set_layout,
    };

    VkPushConstantRange pcr = {0};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset     = 0;
    pcr.size       = 64 + 48 + 64;  // mat4 mvp + 3 vec4 fog + mat4 model

    VkPipelineLayoutCreateInfo pl = {0};
    pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount         = 2;
    pl.pSetLayouts            = set_layouts;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(vk_state.device, &pl, NULL, &vk_pipeline_world.layout) != VK_SUCCESS)
        return false;

    VkShaderModule vs = MakeSM(spirv_world_vert_data, spirv_world_vert_size);
    VkShaderModule fs = MakeSM(spirv_world_frag_data, spirv_world_frag_size);

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
    vb.binding = 0; vb.stride = sizeof(world_vert_t);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription va[3] = {0};
    va[0].location = 0; va[0].format = VK_FORMAT_R32G32B32_SFLOAT; va[0].offset = 0;
    va[1].location = 1; va[1].format = VK_FORMAT_R32G32_SFLOAT;    va[1].offset = sizeof(float)*3;
    va[2].location = 2; va[2].format = VK_FORMAT_R32G32_SFLOAT;    va[2].offset = sizeof(float)*5;

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
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;  // GL1 uses GL_LEQUAL; lets coplanar surfaces resolve by draw order instead of z-fighting

    VkPipelineColorBlendAttachmentState cba = {0};
    cba.blendEnable    = VK_FALSE;
    cba.colorWriteMask = 0xF;

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
    gp.layout              = vk_pipeline_world.layout;
    gp.renderPass          = vk_state.render_pass;

    VkResult r = vkCreateGraphicsPipelines(vk_state.device, VK_NULL_HANDLE, 1, &gp, NULL, &vk_pipeline_world.pipeline);
    vkDestroyShaderModule(vk_state.device, vs, NULL);
    vkDestroyShaderModule(vk_state.device, fs, NULL);
    if (r != VK_SUCCESS) return false;

    // ---- Warp / translucent pipeline (water, lava, glass) ----
    // Bigger push constant: mat4 mvp + 2 vec4 (time/alpha/flowing/undulate +
    // isWarp). Alpha blend SRC_ALPHA, depth-test ON, depth-write OFF (so
    // translucent surfaces sort against opaque depth but don't occlude each
    // other oddly), matching ref_gl1's R_DrawAlphaSurfaces blend setup.
    VkPushConstantRange wpcr = {0};
    wpcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    wpcr.offset     = 0;
    wpcr.size       = 64 + 32 + 48;   // mat4 + 2 vec4 (warp) + 3 vec4 (fog) = 144

    VkPipelineLayoutCreateInfo wpl = {0};
    wpl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    wpl.setLayoutCount         = 2;
    wpl.pSetLayouts            = set_layouts;
    wpl.pushConstantRangeCount = 1;
    wpl.pPushConstantRanges    = &wpcr;
    if (vkCreatePipelineLayout(vk_state.device, &wpl, NULL, &vk_pipeline_world.warp_layout) != VK_SUCCESS)
        return false;

    VkShaderModule wvs = MakeSM(spirv_world_warp_vert_data, spirv_world_warp_vert_size);
    VkShaderModule wfs = MakeSM(spirv_world_warp_frag_data, spirv_world_warp_frag_size);
    VkPipelineShaderStageCreateInfo wstages[2] = {0};
    wstages[0] = stages[0]; wstages[0].module = wvs;
    wstages[1] = stages[1]; wstages[1].module = wfs;

    VkPipelineColorBlendAttachmentState wcba = {0};
    wcba.blendEnable         = VK_TRUE;
    wcba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    wcba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    wcba.colorBlendOp        = VK_BLEND_OP_ADD;
    wcba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    wcba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    wcba.alphaBlendOp        = VK_BLEND_OP_ADD;
    wcba.colorWriteMask      = 0xF;
    VkPipelineColorBlendStateCreateInfo wcb = cb;
    wcb.pAttachments = &wcba;

    VkPipelineDepthStencilStateCreateInfo wds = ds;
    wds.depthWriteEnable = VK_FALSE;   // translucent: test but don't write

    // Cull the back face of warp surfaces (v79 culled the wrong side - you could
    // see through the front of the forcefield to the room behind). Flipped to
    // FRONT_FACE_CLOCKWISE so the visible near side is kept.
    VkPipelineRasterizationStateCreateInfo wrs = rs;
    wrs.cullMode  = VK_CULL_MODE_BACK_BIT;
    wrs.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkGraphicsPipelineCreateInfo wgp = gp;
    wgp.pStages            = wstages;
    wgp.pColorBlendState   = &wcb;
    wgp.pDepthStencilState = &wds;
    wgp.pRasterizationState = &wrs;
    wgp.layout             = vk_pipeline_world.warp_layout;

    VkResult wr = vkCreateGraphicsPipelines(vk_state.device, VK_NULL_HANDLE, 1, &wgp, NULL, &vk_pipeline_world.warp_pipeline);
    vkDestroyShaderModule(vk_state.device, wvs, NULL);
    vkDestroyShaderModule(vk_state.device, wfs, NULL);
    return wr == VK_SUCCESS;
}

// Fill the current frame's dlight UBO from the refdef and return the descriptor
// set to bind at set = 1. 'modulate' is gl_modulate. Done per-pixel in the
// world/warp fragment shaders (ref_gl1 does it per lightmap texel).
VkDescriptorSet VK_World_UpdateDlights(int num_dlights, const float* origins,
                                       const float* intensities, const float* colors_rgb,
                                       float modulate){
    const uint32_t f = vk_state.current_frame % MAX_FRAMES_IN_FLIGHT;
    dlight_ubo_t* u = (dlight_ubo_t*)vk_pipeline_world.dlight_ubo[f].mapped;
    if (!u) return VK_NULL_HANDLE;

    int n = num_dlights;
    if (n < 0) n = 0;
    if (n > VK_MAX_DLIGHTS) n = VK_MAX_DLIGHTS;
    u->count    = n;
    u->modulate = modulate;
    for (int i = 0; i < n; i++) {
        u->pos[i][0]   = origins[i*3+0];
        u->pos[i][1]   = origins[i*3+1];
        u->pos[i][2]   = origins[i*3+2];
        u->pos[i][3]   = intensities[i];
        u->color[i][0] = colors_rgb[i*3+0];
        u->color[i][1] = colors_rgb[i*3+1];
        u->color[i][2] = colors_rgb[i*3+2];
        u->color[i][3] = 0.0f;
    }
    return vk_pipeline_world.dlight_set[f];
}

// Return the current frame's dlight set without refilling (for passes that run
// after VK_World_UpdateDlights has already been called this frame, e.g.
// submodels). Returns VK_NULL_HANDLE if unavailable.
VkDescriptorSet VK_World_CurrentDlightSet(void)
{
    const uint32_t f = vk_state.current_frame % MAX_FRAMES_IN_FLIGHT;
    return vk_pipeline_world.dlight_set[f];
}

void VK_DestroyPipelineWorld(void)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        if (vk_pipeline_world.dlight_ubo[i].buffer) VK_DestroyBuffer(&vk_pipeline_world.dlight_ubo[i]);
    if (vk_pipeline_world.dlight_pool)           vkDestroyDescriptorPool(vk_state.device, vk_pipeline_world.dlight_pool, NULL);
    if (vk_pipeline_world.dlight_set_layout)     vkDestroyDescriptorSetLayout(vk_state.device, vk_pipeline_world.dlight_set_layout, NULL);
    if (vk_pipeline_world.warp_pipeline)         vkDestroyPipeline      (vk_state.device, vk_pipeline_world.warp_pipeline, NULL);
    if (vk_pipeline_world.warp_layout)           vkDestroyPipelineLayout(vk_state.device, vk_pipeline_world.warp_layout, NULL);
    if (vk_pipeline_world.pipeline)              vkDestroyPipeline      (vk_state.device, vk_pipeline_world.pipeline, NULL);
    if (vk_pipeline_world.layout)                vkDestroyPipelineLayout(vk_state.device, vk_pipeline_world.layout,   NULL);
    if (vk_pipeline_world.descriptor_pool)       vkDestroyDescriptorPool(vk_state.device, vk_pipeline_world.descriptor_pool, NULL);
    if (vk_pipeline_world.descriptor_set_layout) vkDestroyDescriptorSetLayout(vk_state.device, vk_pipeline_world.descriptor_set_layout, NULL);
    if (vk_pipeline_world.sampler_diffuse)       vkDestroySampler(vk_state.device, vk_pipeline_world.sampler_diffuse, NULL);
    if (vk_pipeline_world.sampler_lightmap)      vkDestroySampler(vk_state.device, vk_pipeline_world.sampler_lightmap, NULL);
    memset(&vk_pipeline_world, 0, sizeof(vk_pipeline_world));
}

VkDescriptorSet VK_AllocWorldPairDescriptor(VkImageView diffuse, VkImageView lightmap)
{
    VkDescriptorSetAllocateInfo ai = {0};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = vk_pipeline_world.descriptor_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &vk_pipeline_world.descriptor_set_layout;

    VkDescriptorSet ds = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vk_state.device, &ai, &ds) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    VkDescriptorImageInfo di[2] = {0};
    di[0].sampler     = vk_pipeline_world.sampler_diffuse;
    di[0].imageView   = diffuse;
    di[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    di[1].sampler     = vk_pipeline_world.sampler_lightmap;
    di[1].imageView   = lightmap;
    di[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet w[2] = {0};
    w[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet          = ds;
    w[0].dstBinding      = 0;
    w[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].descriptorCount = 1;
    w[0].pImageInfo      = &di[0];
    w[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet          = ds;
    w[1].dstBinding      = 1;
    w[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[1].descriptorCount = 1;
    w[1].pImageInfo      = &di[1];

    vkUpdateDescriptorSets(vk_state.device, 2, w, 0, NULL);
    return ds;
}

// Reset (free) ALL world-pair descriptors at once. Every pair descriptor binds
// a level's diffuse texture + that level's lightmap atlas, so they are entirely
// per-level - none are permanent. Rather than free individually (the pool has
// no FREE bit), we reset the whole pool when the world is torn down. Caller must
// ensure the GPU is idle (VK_World_Free waits before this).
void VK_ResetWorldPairDescriptors(void)
{
    if (vk_pipeline_world.descriptor_pool)
        vkResetDescriptorPool(vk_state.device, vk_pipeline_world.descriptor_pool, 0);
}
