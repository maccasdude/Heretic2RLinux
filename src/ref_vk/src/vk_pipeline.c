//
// vk_pipeline.c - 2D pipeline + descriptor set layout for textured quads.
//

#include "vk_local.h"
#include "vk_buffer.h"
#include "vk_pipeline.h"

#include <string.h>

vk_pipeline2d_t vk_pipeline_2d = {0};

// External (vk_shaders.c) SPIR-V blobs.
extern const uint32_t spirv_quad_vert_data[];
extern const uint32_t spirv_quad_vert_size;
extern const uint32_t spirv_quad_frag_data[];
extern const uint32_t spirv_quad_frag_size;

static VkShaderModule VK_CreateShaderModule(const uint32_t* code, size_t code_size)
{
    VkShaderModuleCreateInfo ci = {0};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code_size;
    ci.pCode    = code;
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(vk_state.device, &ci, NULL, &mod);
    return mod;
}

qboolean VK_CreateSampler2D(void)
{
    VkSamplerCreateInfo sci = {0};
    sci.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // 2D pics in H2 are pixel art - nearest filtering keeps them crisp.
    sci.magFilter     = VK_FILTER_NEAREST;
    sci.minFilter     = VK_FILTER_NEAREST;
    sci.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxAnisotropy = 1.0f;
    sci.maxLod        = 1.0f;
    return vkCreateSampler(vk_state.device, &sci, NULL, &vk_pipeline_2d.sampler) == VK_SUCCESS;
}

qboolean VK_CreatePipeline2D(void)
{
    if (!VK_CreateSampler2D()) {
        ri.Con_Printf(PRINT_ALL, "vk: VK_CreateSampler2D failed\n");
        return false;
    }

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
    if (vkCreateDescriptorSetLayout(vk_state.device, &dsl, NULL, &vk_pipeline_2d.descriptor_set_layout) != VK_SUCCESS)
        return false;

    // Descriptor pool. Each texture gets its own descriptor set; we cap at
    // VK_MAX_TEXTURES (defined in vk_pipeline.h) which is more than enough
    // for menu+console+HUD assets.
    VkDescriptorPoolSize pool_size = {0};
    pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = VK_MAX_TEXTURES;

    VkDescriptorPoolCreateInfo pool_ci = {0};
    pool_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets       = VK_MAX_TEXTURES;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes    = &pool_size;
    if (vkCreateDescriptorPool(vk_state.device, &pool_ci, NULL, &vk_pipeline_2d.descriptor_pool) != VK_SUCCESS)
        return false;

    // Pipeline layout (descriptor set + push constant for viewport size).
    VkPushConstantRange pcr = {0};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(float) * 2;     // vec2 viewport_size

    VkPipelineLayoutCreateInfo pl = {0};
    pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount         = 1;
    pl.pSetLayouts            = &vk_pipeline_2d.descriptor_set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(vk_state.device, &pl, NULL, &vk_pipeline_2d.layout) != VK_SUCCESS)
        return false;

    // Shader modules.
    VkShaderModule vs = VK_CreateShaderModule(spirv_quad_vert_data, spirv_quad_vert_size);
    VkShaderModule fs = VK_CreateShaderModule(spirv_quad_frag_data, spirv_quad_frag_size);
    if (!vs || !fs) {
        ri.Con_Printf(PRINT_ALL, "vk: shader module creation failed\n");
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

    // Vertex input: matches vk_vertex2d_t.
    VkVertexInputBindingDescription vb = {0};
    vb.binding   = 0;
    vb.stride    = sizeof(vk_vertex2d_t);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription va[3] = {0};
    va[0].location = 0;
    va[0].binding  = 0;
    va[0].format   = VK_FORMAT_R32G32_SFLOAT;
    va[0].offset   = 0;
    va[1].location = 1;
    va[1].binding  = 0;
    va[1].format   = VK_FORMAT_R32G32_SFLOAT;
    va[1].offset   = sizeof(float) * 2;
    va[2].location = 2;
    va[2].binding  = 0;
    va[2].format   = VK_FORMAT_R8G8B8A8_UNORM;
    va[2].offset   = sizeof(float) * 4;

    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &vb;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions    = va;

    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Dynamic viewport/scissor so we don't have to rebuild the pipeline on
    // window resize.
    VkPipelineViewportStateCreateInfo vp = {0};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;        // 2D - no culling
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // 2D draws don't write depth and aren't depth-tested. We still need a
    // depth-stencil state because the renderpass has a depth attachment.
    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    // Alpha-blended output (so transparent menu pics composite correctly).
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.blendEnable         = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
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
    gp.layout              = vk_pipeline_2d.layout;
    gp.renderPass          = vk_state.render_pass;
    gp.subpass             = 0;

    VkResult r = vkCreateGraphicsPipelines(vk_state.device, VK_NULL_HANDLE, 1, &gp, NULL, &vk_pipeline_2d.pipeline);

    vkDestroyShaderModule(vk_state.device, vs, NULL);
    vkDestroyShaderModule(vk_state.device, fs, NULL);

    if (r != VK_SUCCESS) {
        ri.Con_Printf(PRINT_ALL, "vk: vkCreateGraphicsPipelines failed (%d)\n", r);
        return false;
    }
    return true;
}

void VK_DestroyPipeline2D(void)
{
    if (vk_pipeline_2d.pipeline)
        vkDestroyPipeline(vk_state.device, vk_pipeline_2d.pipeline, NULL);
    if (vk_pipeline_2d.layout)
        vkDestroyPipelineLayout(vk_state.device, vk_pipeline_2d.layout, NULL);
    if (vk_pipeline_2d.descriptor_pool)
        vkDestroyDescriptorPool(vk_state.device, vk_pipeline_2d.descriptor_pool, NULL);
    if (vk_pipeline_2d.descriptor_set_layout)
        vkDestroyDescriptorSetLayout(vk_state.device, vk_pipeline_2d.descriptor_set_layout, NULL);
    if (vk_pipeline_2d.sampler)
        vkDestroySampler(vk_state.device, vk_pipeline_2d.sampler, NULL);
    memset(&vk_pipeline_2d, 0, sizeof(vk_pipeline_2d));
}

VkDescriptorSet VK_AllocDescriptorSetFor(VkImageView view)
{
    VkDescriptorSetAllocateInfo ai = {0};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = vk_pipeline_2d.descriptor_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &vk_pipeline_2d.descriptor_set_layout;

    VkDescriptorSet ds = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vk_state.device, &ai, &ds) != VK_SUCCESS) {
        ri.Con_Printf(PRINT_ALL, "vk: descriptor pool exhausted\n");
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo dii = {0};
    dii.sampler     = vk_pipeline_2d.sampler;
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
