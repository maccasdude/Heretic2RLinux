//
// vk_debug.c - developer debug-draw primitives, Vulkan port of ref_gl1
// gl1_Debug.c. The CPU bookkeeping (slot allocation, lifetimes, geometry
// build) mirrors the GL version 1:1; the GL immediate-mode line drawing is
// replaced by a LINE_LIST pipeline fed from a per-frame vertex buffer, and the
// 3D->2D label projection is done against the frame's view-projection matrix.
//
// Like gl1_Debug.c this file compiles in every build; it is only wired into the
// refexport and invoked per frame under _DEBUG (see vk_main.c), matching GL so
// a future _DEBUG build (e.g. a Windows backport) gets like-for-like behavior.
//

#include "vk_local.h"
#include "vk_buffer.h"
#include "vk_draw.h"
#include "vk_debug.h"

#include "Game.h"
#include "Vector.h"
#include "qcommon/Angles.h"
#include "client/vid.h"

#include <string.h>
#include <math.h>

extern viddef_t viddef;   // defined in vk_main.c

#define DEBUG_LABEL_SIZE   64

// External (vk_shaders.c) SPIR-V blobs for the debug line shaders.
extern const uint32_t spirv_debug_vert_data[];
extern const uint32_t spirv_debug_vert_size;
extern const uint32_t spirv_debug_frag_data[];
extern const uint32_t spirv_debug_frag_size;

// ---------------------------------------------------------------------------
// CPU bookkeeping (ported from gl1_Debug.c)
// ---------------------------------------------------------------------------

typedef enum {
    DPT_NONE,
    DPT_LINE,
    DPT_ARROW,
    DPT_MARKER,
    DPT_BOX,
    DPT_BBOX,
    DPT_ENTITY_BBOX,
} DebugPrimitiveType_e;

typedef struct {
    DebugPrimitiveType_e  type;
    vec3_t                verts[8];
    const struct edict_s* ent;
    paletteRGBA_t         color;
    float                 lifetime;
} DebugPrimitive_t;

#define MAX_DEBUG_PRIMITIVES   512
static DebugPrimitive_t dbg_primitives[MAX_DEBUG_PRIMITIVES];

typedef struct {
    char          label[DEBUG_LABEL_SIZE];
    vec3_t        origin;
    paletteRGBA_t color;
    float         lifetime;
} DebugLabel_t;

#define MAX_DEBUG_LABELS   128
static DebugLabel_t dbg_labels[MAX_DEBUG_LABELS];

typedef struct {
    char                  label[DEBUG_LABEL_SIZE];
    const struct edict_s* ent;
    vec3_t                cur_origin;
    vec3_t                old_origin;
    paletteRGBA_t         color;
    float                 last_update;
} DebugEntityLabel_t;

#define MAX_DEBUG_ENTITY_LABELS   128
static DebugEntityLabel_t dbg_ent_labels[MAX_DEBUG_ENTITY_LABELS];

// Current refdef time, captured each frame (GL reads r_newrefdef.time).
static float s_time = 0.0f;

void VK_Debug_SetTime(float time) { s_time = time; }

static void CopyLabel(char* dst, const char* src)
{
    if (!src) { dst[0] = '\0'; return; }
    int i = 0;
    for (; i < DEBUG_LABEL_SIZE - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void SetDebugBoxVerts(DebugPrimitive_t* box, const vec3_t mins, const vec3_t maxs)
{
    VectorSet(box->verts[0], mins[0], mins[1], maxs[2]);
    VectorSet(box->verts[1], mins[0], maxs[1], maxs[2]);
    VectorSet(box->verts[2], maxs[0], maxs[1], maxs[2]);
    VectorSet(box->verts[3], maxs[0], mins[1], maxs[2]);

    VectorSet(box->verts[4], mins[0], mins[1], mins[2]);
    VectorSet(box->verts[5], mins[0], maxs[1], mins[2]);
    VectorSet(box->verts[6], maxs[0], maxs[1], mins[2]);
    VectorSet(box->verts[7], maxs[0], mins[1], mins[2]);
}

static DebugPrimitive_t* InitDebugPrimitive(const struct edict_s* ent, const vec3_t mins, const vec3_t maxs,
                                            const paletteRGBA_t color, const float lifetime, const DebugPrimitiveType_e type)
{
    DebugPrimitive_t* p = NULL;
    for (int i = 0; i < MAX_DEBUG_PRIMITIVES; i++) {
        DebugPrimitive_t* cp = &dbg_primitives[i];
        if (cp->type == DPT_NONE || (cp->lifetime != -1.0f && cp->lifetime < s_time) || (ent != NULL && cp->ent == ent)) {
            p = cp;
            break;
        }
    }
    if (p == NULL)
        return NULL;

    p->type  = type;
    p->color = color;
    p->ent   = ent;

    if (lifetime == -1.0f)
        p->lifetime = lifetime;
    else
        p->lifetime = s_time + max(lifetime, 0.025f);  // absolute, at least a frame

    if (type == DPT_BOX || type == DPT_BBOX || type == DPT_ENTITY_BBOX)
        SetDebugBoxVerts(p, mins, maxs);

    return p;
}

static DebugLabel_t* InitDebugLabel(const vec3_t origin, const paletteRGBA_t color, const float lifetime)
{
    DebugLabel_t* l = NULL;
    for (int i = 0; i < MAX_DEBUG_LABELS; i++) {
        DebugLabel_t* cl = &dbg_labels[i];
        if (VectorCompare(cl->origin, origin) || (cl->lifetime != -1.0f && cl->lifetime < s_time)) {
            l = cl;
            break;
        }
    }
    if (l == NULL)
        return NULL;

    l->color = color;
    VectorCopy(origin, l->origin);

    if (lifetime == -1.0f)
        l->lifetime = lifetime;
    else
        l->lifetime = s_time + max(lifetime, 0.025f);

    return l;
}

static DebugEntityLabel_t* InitDebugEntityLabel(const struct edict_s* ent, const paletteRGBA_t color)
{
    DebugEntityLabel_t* l = NULL;
    for (int i = 0; i < MAX_DEBUG_ENTITY_LABELS; i++)
        if (dbg_ent_labels[i].ent == ent) { l = &dbg_ent_labels[i]; break; }

    if (l == NULL)
        for (int i = 0; i < MAX_DEBUG_ENTITY_LABELS; i++)
            if (dbg_ent_labels[i].ent == NULL) { l = &dbg_ent_labels[i]; break; }

    if (l == NULL)
        return NULL;

    l->color = color;
    if (l->ent != ent) {
        l->ent = ent;
        l->last_update = s_time;
        VectorCopy(ent->s.origin, l->cur_origin);
        VectorCopy(ent->s.origin, l->old_origin);
    }
    return l;
}

void RI_AddDebugBox(const vec3_t center, float size, const paletteRGBA_t color, const float lifetime)
{
    size *= 0.5f;
    const vec3_t mins = { center[0] - size, center[1] - size, center[2] - size };
    const vec3_t maxs = { center[0] + size, center[1] + size, center[2] + size };
    if (InitDebugPrimitive(NULL, mins, maxs, color, lifetime, DPT_BOX) == NULL)
        ri.Con_Printf(PRINT_DEVELOPER, "RI_AddDebugBox: no free slot\n");
}

void RI_AddDebugBbox(const vec3_t mins, const vec3_t maxs, const paletteRGBA_t color, const float lifetime)
{
    if (InitDebugPrimitive(NULL, mins, maxs, color, lifetime, DPT_BBOX) == NULL)
        ri.Con_Printf(PRINT_DEVELOPER, "RI_AddDebugBbox: no free slot\n");
}

void RI_AddDebugEntityBbox(const struct edict_s* ent, const paletteRGBA_t color)
{
    if (ent == NULL)
        return;
    if (InitDebugPrimitive(ent, ent->mins, ent->maxs, color, -1.0f, DPT_ENTITY_BBOX) == NULL)
        ri.Con_Printf(PRINT_DEVELOPER, "RI_AddDebugEntityBbox: no free slot\n");
}

void RI_AddDebugLabel(const vec3_t origin, const paletteRGBA_t color, const float lifetime, const char* label)
{
    DebugLabel_t* l = InitDebugLabel(origin, color, lifetime);
    if (l != NULL)
        CopyLabel(l->label, label);
    else
        ri.Con_Printf(PRINT_DEVELOPER, "RI_AddDebugLabel: no free slot\n");
}

void RI_AddDebugEntityLabel(const struct edict_s* ent, const paletteRGBA_t color, const char* label)
{
    if (ent == NULL)
        return;
    DebugEntityLabel_t* l = InitDebugEntityLabel(ent, color);
    if (l != NULL)
        CopyLabel(l->label, label);
    else
        ri.Con_Printf(PRINT_DEVELOPER, "RI_AddDebugEntityLabel: no free slot\n");
}

void RI_AddDebugLine(const vec3_t start, const vec3_t end, const paletteRGBA_t color, const float lifetime)
{
    DebugPrimitive_t* line = InitDebugPrimitive(NULL, start, end, color, lifetime, DPT_LINE);
    if (line != NULL) {
        VectorCopy(start, line->verts[0]);
        VectorCopy(end,   line->verts[1]);
    } else {
        ri.Con_Printf(PRINT_DEVELOPER, "RI_AddDebugLine: no free slot\n");
    }
}

void RI_AddDebugArrow(const vec3_t start, const vec3_t end, const paletteRGBA_t color, const float lifetime)
{
#define ARROWHEAD_SIZE  8.0f
    DebugPrimitive_t* arrow = InitDebugPrimitive(NULL, start, end, color, lifetime, DPT_ARROW);
    if (arrow == NULL) {
        ri.Con_Printf(PRINT_DEVELOPER, "RI_AddDebugArrow: no free slot\n");
        return;
    }

    VectorCopy(start, arrow->verts[0]);
    VectorCopy(end,   arrow->verts[1]);

    vec3_t dir;
    VectorSubtract(end, start, dir);
    float len = VectorNormalize(dir) * 0.5f;
    len = min(len, ARROWHEAD_SIZE);

    vec3_t up;
    PerpendicularVector(up, dir);

    vec3_t right;
    CrossProduct(up, dir, right);

    vec3_t arrowhead_pos;
    VectorMA(end, -len, dir, arrowhead_pos);

    float angle = ANGLE_45;
    for (int i = 0; i < 4; i++) {
        vec3_t v;
        VectorScale(up, sinf(angle) * len * 0.5f, v);
        VectorMA(v, cosf(angle) * len * 0.5f, right, v);
        VectorSubtract(arrowhead_pos, v, arrow->verts[i + 2]);
        angle += ANGLE_90;
    }
}

void RI_AddDebugDirection(const vec3_t start, const vec3_t direction, const float size, const paletteRGBA_t color, const float lifetime)
{
    vec3_t normal;
    VectorNormalize2(direction, normal);
    vec3_t end;
    VectorMA(start, size, normal, end);
    RI_AddDebugArrow(start, end, color, lifetime);
}

void RI_AddDebugAngles(const vec3_t start, const vec3_t angles_deg, const float size, const paletteRGBA_t color, const float lifetime)
{
    vec3_t angles;
    VectorScale(angles_deg, ANGLE_TO_RAD, angles);
    RI_AddDebugAnglesRad(start, angles, size, color, lifetime);
}

void RI_AddDebugAnglesRad(const vec3_t start, const vec3_t angles_rad, const float size, const paletteRGBA_t color, const float lifetime)
{
    vec3_t direction;
    DirFromAngles(angles_rad, direction);
    RI_AddDebugDirection(start, direction, size, color, lifetime);
}

void RI_AddDebugMarker(const vec3_t center, const float size, const paletteRGBA_t color, const float lifetime)
{
    DebugPrimitive_t* marker = InitDebugPrimitive(NULL, NULL, NULL, color, lifetime, DPT_MARKER);
    if (marker == NULL) {
        ri.Con_Printf(PRINT_DEVELOPER, "RI_AddDebugMarker: no free slot\n");
        return;
    }
    const float hs = size * 0.5f;
    VectorSet(marker->verts[0], center[0] + hs, center[1], center[2]);
    VectorSet(marker->verts[1], center[0] - hs, center[1], center[2]);
    VectorSet(marker->verts[2], center[0], center[1] + hs, center[2]);
    VectorSet(marker->verts[3], center[0], center[1] - hs, center[2]);
    VectorSet(marker->verts[4], center[0], center[1], center[2] + hs);
    VectorSet(marker->verts[5], center[0], center[1], center[2] - hs);
}

// ---------------------------------------------------------------------------
// Vulkan line pipeline + per-frame vertex buffers
// ---------------------------------------------------------------------------

typedef struct { float pos[3]; byte color[4]; } debug_vert_t;

#define DEBUG_MAX_VERTS   16384

static VkPipeline       s_pipeline      = VK_NULL_HANDLE;
static VkPipelineLayout s_layout        = VK_NULL_HANDLE;
static qboolean         s_pipe_failed   = false;
static vk_buffer_t      s_vbo[MAX_FRAMES_IN_FLIGHT];
static qboolean         s_vbo_ready[MAX_FRAMES_IN_FLIGHT];

static debug_vert_t     s_verts[DEBUG_MAX_VERTS];   // CPU scratch, filled per frame
static int              s_num_verts = 0;

static VkShaderModule MakeSM(const uint32_t* code, size_t size)
{
    VkShaderModuleCreateInfo ci = {0};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size;
    ci.pCode    = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(vk_state.device, &ci, NULL, &m);
    return m;
}

static qboolean EnsurePipeline(void)
{
    if (s_pipeline != VK_NULL_HANDLE) return true;
    if (s_pipe_failed) return false;

    // Layout: no descriptor sets, one push constant (mat4 mvp) for the vertex stage.
    VkPushConstantRange pcr = {0};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = 64;   // mat4

    VkPipelineLayoutCreateInfo pl = {0};
    pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(vk_state.device, &pl, NULL, &s_layout) != VK_SUCCESS) {
        s_pipe_failed = true;
        return false;
    }

    VkShaderModule vs = MakeSM(spirv_debug_vert_data, spirv_debug_vert_size);
    VkShaderModule fs = MakeSM(spirv_debug_frag_data, spirv_debug_frag_size);
    if (!vs || !fs) { s_pipe_failed = true; return false; }

    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs; stages[1].pName = "main";

    VkVertexInputBindingDescription vb = {0};
    vb.binding = 0; vb.stride = sizeof(debug_vert_t); vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription va[2] = {0};
    va[0].location = 0; va[0].binding = 0; va[0].format = VK_FORMAT_R32G32B32_SFLOAT; va[0].offset = 0;
    va[1].location = 1; va[1].binding = 0; va[1].format = VK_FORMAT_R8G8B8A8_UNORM;   va[1].offset = sizeof(float) * 3;

    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &vb;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions    = va;

    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    VkPipelineViewportStateCreateInfo vp = {0};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test OFF (ref_gl1 disables it for debug primitives so they show
    // through geometry); the renderpass has a depth attachment so a state is
    // still required.
    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cba = {0};
    cba.blendEnable    = VK_FALSE;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb = {0};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {0};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gp = {0};
    gp.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount          = 2; gp.pStages = stages;
    gp.pVertexInputState   = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState      = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState   = &ms;
    gp.pDepthStencilState  = &ds;
    gp.pColorBlendState    = &cb;
    gp.pDynamicState       = &dyn;
    gp.layout              = s_layout;
    gp.renderPass          = vk_state.render_pass;
    gp.subpass             = 0;

    VkResult r = vkCreateGraphicsPipelines(vk_state.device, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipeline);
    vkDestroyShaderModule(vk_state.device, vs, NULL);
    vkDestroyShaderModule(vk_state.device, fs, NULL);

    if (r != VK_SUCCESS) { s_pipe_failed = true; return false; }
    return true;
}

static vk_buffer_t* EnsureVBO(uint32_t frame)
{
    if (s_vbo_ready[frame]) return &s_vbo[frame];
    if (!VK_CreateBuffer(sizeof(debug_vert_t) * DEBUG_MAX_VERTS,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &s_vbo[frame]))
        return NULL;
    if (!VK_MapBuffer(&s_vbo[frame])) { VK_DestroyBuffer(&s_vbo[frame]); return NULL; }
    s_vbo_ready[frame] = true;
    return &s_vbo[frame];
}

static void EmitVert(const vec3_t p, const paletteRGBA_t c)
{
    if (s_num_verts >= DEBUG_MAX_VERTS) return;
    debug_vert_t* v = &s_verts[s_num_verts++];
    v->pos[0] = p[0]; v->pos[1] = p[1]; v->pos[2] = p[2];
    v->color[0] = c.r; v->color[1] = c.g; v->color[2] = c.b; v->color[3] = 255;
}

static void EmitSeg(const vec3_t a, const vec3_t b, const paletteRGBA_t c) { EmitVert(a, c); EmitVert(b, c); }

static void EmitBox(const DebugPrimitive_t* box)
{
    // Top + bottom loops.
    for (int i = 0; i < 4; i++) {
        EmitSeg(box->verts[i],     box->verts[(i + 1) % 4],         box->color);
        EmitSeg(box->verts[i + 4], box->verts[4 + (i + 1) % 4],     box->color);
    }
    // Vertical edges.
    for (int i = 0; i < 4; i++)
        EmitSeg(box->verts[i], box->verts[i + 4], box->color);
}

static void EmitArrow(const DebugPrimitive_t* a)
{
    EmitSeg(a->verts[0], a->verts[1], a->color);   // shaft
    for (int i = 2; i < 6; i++)
        EmitSeg(a->verts[i], a->verts[1], a->color); // head
}

static void EmitMarker(const DebugPrimitive_t* m)
{
    for (int i = 0; i < 6; i += 2)
        EmitSeg(m->verts[i], m->verts[i + 1], m->color);
}

void VK_Debug_DrawPrimitives(const struct refdef_s* fd, const float mvp[16])
{
    if (!fd || !vk_state.frame_started) return;
    if (!EnsurePipeline()) return;

    s_num_verts = 0;

    DebugPrimitive_t* p = &dbg_primitives[0];
    for (int i = 0; i < MAX_DEBUG_PRIMITIVES; i++, p++) {
        if (p->type != DPT_NONE && p->type != DPT_ENTITY_BBOX &&
            p->lifetime > -1.0f && p->lifetime < s_time)
            p->type = DPT_NONE;

        if (p->type == DPT_NONE)
            continue;

        switch (p->type) {
            case DPT_ENTITY_BBOX: {
                if (p->ent == NULL) { p->type = DPT_NONE; continue; }
                vec3_t mins, maxs;
                VectorAdd(p->ent->s.origin, p->ent->mins, mins);
                VectorAdd(p->ent->s.origin, p->ent->maxs, maxs);
                SetDebugBoxVerts(p, mins, maxs);
                EmitBox(p);
                break;
            }
            case DPT_BOX:
            case DPT_BBOX:   EmitBox(p);    break;
            case DPT_LINE:   EmitSeg(p->verts[0], p->verts[1], p->color); break;
            case DPT_ARROW:  EmitArrow(p);  break;
            case DPT_MARKER: EmitMarker(p); break;
            default: break;
        }
    }

    if (s_num_verts < 2)
        return;

    const uint32_t frame = vk_state.current_frame;
    vk_buffer_t* vbo = EnsureVBO(frame);
    if (!vbo || !vbo->mapped)
        return;

    memcpy(vbo->mapped, s_verts, sizeof(debug_vert_t) * s_num_verts);

    VkCommandBuffer cb = vk_state.command_buffers[frame];

    VkViewport vp = {0};
    vp.x = (float)fd->x; vp.y = (float)fd->y;
    vp.width = (float)fd->width; vp.height = (float)fd->height;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    VkRect2D sc = {0};
    sc.offset.x = fd->x; sc.offset.y = fd->y;
    sc.extent.width = (uint32_t)fd->width; sc.extent.height = (uint32_t)fd->height;
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
    vkCmdPushConstants(cb, s_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp);

    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vbo->buffer, &zero);
    vkCmdDraw(cb, (uint32_t)s_num_verts, 1, 0, 0);
}

// ---------------------------------------------------------------------------
// Labels (3D world point -> 2D screen, then conchar text)
// ---------------------------------------------------------------------------

// Project p through the column-major mvp into screen pixels. Returns false if
// behind the camera. out[2] is the Vulkan depth (0..1) for a frustum check.
static qboolean PointToScreen(const float mvp[16], const vec3_t p, vec3_t out)
{
    float c[4];
    for (int r = 0; r < 4; r++)
        c[r] = mvp[0 * 4 + r] * p[0] + mvp[1 * 4 + r] * p[1] + mvp[2 * 4 + r] * p[2] + mvp[3 * 4 + r];
    if (c[3] <= 0.0001f)
        return false;
    const float inv = 1.0f / c[3];
    out[0] = (c[0] * inv * 0.5f + 0.5f) * (float)viddef.width;
    out[1] = (c[1] * inv * 0.5f + 0.5f) * (float)viddef.height;  // proj has -f in [5]: NDC y is already down
    out[2] = c[2] * inv;
    return true;
}

static void DrawLabelAt(const float mvp[16], const vec3_t pos, const paletteRGBA_t color, const char* label)
{
    vec3_t sp;
    if (!PointToScreen(mvp, pos, sp) || sp[2] <= 0.0f || sp[2] > 1.0f)
        return;

    int ui_scale = (int)(roundf((float)viddef.width / DEF_WIDTH));
    const int sy_scale = (int)(roundf((float)viddef.height / DEF_HEIGHT));
    if (sy_scale < ui_scale) ui_scale = sy_scale;
    if (ui_scale < 1) ui_scale = 1;
    const int ui_char_size = CONCHAR_SIZE * ui_scale;

    const int len = (int)strlen(label);
    const int ui_len = len * ui_char_size;

    const int sx = (int)sp[0] - ui_len / 2;
    const int ex = (int)sp[0] + ui_len / 2;
    const int sy = (int)sp[1] - ui_char_size / 2;
    const int ey = (int)sp[1] + ui_char_size / 2;

    if (sx >= viddef.width || ex <= 0 || sy >= viddef.height || ey <= 0)
        return;

    int x = sx;
    for (int i = 0; i < len; i++, x += ui_char_size)
        VK_Draw_Char(x, sy, ui_scale, label[i], color, true);
}

void VK_Debug_DrawLabels(const struct refdef_s* fd, const float mvp[16])
{
    if (!fd) return;

    const DebugLabel_t* l = &dbg_labels[0];
    for (int i = 0; i < MAX_DEBUG_LABELS; i++, l++)
        if (l->label[0] && (l->lifetime == -1.0f || l->lifetime >= s_time))
            DrawLabelAt(mvp, l->origin, l->color, l->label);

    DebugEntityLabel_t* el = &dbg_ent_labels[0];
    for (int i = 0; i < MAX_DEBUG_ENTITY_LABELS; i++, el++) {
        if (el->ent != NULL && el->ent->inuse) {
            // Interpolate position so labels on moving entities aren't twitchy.
            float delta = s_time - el->last_update;
            if (delta >= 0.2f) {
                el->last_update = s_time;
                VectorCopy(el->cur_origin, el->old_origin);
                VectorCopy(el->ent->s.origin, el->cur_origin);
                delta = 0.0f;
            }
            vec3_t label_pos;
            VectorLerp(el->old_origin, delta * 5.0f, el->cur_origin, label_pos);
            label_pos[2] += 32.0f;
            DrawLabelAt(mvp, label_pos, el->color, el->label);
        } else {
            el->ent = NULL;
        }
    }
}

void VK_Debug_Free(void)
{
    memset(dbg_primitives, 0, sizeof(dbg_primitives));
    memset(dbg_labels,     0, sizeof(dbg_labels));
    memset(dbg_ent_labels, 0, sizeof(dbg_ent_labels));
}

void VK_Debug_Shutdown(void)
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (s_vbo_ready[i]) { VK_DestroyBuffer(&s_vbo[i]); s_vbo_ready[i] = false; }
    }
    if (s_pipeline) { vkDestroyPipeline(vk_state.device, s_pipeline, NULL); s_pipeline = VK_NULL_HANDLE; }
    if (s_layout)   { vkDestroyPipelineLayout(vk_state.device, s_layout, NULL); s_layout = VK_NULL_HANDLE; }
    s_pipe_failed = false;
}
