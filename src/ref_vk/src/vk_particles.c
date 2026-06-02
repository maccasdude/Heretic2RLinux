//
// vk_particles.c
//
// H2 particles are camera-facing diamonds (square rotated 45°), one per
// particle, with per-particle color and a texture lookup from a fixed
// 62-entry UV atlas. Two flavors:
//   normal:    SRC_ALPHA + ONE_MINUS_SRC_ALPHA   (particles)
//   additive:  ONE + ONE                         (aparticles)
//
// We implement both as separate pipelines (different blend states) sharing
// the same shader and per-frame VBO.
//

#include "vk_particles.h"
#include "vk_local.h"
#include "vk_buffer.h"
#include "vk_image.h"
#include "vk_pipeline3d.h"

#include "client/ref.h"
#include "qcommon/ParticleFlags.h"

#include <stdlib.h>
#include <string.h>

// 62 particle UV regions (xl, yt, xr, yb) in the 256x256 particle atlas.
// Copied verbatim from gl1_Main.c R_DrawParticles.
static const float k_particle_st[62][4] = {
    { 0.00390625f, 0.00390625f, 0.02734375f, 0.02734375f },
    { 0.03515625f, 0.00390625f, 0.05859375f, 0.02734375f },
    { 0.06640625f, 0.00390625f, 0.08984375f, 0.02734375f },
    { 0.09765625f, 0.00390625f, 0.12109375f, 0.02734375f },
    { 0.00390625f, 0.03515625f, 0.02734375f, 0.05859375f },
    { 0.03515625f, 0.03515625f, 0.05859375f, 0.05859375f },
    { 0.06640625f, 0.03515625f, 0.08984375f, 0.05859375f },
    { 0.09765625f, 0.03515625f, 0.12109375f, 0.05859375f },
    { 0.00390625f, 0.06640625f, 0.02734375f, 0.08984375f },
    { 0.03515625f, 0.06640625f, 0.05859375f, 0.08984375f },
    { 0.06640625f, 0.06640625f, 0.08984375f, 0.08984375f },
    { 0.09765625f, 0.06640625f, 0.12109375f, 0.08984375f },
    { 0.00390625f, 0.09765625f, 0.02734375f, 0.12109375f },
    { 0.03515625f, 0.09765625f, 0.05859375f, 0.12109375f },
    { 0.06640625f, 0.09765625f, 0.08984375f, 0.12109375f },
    { 0.09765625f, 0.09765625f, 0.12109375f, 0.12109375f },
    { 0.12890625f, 0.00390625f, 0.18359375f, 0.05859375f },
    { 0.19140625f, 0.00390625f, 0.24609375f, 0.05859375f },
    { 0.12890625f, 0.06640625f, 0.18359375f, 0.12109375f },
    { 0.19140625f, 0.06640625f, 0.24609375f, 0.12109375f },
    { 0.00390625f, 0.12890625f, 0.12109375f, 0.24609375f },
    { 0.12890625f, 0.12890625f, 0.24609375f, 0.24609375f },
    { 0.25390625f, 0.00390625f, 0.37109375f, 0.12109375f },
    { 0.37890625f, 0.00390625f, 0.49609375f, 0.12109375f },
    { 0.25390625f, 0.12890625f, 0.37109375f, 0.24609375f },
    { 0.37890625f, 0.12890625f, 0.49609375f, 0.24609375f },
    { 0.00390625f, 0.25390625f, 0.24609375f, 0.49609375f },
    { 0.25390625f, 0.25390625f, 0.49609375f, 0.49609375f },
    { 0.50390625f, 0.00390625f, 0.74609375f, 0.24609375f },
    { 0.75390625f, 0.00390625f, 0.99609375f, 0.24609375f },
    { 0.50390625f, 0.25390625f, 0.74609375f, 0.49609375f },
    { 0.75390625f, 0.25390625f, 0.87109375f, 0.37109375f },
    { 0.87890625f, 0.25390625f, 0.99609375f, 0.37109375f },
    { 0.75390625f, 0.37890625f, 0.87109375f, 0.49609375f },
    { 0.87890625f, 0.37890625f, 0.99609375f, 0.49609375f },
    { 0.00390625f, 0.50390625f, 0.24609375f, 0.74609375f },
    { 0.00390625f, 0.50390625f, 0.24609375f, 0.74609375f },
    { 0.25390625f, 0.50390625f, 0.37109375f, 0.62109375f },
    { 0.37890625f, 0.50390625f, 0.43359375f, 0.55859375f },
    { 0.44140625f, 0.50390625f, 0.49609375f, 0.55859375f },
    { 0.37890625f, 0.56640625f, 0.43359375f, 0.62109375f },
    { 0.44140625f, 0.56640625f, 0.49609375f, 0.62109375f },
    { 0.25390625f, 0.62890625f, 0.30859375f, 0.68359375f },
    { 0.31640625f, 0.62890625f, 0.37109375f, 0.68359375f },
    { 0.25390625f, 0.69140625f, 0.30859375f, 0.74609375f },
    { 0.31640625f, 0.69140625f, 0.37109375f, 0.74609375f },
    { 0.37890625f, 0.62890625f, 0.43359375f, 0.68359375f },
    { 0.44140625f, 0.62890625f, 0.49609375f, 0.68359375f },
    { 0.37890625f, 0.69140625f, 0.43359375f, 0.74609375f },
    { 0.44140625f, 0.69140625f, 0.49609375f, 0.74609375f },
    { 0.00390625f, 0.75390625f, 0.24609375f, 0.99609375f },
    { 0.25390625f, 0.75390625f, 0.49609375f, 0.99609375f },
    { 0.50390625f, 0.50390625f, 0.62109375f, 0.62109375f },
    { 0.62890625f, 0.50390625f, 0.74609375f, 0.62109375f },
    { 0.50390625f, 0.62890625f, 0.62109375f, 0.74609375f },
    { 0.62890625f, 0.62890625f, 0.74609375f, 0.74609375f },
    { 0.75390625f, 0.50390625f, 0.99609375f, 0.74609375f },
    { 0.50390625f, 0.75390625f, 0.74609375f, 0.99609375f },
    { 0.75390625f, 0.75390625f, 0.87109375f, 0.87109375f },
    { 0.87890625f, 0.75390625f, 0.99609375f, 0.87109375f },
    { 0.75390625f, 0.87890625f, 0.87109375f, 0.99609375f },
    { 0.87890625f, 0.87890625f, 0.99609375f, 0.99609375f }
};

// ---------------------------------------------------------------------------
// Pipelines + state
// ---------------------------------------------------------------------------

typedef struct {
    float x, y, z;
    float u, v;
    byte  r, g, b, a;
} part_vert_t;

#define PART_MAX_VERTS  (6 * 4096)    // 4096 particles per frame

static vk_buffer_t       s_part_vbo[MAX_FRAMES_IN_FLIGHT];
static part_vert_t*      s_part_mapped[MAX_FRAMES_IN_FLIGHT];
static uint32_t          s_part_cursor[MAX_FRAMES_IN_FLIGHT];
static qboolean          s_part_ready = false;

static VkPipeline        s_pipeline_alpha    = VK_NULL_HANDLE;  // SRC_ALPHA / ONE_MINUS_SRC_ALPHA
static VkPipeline        s_pipeline_additive = VK_NULL_HANDLE;  // ONE / ONE
static VkPipelineLayout  s_pipeline_layout   = VK_NULL_HANDLE;

static image_t*          s_part_image  = NULL;
static image_t*          s_apart_image = NULL;
static VkDescriptorSet   s_part_desc   = VK_NULL_HANDLE;
static VkDescriptorSet   s_apart_desc  = VK_NULL_HANDLE;

extern const uint32_t spirv_part_vert_data[];
extern const uint32_t spirv_part_vert_size;
extern const uint32_t spirv_part_frag_data[];
extern const uint32_t spirv_part_frag_size;

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

static qboolean CreatePartPipeline(qboolean additive, VkPipeline* out)
{
    VkShaderModule vs = MakeSM(spirv_part_vert_data, spirv_part_vert_size);
    VkShaderModule fs = MakeSM(spirv_part_frag_data, spirv_part_frag_size);
    if (!vs || !fs) return false;

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
    vb.stride    = sizeof(part_vert_t);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription va[3] = {0};
    va[0].location = 0; va[0].format = VK_FORMAT_R32G32B32_SFLOAT;  va[0].offset = 0;
    va[1].location = 1; va[1].format = VK_FORMAT_R32G32_SFLOAT;     va[1].offset = sizeof(float)*3;
    va[2].location = 2; va[2].format = VK_FORMAT_R8G8B8A8_UNORM;    va[2].offset = sizeof(float)*5;

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
    ds.depthWriteEnable = VK_FALSE;   // alpha-blended things don't write depth
    ds.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;  // match GL1 GL_LEQUAL

    VkPipelineColorBlendAttachmentState cba = {0};
    cba.blendEnable    = VK_TRUE;
    cba.colorWriteMask = 0xF;
    cba.colorBlendOp   = VK_BLEND_OP_ADD;
    cba.alphaBlendOp   = VK_BLEND_OP_ADD;
    if (additive) {
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    } else {
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    }

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
    gp.layout              = s_pipeline_layout;
    gp.renderPass          = vk_state.render_pass;

    VkResult r = vkCreateGraphicsPipelines(vk_state.device, VK_NULL_HANDLE, 1, &gp, NULL, out);

    vkDestroyShaderModule(vk_state.device, vs, NULL);
    vkDestroyShaderModule(vk_state.device, fs, NULL);
    return r == VK_SUCCESS;
}

qboolean VK_Particles_Init(void)
{
    // Particle textures.
    s_part_image  = VK_FindPic("misc/particle.m32");
    s_apart_image = VK_FindPic("misc/aparticle.m8");
    if (!s_part_image || !s_apart_image) {
        ri.Con_Printf(PRINT_ALL, "vk: particle textures missing (particle=%p aparticle=%p)\n",
                      s_part_image, s_apart_image);
        // Not fatal; particles just won't draw.
    }
    if (s_part_image)
        s_part_desc = VK_AllocWorldDescriptor(VK_ImageView(s_part_image));
    if (s_apart_image)
        s_apart_desc = VK_AllocWorldDescriptor(VK_ImageView(s_apart_image));

    // Pipeline layout: descriptor set + 64-byte push constant (mat4 mvp).
    VkPushConstantRange pcr = {0};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = 64;

    extern VkDescriptorSetLayout VK_World_GetDescSetLayout(void);
    VkDescriptorSetLayout dsl = vk_pipeline_3d.descriptor_set_layout;

    VkPipelineLayoutCreateInfo pl = {0};
    pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount         = 1;
    pl.pSetLayouts            = &dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(vk_state.device, &pl, NULL, &s_pipeline_layout) != VK_SUCCESS)
        return false;

    if (!CreatePartPipeline(false, &s_pipeline_alpha))    return false;
    if (!CreatePartPipeline(true,  &s_pipeline_additive)) return false;

    // Per-frame VBO.
    const VkDeviceSize sz = sizeof(part_vert_t) * PART_MAX_VERTS;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (!VK_CreateBuffer(sz, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &s_part_vbo[i]))
            return false;
        if (!VK_MapBuffer(&s_part_vbo[i])) return false;
        s_part_mapped[i] = (part_vert_t*)s_part_vbo[i].mapped;
        s_part_cursor[i] = 0;
    }
    s_part_ready = true;
    return true;
}

void VK_Particles_Shutdown(void)
{
    if (s_part_ready) {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
            VK_DestroyBuffer(&s_part_vbo[i]);
        s_part_ready = false;
    }
    if (s_pipeline_alpha)    vkDestroyPipeline(vk_state.device, s_pipeline_alpha,    NULL);
    if (s_pipeline_additive) vkDestroyPipeline(vk_state.device, s_pipeline_additive, NULL);
    if (s_pipeline_layout)   vkDestroyPipelineLayout(vk_state.device, s_pipeline_layout, NULL);
    s_pipeline_alpha = s_pipeline_additive = VK_NULL_HANDLE;
    s_pipeline_layout = VK_NULL_HANDLE;
}

void VK_Particles_BeginFrame(void)
{
    if (!s_part_ready) return;
    s_part_cursor[vk_state.current_frame] = 0;
}

// Push one diamond quad (4 unique verts, 6 indices) into the VBO. Caller has
// already checked there's room.
static void EmitParticle(part_vert_t* vb, uint32_t* cursor,
                         const particle_t* p, const float vup[3], const float vright[3],
                         qboolean additive)
{
    const byte ptype = p->type & PFL_FLAG_MASK;
    const float* st = k_particle_st[(ptype < 62) ? ptype : 0];

    float pup[3]    = { vup[0]*p->scale,    vup[1]*p->scale,    vup[2]*p->scale };
    float pright[3] = { vright[0]*p->scale, vright[1]*p->scale, vright[2]*p->scale };

    byte cr = p->color.r, cg = p->color.g, cb_ = p->color.b, ca = p->color.a;
    if (additive) {
        cr = (byte)((cr * ca) / 255);
        cg = (byte)((cg * ca) / 255);
        cb_ = (byte)((cb_ * ca) / 255);
    }

    // Four corners (diamond layout). Mirrors GL1's R_DrawParticles:
    //   v0: origin + pup       uv (xl, yt)
    //   v1: origin + pright    uv (xr, yt)
    //   v2: origin - pup       uv (xr, yb)
    //   v3: origin - pright    uv (xl, yb)
    float c[4][3], uv[4][2];
    for (int k = 0; k < 3; k++) {
        c[0][k] = p->origin[k] + pup[k];
        c[1][k] = p->origin[k] + pright[k];
        c[2][k] = p->origin[k] - pup[k];
        c[3][k] = p->origin[k] - pright[k];
    }
    uv[0][0]=st[0]; uv[0][1]=st[1];
    uv[1][0]=st[2]; uv[1][1]=st[1];
    uv[2][0]=st[2]; uv[2][1]=st[3];
    uv[3][0]=st[0]; uv[3][1]=st[3];

    // Two triangles: 0,1,2 and 0,2,3
    const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        const int v = idx[i];
        vb[*cursor].x = c[v][0]; vb[*cursor].y = c[v][1]; vb[*cursor].z = c[v][2];
        vb[*cursor].u = uv[v][0]; vb[*cursor].v = uv[v][1];
        vb[*cursor].r = cr; vb[*cursor].g = cg; vb[*cursor].b = cb_; vb[*cursor].a = ca;
        (*cursor)++;
    }
}

static void DrawParticleList(VkCommandBuffer cb, const particle_t* list, int count,
                             const float vup[3], const float vright[3],
                             qboolean additive, const float* mvp,
                             VkDescriptorSet desc, VkPipeline pipe)
{
    if (count <= 0 || !list || desc == VK_NULL_HANDLE) return;
    const uint32_t frame = vk_state.current_frame;
    part_vert_t* vb = s_part_mapped[frame];

    uint32_t cursor = s_part_cursor[frame];
    const uint32_t start = cursor;

    for (int i = 0; i < count; i++) {
        if (cursor + 6 > PART_MAX_VERTS) break;
        EmitParticle(vb, &cursor, &list[i], vup, vright, additive);
    }
    const uint32_t n = cursor - start;
    if (n == 0) return;
    s_part_cursor[frame] = cursor;

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdPushConstants(cb, s_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline_layout, 0, 1, &desc, 0, NULL);
    VkDeviceSize offset = (VkDeviceSize)start * sizeof(part_vert_t);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_part_vbo[frame].buffer, &offset);
    vkCmdDraw(cb, n, 1, 0, 0);
}

void VK_Particles_Render(const refdef_t* fd, const float vup[3], const float vright[3], const float* mvp)
{
    if (!s_part_ready || !fd || !vk_state.frame_started) return;
    const uint32_t frame = vk_state.current_frame;
    VkCommandBuffer cb = vk_state.command_buffers[frame];


    // Normal particles (alpha-blended).
    DrawParticleList(cb, fd->particles, fd->num_particles, vup, vright,
                     false, mvp, s_part_desc, s_pipeline_alpha);

    // Additive particles (glow).
    DrawParticleList(cb, fd->aparticles, fd->anum_particles, vup, vright,
                     true, mvp, s_apart_desc, s_pipeline_additive);

    vk_state.pipeline_2d_bound = false;
}
