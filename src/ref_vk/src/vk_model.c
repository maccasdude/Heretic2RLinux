//
// vk_model.c - H2 flex model (.fm) loading + rendering (non-skeletal path).
//
// The .fm format is a block-based container very similar to Quake2's MD2:
//   "header"     - fmheader_t (counts, framesize, skin dims)
//   "skin"       - skin name strings
//   "st coord"   - texture coords (unused by us; glcmds carry their own)
//   "tris"       - triangle indices (unused by us; we use glcmds)
//   "frames"     - per-frame compressed vertex positions (byte xyz * scale + translate)
//   "glcmds"     - GL draw-list: [count][s,t,vertidx]... ; +count=strip, -count=fan, 0=end
//   "mesh nodes" - per-mesh glcmd ranges
//   (skeleton/references/comp blocks - skipped for now; skeletal path TODO)
//
// Rendering approach (CPU skinning): decode current+old frame, lerp vertex
// positions on the CPU, walk the glcmds emitting a triangle list with (pos,uv),
// stream into a per-frame dynamic vertex buffer, draw with the model's skin.
//

#include "vk_model.h"
#include "vk_engine_model.h"
#include "vk_local.h"
#include "vk_buffer.h"
#include "vk_image.h"
#include "vk_pipeline3d.h"
#include "vk_pipeline_world.h"
#include "vk_skeleton.h"
#include "vk_lightpoint.h"

#include "qcommon/FlexModel.h"
#include "qcommon/qfiles.h"
#include "qcommon/anorms.h"
#include "client/ref.h"

// These names are renderer-side constants from gl1_FlexModel.h. We replicate
// them here so we don't have to drag in GL1 headers.
#define FM_SKIN_NAME    "skin"
#define FM_FRAME_NAME   "frames"
#define FM_GLCMDS_NAME  "glcmds"
#define FM_MESH_NAME    "mesh nodes"
#define MAX_FM_TRIANGLES 2048
#define MAX_FM_VERTS     2048

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VKM_MAX_MODELS      512
#define VKM_MAX_VERTS       4096

// .fm block idents not present in qcommon/FlexModel.h (they live in ref_gl1's
// gl1_FlexModel.h). Defined here so the parser can recognize them.
#ifndef FM_SKELETON_NAME
#define FM_SKELETON_NAME    "skeleton"
#endif
#ifndef FM_REFERENCES_NAME
#define FM_REFERENCES_NAME  "references"
#endif
#define VKM_MAX_GLCMDS      16384
#define VKM_MAX_FRAMES_MEM  (16 * 1024 * 1024)   // per-model frame data cap
#define VKM_MAX_MESH_NODES  64
#define VKM_MAX_SKINS       32

// On-disk frame vertex: byte xyz + normal index.
typedef struct { byte v[3]; byte n; } fm_trivert_t;
// On-disk frame header (variable-size verts follow).
typedef struct {
    float scale[3];
    float translate[3];
    char  name[16];
    fm_trivert_t verts[1];
} fm_frame_t;

typedef struct { short start_glcmds, num_glcmds; } vkm_mesh_t;

struct vk_model_s {
    char       name[256];
    qboolean   used;
    qboolean   valid;

    fmheader_t header;

    byte*      frames;        // raw frame block (header.num_frames * framesize)
    int*       glcmds;        // glcmd dwords
    int        num_glcmds;

    vkm_mesh_t mesh_nodes[VKM_MAX_MESH_NODES];
    int        num_mesh_nodes;

    image_t*        skins[VKM_MAX_SKINS];
    VkDescriptorSet skin_desc[VKM_MAX_SKINS];
    int             num_skins;

    // Skeletal/reference data (players, monsters with bend/refs). Parsed from
    // the "skeleton" and "references" .fm blocks. ref_type < 0 means none.
    vk_model_skel_t skel;
    qboolean        has_skel;
};

typedef struct { float x,y,z; float u,v; float nx,ny,nz; } vkm_vert_t;

static struct vk_model_s s_models[VKM_MAX_MODELS];
static int               s_num_models = 0;

// Pre-lerped vertex scratch for the skeletal path. GL1 lerps all verts into
// s_lerped[], optionally rotates cluster verts by joint angles, then the glcmd
// loop reads from it. Sized to hold full verts + packed reference/skel slots.
static vec3_t s_lerped[VKM_MAX_VERTS];

// Current refdef time (r_newrefdef.time equivalent), set each frame. Used by
// the skeletal reference lerp to stamp referenceInfo->lastUpdate.
static float s_refdef_time = 0.0f;
static float s_view_origin[3] = {0,0,0};

// Underwater fog cvars (shared with vk_world / ref_gl1 R_WaterFog).
static cvar_t* s_uw_cam_m     = NULL;
static cvar_t* s_uw_mode_m    = NULL;
static cvar_t* s_uw_density_m = NULL;
static cvar_t* s_uw_start_m   = NULL;
static cvar_t* s_uw_col_r_m   = NULL;
static cvar_t* s_uw_col_g_m   = NULL;
static cvar_t* s_uw_col_b_m   = NULL;
static cvar_t* s_farclip_m    = NULL;
// Global level fog (r_fog), matching vk_world.c. Set per-level (Darkmire etc).
static cvar_t* s_fog_m        = NULL;
static cvar_t* s_fog_mode_m   = NULL;
static cvar_t* s_fog_density_m= NULL;
static cvar_t* s_fog_start_m  = NULL;
static cvar_t* s_fog_col_r_m  = NULL;
static cvar_t* s_fog_col_g_m  = NULL;
static cvar_t* s_fog_col_b_m  = NULL;

// Fill the 12-float fog tail (3 vec4) of an entity push constant at offset 20
// (after mat4 + vec4 tint). Entity verts are pre-transformed to world space, so
// the fog camera is just the view origin.
static void Model_FillFogParams(float* pc /* >=32 floats */)
{
    if (!s_uw_cam_m)     s_uw_cam_m     = ri.Cvar_Get("cl_camera_under_surface", "0", 0);
    if (!s_uw_mode_m)    s_uw_mode_m    = ri.Cvar_Get("r_fog_underwater_mode", "1", 0);
    if (!s_uw_density_m) s_uw_density_m = ri.Cvar_Get("r_fog_underwater_density", "0.0015", 0);
    if (!s_uw_start_m)   s_uw_start_m   = ri.Cvar_Get("r_fog_underwater_startdist", "100.0", 0);
    if (!s_uw_col_r_m)   s_uw_col_r_m   = ri.Cvar_Get("r_fog_underwater_color_r", "1.0", 0);
    if (!s_uw_col_g_m)   s_uw_col_g_m   = ri.Cvar_Get("r_fog_underwater_color_g", "1.0", 0);
    if (!s_uw_col_b_m)   s_uw_col_b_m   = ri.Cvar_Get("r_fog_underwater_color_b", "1.0", 0);
    if (!s_farclip_m)    s_farclip_m    = ri.Cvar_Get("r_farclipdist", "4096.0", 0);
    if (!s_fog_m)         s_fog_m         = ri.Cvar_Get("r_fog", "0", 0);
    if (!s_fog_mode_m)    s_fog_mode_m    = ri.Cvar_Get("r_fog_mode", "1", 0);
    if (!s_fog_density_m) s_fog_density_m = ri.Cvar_Get("r_fog_density", "0.004", 0);
    if (!s_fog_start_m)   s_fog_start_m   = ri.Cvar_Get("r_fog_startdist", "50.0", 0);
    if (!s_fog_col_r_m)   s_fog_col_r_m   = ri.Cvar_Get("r_fog_color_r", "1.0", 0);
    if (!s_fog_col_g_m)   s_fog_col_g_m   = ri.Cvar_Get("r_fog_color_g", "1.0", 0);
    if (!s_fog_col_b_m)   s_fog_col_b_m   = ri.Cvar_Get("r_fog_color_b", "1.0", 0);

    const qboolean underwater = (s_uw_cam_m && s_uw_cam_m->value != 0.0f);
    const qboolean globalfog  = !underwater && (s_fog_m && s_fog_m->value != 0.0f);

    // fog_cam @20 = (camX, camY, camZ, density)
    pc[20] = s_view_origin[0]; pc[21] = s_view_origin[1]; pc[22] = s_view_origin[2];

    // fog_color @24 = (r, g, b, mode)  mode <0 => off
    if (underwater) {
        int mode = s_uw_mode_m ? (int)s_uw_mode_m->value : 1;
        if (mode < 0) mode = 0; if (mode > 2) mode = 2;
        pc[23] = s_uw_density_m ? s_uw_density_m->value : 0.0015f;
        pc[24] = s_uw_col_r_m ? s_uw_col_r_m->value : 1.0f;
        pc[25] = s_uw_col_g_m ? s_uw_col_g_m->value : 1.0f;
        pc[26] = s_uw_col_b_m ? s_uw_col_b_m->value : 1.0f;
        pc[27] = (float)mode;
        pc[28] = s_uw_start_m ? s_uw_start_m->value : 100.0f;
    } else if (globalfog) {
        int mode = s_fog_mode_m ? (int)s_fog_mode_m->value : 1;
        if (mode < 0) mode = 0; if (mode > 2) mode = 2;
        pc[23] = s_fog_density_m ? s_fog_density_m->value : 0.004f;
        pc[24] = s_fog_col_r_m ? s_fog_col_r_m->value : 1.0f;
        pc[25] = s_fog_col_g_m ? s_fog_col_g_m->value : 1.0f;
        pc[26] = s_fog_col_b_m ? s_fog_col_b_m->value : 1.0f;
        pc[27] = (float)mode;
        pc[28] = s_fog_start_m ? s_fog_start_m->value : 50.0f;
    } else {
        pc[23] = 0.0015f;
        pc[24] = pc[25] = pc[26] = 0.0f;
        pc[27] = -1.0f;
        pc[28] = 100.0f;
    }

    // fog_extra @28 = (startdist, farclip, dlight_enable, 0)
    pc[29] = s_farclip_m  ? s_farclip_m->value  : 4096.0f;
    pc[30] = 1.0f;   // enable dynamic lights on models
    pc[31] = 0.0f;
}

// Per-frame dynamic vertex buffer for streamed model geometry.
static vk_buffer_t s_model_vbo[MAX_FRAMES_IN_FLIGHT];
static vkm_vert_t* s_model_mapped[MAX_FRAMES_IN_FLIGHT];
static uint32_t    s_model_cursor[MAX_FRAMES_IN_FLIGHT];
static qboolean    s_model_vbo_ready = false;

// ---------------------------------------------------------------------------
// Dynamic VBO lifecycle
// ---------------------------------------------------------------------------

static qboolean EnsureModelVBO(void)
{
    if (s_model_vbo_ready) return true;
    const VkDeviceSize sz = sizeof(vkm_vert_t) * VKM_MAX_VERTS * 64; // room for many models/frame
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (!VK_CreateBuffer(sz, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &s_model_vbo[i]))
            return false;
        if (!VK_MapBuffer(&s_model_vbo[i])) return false;
        s_model_mapped[i] = (vkm_vert_t*)s_model_vbo[i].mapped;
        s_model_cursor[i] = 0;
    }
    s_model_vbo_ready = true;
    return true;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

static struct vk_model_s* FindCached(const char* name)
{
    for (int i = 0; i < s_num_models; i++)
        if (s_models[i].used && strcasecmp(s_models[i].name, name) == 0)
            return &s_models[i];
    return NULL;
}

static struct vk_model_s* AllocSlot(void)
{
    for (int i = 0; i < VKM_MAX_MODELS; i++)
        if (!s_models[i].used) {
            if (i >= s_num_models) s_num_models = i + 1;
            memset(&s_models[i], 0, sizeof(s_models[i]));
            s_models[i].used = true;
            return &s_models[i];
        }
    return NULL;
}

vk_model_t* VK_Model_Register(const char* name)
{
    if (!name || !*name) return NULL;
    struct vk_model_s* hit = FindCached(name);
    if (hit) return hit;

    byte* buffer = NULL;
    int length = ri.FS_LoadFile(name, (void**)&buffer);
    if (!buffer || length < (int)sizeof(fmdl_blockheader_t)) {
        if (buffer) ri.FS_FreeFile(buffer);
        return NULL;
    }

    // Must start with a "header" block.
    if (length <= 6 || Q_strncasecmp((char*)buffer, "header", 6) != 0) {
        ri.FS_FreeFile(buffer);
        return NULL;   // Not a flex model (could be .sp2, .bk, etc.)
    }

    struct vk_model_s* m = AllocSlot();
    if (!m) { ri.FS_FreeFile(buffer); return NULL; }
    strcpy_s(m->name, sizeof(m->name), name);

    // Skeletal defaults: 0 is a valid SKEL_/REF_ type, so init to -1 (none).
    m->skel.skeletalType  = SKEL_NULL;
    m->skel.rootCluster   = -1;
    m->skel.referenceType = REF_NULL;

    byte* in = buffer;
    int   remaining = length;

    while (remaining > (int)sizeof(fmdl_blockheader_t)) {
        fmdl_blockheader_t* bh = (fmdl_blockheader_t*)in;
        byte* data = in + sizeof(fmdl_blockheader_t);
        const int bsize = bh->size;

        if (strcasecmp(bh->ident, FM_HEADER_NAME) == 0) {
            memcpy(&m->header, data, sizeof(fmheader_t));
        } else if (strcasecmp(bh->ident, FM_SKIN_NAME) == 0) {
            // Skin names are stored as fixed-width MAX_FRAMENAME (64) byte
            // strings, NOT FMDL_BLOCK_IDENT_SIZE (32). Using the wrong stride
            // reads later skins from the wrong offset (showing damaged/bloody
            // variants instead of the base skin). Matches ref_gl1 fmLoadSkin.
            const int n = m->header.num_skins;
            const char* sn = (const char*)data;
            for (int i = 0; i < n && i < VKM_MAX_SKINS; i++, sn += MAX_FRAMENAME) {
                m->skins[i] = VK_FindImage(sn);
                if (m->skins[i])
                    m->skin_desc[i] = VK_AllocWorldDescriptor(VK_ImageView(m->skins[i]));
            }
            m->num_skins = (n < VKM_MAX_SKINS) ? n : VKM_MAX_SKINS;
        } else if (strcasecmp(bh->ident, FM_FRAME_NAME) == 0) {
            m->frames = malloc(bsize);
            if (m->frames) memcpy(m->frames, data, bsize);
        } else if (strcasecmp(bh->ident, FM_GLCMDS_NAME) == 0) {
            m->num_glcmds = bsize / (int)sizeof(int);
            m->glcmds = malloc(bsize);
            if (m->glcmds) memcpy(m->glcmds, data, bsize);
        } else if (strcasecmp(bh->ident, FM_MESH_NAME) == 0) {
            // mesh node block: layout has unused arrays then start/num glcmds.
            // Stride per node = header.framesize won't help; the GL1 struct is
            // fmmeshnode_t with 256+256 unused bytes + 2 shorts = 516 bytes.
            const int node_stride = (MAX_FM_TRIANGLES >> 3) + (MAX_FM_VERTS >> 3) + 2 * (int)sizeof(short);
            int n = m->header.num_mesh_nodes;
            if (n > VKM_MAX_MESH_NODES) n = VKM_MAX_MESH_NODES;
            for (int i = 0; i < n; i++) {
                const byte* node = data + i * node_stride;
                const short* sc = (const short*)(node + (MAX_FM_TRIANGLES >> 3) + (MAX_FM_VERTS >> 3));
                m->mesh_nodes[i].start_glcmds = sc[0];
                m->mesh_nodes[i].num_glcmds   = sc[1];
            }
            m->num_mesh_nodes = n;
        } else if (strcasecmp(bh->ident, FM_SKELETON_NAME) == 0) {
            // Skeletal cluster data + optional per-frame joint placements. May
            // reduce num_xyz when the skeleton is packed inline into the verts.
            m->skel.num_xyz_full = m->header.num_xyz;
            VK_Skel_ParseSkeleton(&m->skel, data, m->header.num_frames,
                                  m->header.framesize, &m->header.num_xyz);
            m->has_skel = true;
        } else if (strcasecmp(bh->ident, FM_REFERENCES_NAME) == 0) {
            // Reference placements (hand/foot/staff points). May reduce num_xyz
            // when refs are packed inline into the verts.
            if (m->skel.num_xyz_full == 0) m->skel.num_xyz_full = m->header.num_xyz;
            VK_Skel_ParseReferences(&m->skel, data, m->header.num_frames, &m->header.num_xyz);
            m->has_skel = true;
        }
        // Other blocks (st coord, tris, skeleton, references, comp, normals,
        // short frames) are skipped for the non-skeletal path.

        in += sizeof(fmdl_blockheader_t) + bsize;
        remaining -= (int)sizeof(fmdl_blockheader_t) + bsize;
    }

    ri.FS_FreeFile(buffer);

    // Finalize skeletal bookkeeping. num_xyz may have been reduced by inline
    // skeleton/reference data; the reduced value is what we render, and packed
    // refs (if any) live at indices >= num_xyz_render in each frame's verts.
    if (m->has_skel) {
        m->skel.num_xyz_render = m->header.num_xyz;
        if (m->skel.num_xyz_full == 0) m->skel.num_xyz_full = m->header.num_xyz;
        m->skel.num_frames = m->header.num_frames;
        ri.Con_Printf(PRINT_ALL,
            "vk: model '%s' skeletal: skelType=%d refType=%d haveSkel=%d haveRefs=%d xyz %d->%d\n",
            name, m->skel.skeletalType, m->skel.referenceType,
            m->skel.haveSkeleton, m->skel.haveRefs,
            m->skel.num_xyz_full, m->skel.num_xyz_render);
    }

    // Validate the minimum we need to draw.
    if (!m->frames || !m->glcmds || m->header.num_frames <= 0) {
        ri.Con_Printf(PRINT_ALL, "vk: flex model '%s' missing frames/glcmds\n", name);
        m->valid = false;
        return m;       // keep cached as invalid so we don't retry
    }

    // If no explicit mesh nodes, synthesize one covering all glcmds.
    if (m->num_mesh_nodes == 0) {
        m->mesh_nodes[0].start_glcmds = 0;
        m->mesh_nodes[0].num_glcmds   = m->num_glcmds;
        m->num_mesh_nodes = 1;
    }

    m->valid = true;
    ri.Con_Printf(PRINT_ALL, "vk: flex model '%s': %d frames, %d xyz, %d glcmds, %d skins\n",
                  name, m->header.num_frames, m->header.num_xyz, m->num_glcmds, m->num_skins);
    return m;
}

void VK_Model_FreeAll(void)
{
    for (int i = 0; i < s_num_models; i++) {
        if (!s_models[i].used) continue;
        free(s_models[i].frames);
        free(s_models[i].glcmds);
        if (s_models[i].has_skel)
            VK_Skel_FreeModel(&s_models[i].skel);
    }
    memset(s_models, 0, sizeof(s_models));
    s_num_models = 0;

    if (s_model_vbo_ready) {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
            VK_DestroyBuffer(&s_model_vbo[i]);
        s_model_vbo_ready = false;
    }
}

int VK_Model_ReferenceType(const vk_model_t* m)
{
    if (!m || !m->valid || !m->has_skel) return -1;
    return VK_Skel_ReferenceType(&m->skel);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static const fm_frame_t* GetFrame(const struct vk_model_s* m, int idx)
{
    if (idx < 0) idx = 0;
    if (idx >= m->header.num_frames) idx = m->header.num_frames - 1;
    return (const fm_frame_t*)(m->frames + (size_t)idx * m->header.framesize);
}

// Decode + lerp a vertex position from current/old frame.
static void DecodeLerpVert(const fm_frame_t* cur, const fm_frame_t* old,
                           int vi, float backlerp, float out[3])
{
    const float blend = 1.0f - backlerp;
    for (int k = 0; k < 3; k++) {
        const float c = cur->verts[vi].v[k] * cur->scale[k] + cur->translate[k];
        const float o = old->verts[vi].v[k] * old->scale[k] + old->translate[k];
        out[k] = o * backlerp + c * blend;
    }
}

// Build the entity's model->world transform as a 3x4 matrix, matching GL1's
// R_RotateForEntity exactly:
//   glTranslatef(origin)
//   glRotatef(+yaw,    0,0,1)
//   glRotatef(-pitch,  0,1,0)
//   glRotatef(-roll,   1,0,0)
// Angles are in radians. OpenGL post-multiplies each transform so the
// composite is T * Rz * Ry * Rx applied to a column vector.
// Result: store the rotation in mat[0..2], translation in mat[3].
typedef struct { float m[3][4]; } entity_xform_t;

static void BuildEntityXform(const entity_t* e, entity_xform_t* out)
{
    // GL1 multiplies by RAD_TO_ANGLE to get degrees, then calls glRotatef.
    // We just do everything in radians.
    const float yaw_r   =  e->angles[1];
    const float pitch_r = -e->angles[0];
    const float roll_r  = -e->angles[2];

    const float cy = cosf(yaw_r),   sy = sinf(yaw_r);
    const float cp = cosf(pitch_r), sp = sinf(pitch_r);
    const float cr = cosf(roll_r),  sr = sinf(roll_r);

    // Rz(yaw):
    //   [ cy -sy 0 ]
    //   [ sy  cy 0 ]
    //   [  0   0 1 ]
    // Ry(pitch):
    //   [ cp 0  sp ]
    //   [  0 1   0 ]
    //   [-sp 0  cp ]
    // Rx(roll):
    //   [ 1   0   0 ]
    //   [ 0  cr -sr ]
    //   [ 0  sr  cr ]
    //
    // R = Rz * Ry * Rx. Multiplied out below.

    // First: A = Rz * Ry
    const float a00 =  cy*cp;
    const float a01 = -sy;
    const float a02 =  cy*sp;
    const float a10 =  sy*cp;
    const float a11 =  cy;
    const float a12 =  sy*sp;
    const float a20 = -sp;
    const float a21 =  0.0f;
    const float a22 =  cp;

    // R = A * Rx
    const float r00 = a00;
    const float r01 = a01*cr + a02*sr;
    const float r02 = a01*(-sr) + a02*cr;
    const float r10 = a10;
    const float r11 = a11*cr + a12*sr;
    const float r12 = a11*(-sr) + a12*cr;
    const float r20 = a20;
    const float r21 = a21*cr + a22*sr;
    const float r22 = a21*(-sr) + a22*cr;

    out->m[0][0]=r00; out->m[0][1]=r01; out->m[0][2]=r02; out->m[0][3]=e->origin[0];
    out->m[1][0]=r10; out->m[1][1]=r11; out->m[1][2]=r12; out->m[1][3]=e->origin[1];
    out->m[2][0]=r20; out->m[2][1]=r21; out->m[2][2]=r22; out->m[2][3]=e->origin[2];
}

static inline void XformPoint(const entity_xform_t* X, float scale,
                                  const float local[3], float out[3])
{
    out[0] = (X->m[0][0]*local[0] + X->m[0][1]*local[1] + X->m[0][2]*local[2]) * scale + X->m[0][3];
    out[1] = (X->m[1][0]*local[0] + X->m[1][1]*local[1] + X->m[1][2]*local[2]) * scale + X->m[1][3];
    out[2] = (X->m[2][0]*local[0] + X->m[2][1]*local[1] + X->m[2][2]*local[2]) * scale + X->m[2][3];
}

// Rotate a direction (normal) by the entity transform's rotation only - no
// translation, no scale. The rotation part of entity_xform_t is orthonormal
// (R_RotateForEntity is pure rotation), so this keeps the normal unit-length.
static inline void XformNormal(const entity_xform_t* X,
                               const float local[3], float out[3])
{
    out[0] = X->m[0][0]*local[0] + X->m[0][1]*local[1] + X->m[0][2]*local[2];
    out[1] = X->m[1][0]*local[0] + X->m[1][1]*local[1] + X->m[1][2]*local[2];
    out[2] = X->m[2][0]*local[0] + X->m[2][1]*local[1] + X->m[2][2]*local[2];
}

// Decode + lerp a vertex normal from the anorms table (cur/old frame).
static void DecodeLerpNormal(const fm_frame_t* cur, const fm_frame_t* old,
                             int vi, float backlerp, float out[3])
{
    const float blend = 1.0f - backlerp;
    const float* cn = bytedirs[cur->verts[vi].n];
    const float* on = bytedirs[old->verts[vi].n];
    for (int k = 0; k < 3; k++)
        out[k] = on[k] * backlerp + cn[k] * blend;
    // Renormalize after the lerp (lerp of two unit vectors isn't unit length).
    float len = out[0]*out[0] + out[1]*out[1] + out[2]*out[2];
    if (len > 1e-8f) {
        len = 1.0f / sqrtf(len);
        out[0]*=len; out[1]*=len; out[2]*=len;
    }
}

// Lazily load misc/reflect.m32 (the sphere-map reflection texture, same asset
// ref_gl1 uses for RF_REFLECTION) and cache its descriptor.
static VkDescriptorSet GetReflectDescriptor(void)
{
    static VkDescriptorSet s_reflect_desc = VK_NULL_HANDLE;
    static qboolean s_tried = false;
    if (s_reflect_desc != VK_NULL_HANDLE) return s_reflect_desc;
    if (s_tried) return VK_NULL_HANDLE;       // don't retry every frame on failure
    s_tried = true;
    image_t* img = VK_FindImage("misc/reflect.m32");
    if (img) s_reflect_desc = VK_ImageDescriptor(img);
    return s_reflect_desc;
}

void VK_Model_DrawEntity(const struct entity_s* e, const float* mvp)
{
    if (!e || !e->model || !*e->model) return;
    // *e->model is a vk_engine_model_t* wrapper. Unwrap to a flex model.
    const vk_engine_model_t* w = (const vk_engine_model_t*)(*e->model);
    if (w->type != VK_EMODEL_FLEX || !w->impl) return;
    const struct vk_model_s* m = (const struct vk_model_s*)w->impl;
    if (!m->valid) return;
    if (!vk_state.frame_started) return;
    if (!EnsureModelVBO()) return;

    const uint32_t frame = vk_state.current_frame;
    VkCommandBuffer cb = vk_state.command_buffers[frame];

    const fm_frame_t* cur = GetFrame(m, e->frame);
    const fm_frame_t* old = GetFrame(m, e->oldframe);
    const float backlerp = e->backlerp;

    // Build entity transform (model->world) matching R_RotateForEntity.
    entity_xform_t xform;
    BuildEntityXform(e, &xform);
    const float scale = (e->scale > 0.001f) ? e->scale : 1.0f;

    // Skeletal path: pre-lerp all verts into s_lerped[], optionally rotate the
    // upper-body cluster by joint angles, and compute reference placements for
    // the client effects system (staff trails, foot shadows, casting bend).
    // This mirrors ref_gl1's StandardFrameLerp/LerpReferences.
    qboolean use_lerped = false;
    if (m->has_skel && m->header.num_xyz <= VKM_MAX_VERTS) {
        // Frame lerp vectors (move/front/back), matching StandardFrameLerp.
        const float blend = 1.0f - backlerp;
        vec3_t move, frontv, backv;
        for (int i = 0; i < 3; i++) {
            move[i]   = blend * cur->translate[i] + backlerp * old->translate[i];
            frontv[i] = blend * cur->scale[i];
            backv[i]  = backlerp * old->scale[i];
        }
        if (scale != 1.0f) {
            for (int i = 0; i < 3; i++) { move[i]*=scale; frontv[i]*=scale; backv[i]*=scale; }
        }

        // Lerp every vertex (including any packed reference/skeleton slots).
        const int total = m->skel.num_xyz_full > 0 ? m->skel.num_xyz_full : m->header.num_xyz;
        const int lim = (total > VKM_MAX_VERTS) ? VKM_MAX_VERTS : total;
        for (int vi = 0; vi < lim; vi++) {
            for (int k = 0; k < 3; k++)
                s_lerped[vi][k] = (float)cur->verts[vi].v[k] * frontv[k]
                                + (float)old->verts[vi].v[k] * backv[k] + move[k];
        }

        // Upper-body bend + swapFrame split: rotate cluster verts by joint
        // angles, blending the cast pose (swapFrame) over the run pose.
        if (m->skel.skeletalType != SKEL_NULL)
            VK_Skel_RotateMeshVerts(&((struct vk_model_s*)m)->skel, (struct entity_s*)e,
                                    s_lerped, m->frames, m->header.framesize,
                                    move, frontv, backv);

        // Reference placements -> entity->referenceInfo (consumed by effects).
        if (((entity_t*)e)->referenceInfo && m->skel.referenceType != REF_NULL)
            VK_Skel_LerpReferences(&((struct vk_model_s*)m)->skel, (struct entity_s*)e,
                                   (const vec3_t*)s_lerped, m->frames, m->header.framesize,
                                   move, frontv, backv, s_refdef_time);

        use_lerped = true;
    }

    // Resolve the default skin (entity override > model internal skin).
    // ent->skin is an externally-registered image (e.g. player skin); its
    // descriptor was allocated at image creation.
    VkDescriptorSet default_desc = VK_NULL_HANDLE;
    if (e->skin) {
        default_desc = VK_ImageDescriptor((const image_t*)e->skin);
    }
    if (default_desc == VK_NULL_HANDLE) {
        int skin_idx = e->skinnum;
        if (skin_idx < 0 || skin_idx >= m->num_skins) skin_idx = 0;
        if (m->num_skins > 0) default_desc = m->skin_desc[skin_idx];
    }

    // Push constants (shared by all node draws of this entity).
    // ref_gl1 enables blending when color.a != 255 OR any RF_TRANS flag is set
    // OR the skin has its own alpha (gl1_FlexModel.c R_DrawFlexFrameLerp). The
    // cloaking enemies fade purely via color.a with no RF_TRANS flag, so keying
    // only off RF_TRANS_ANY (as before) left them opaque under VK. Match GL1.
    // (skin->has_alpha not tracked yet; color.a + RF_TRANS_ANY covers fades.)
    const qboolean want_trans = (e->color.a != 255) || (e->flags & RF_TRANS_ANY);

    VkPipeline pipe = vk_pipeline_3d.pipeline;
    if (e->flags & (RF_TRANS_ADD | RF_TRANS_ADD_ALPHA))
        pipe = vk_pipeline_3d.pipeline_additive;
    else if (want_trans)
        pipe = vk_pipeline_3d.pipeline_translucent;

    // One-time diagnostic for translucent/alpha-textured models (e.g. shadow),
    // to confirm they reach the renderer and with what color/alpha.
    {
        static int s_md = 0;
        // Skip the fx/shadow blob models (they spam the log); focus on actual
        // translucent entity models like the cloaking enemy.
        const qboolean is_shadow = (strstr(m->name, "fx/shadow") != NULL);
        if (s_md < 40 && !is_shadow &&
            (e->flags & (RF_TRANS_ANY | RF_ALPHA_TEXTURE))) {
            ri.Con_Printf(PRINT_ALL,
                "vk: model '%s' flags=0x%x color=(%d,%d,%d,%d) scale=%.2f skins=%d pipe=%s\n",
                m->name, e->flags, e->color.r, e->color.g, e->color.b, e->color.a,
                scale, m->num_skins,
                (e->flags & (RF_TRANS_ADD|RF_TRANS_ADD_ALPHA)) ? "add" :
                (e->flags & RF_TRANS_ANY) ? "trans" : "opaque");
            s_md++;
        }
    }
    float pc[32];
    memcpy(pc, mvp, 64);

    // Base shadelight, following ref_gl1 R_DrawFlexModel order:
    //   RF_FULLBRIGHT      -> white
    //   absLight set       -> that color
    //   RF_GLOW            -> handled below (pulse), skip sampling
    //   otherwise          -> R_LightPoint(origin): sample world light so the
    //                         model is dark in shadow and bright in light.
    // Then multiply by the entity color, apply RF_MINLIGHT floor, and the
    // RF_GLOW pulse. Previously we used the raw entity color as the tint and
    // never sampled the world light, so models (e.g. the player) stayed full-
    // bright in shadowed areas.
    float shade[3];
    if (e->flags & RF_TRANS_ADD_ALPHA) {
        // ref_gl1: additive-alpha models use a grey shade = entity alpha, so the
        // additive contribution scales with alpha.
        float a = (float)e->color.a / 255.0f;
        shade[0] = shade[1] = shade[2] = a;
    } else if (e->flags & RF_FULLBRIGHT) {
        shade[0] = shade[1] = shade[2] = 1.0f;
    } else if (e->absLight.r != 0 || e->absLight.g != 0 || e->absLight.b != 0) {
        shade[0] = (float)e->absLight.r / 255.0f;
        shade[1] = (float)e->absLight.g / 255.0f;
        shade[2] = (float)e->absLight.b / 255.0f;
    } else if (e->flags & RF_GLOW) {
        shade[0] = shade[1] = shade[2] = 1.0f;   // overwritten by pulse below
    } else {
        float sl[3];
        if (VK_LightPoint_SampleRGB(e->origin, sl)) {
            // The world surfaces are drawn at lm * 2.0 (our effective modulate),
            // so scale the model's sampled light by the same factor; otherwise
            // the player would render at half the brightness of the floor it
            // stands on. Apply the same desaturating peak cap world.frag uses so
            // brightly-lit spots don't blow out differently.
            shade[0] = sl[0] * 2.0f;
            shade[1] = sl[1] * 2.0f;
            shade[2] = sl[2] * 2.0f;
            float peak = shade[0];
            if (shade[1] > peak) peak = shade[1];
            if (shade[2] > peak) peak = shade[2];
            if (peak > 2.0f) {
                float s = 2.0f / peak;
                shade[0] *= s; shade[1] *= s; shade[2] *= s;
            }
        } else {
            shade[0] = shade[1] = shade[2] = 1.0f;  // no light data -> fullbright
        }
    }

    // Modulate by entity color (c_array / 255), matching GL.
    float ecol[3] = {
        (float)e->color.r / 255.0f,
        (float)e->color.g / 255.0f,
        (float)e->color.b / 255.0f,
    };
    // All-zero entity RGB historically means "default = white".
    if (ecol[0] == 0.0f && ecol[1] == 0.0f && ecol[2] == 0.0f)
        ecol[0] = ecol[1] = ecol[2] = 1.0f;

    pc[16] = shade[0] * ecol[0];
    pc[17] = shade[1] * ecol[1];
    pc[18] = shade[2] * ecol[2];
    pc[19] = (float)e->color.a / 255.0f;
    if (pc[19] == 0.0f && !want_trans) pc[19] = 1.0f;  // truly uninitialized alpha

    // RF_MINLIGHT: floor at 0.1 when the model is essentially black, so it never
    // vanishes entirely in deep shadow (matches ref_gl1).
    if ((e->flags & RF_MINLIGHT) && pc[16] <= 0.1f && pc[17] <= 0.1f && pc[18] <= 0.1f) {
        pc[16] = pc[17] = pc[18] = 0.1f;
    }
    // RF_GLOW: bonus items (health, mana, etc) pulse with time. ref_gl1
    // (gl1_FlexModel.c) overrides shadelight with sin(time*7)*0.3 + 0.7 (a grey
    // pulse 0.4..1.0). Apply it as a tint multiplier so the item brightens and
    // dims instead of sitting at static full brightness.
    if (e->flags & RF_GLOW) {
        float val = sinf(s_refdef_time * 7.0f) * 0.3f + 0.7f;
        pc[16] *= val; pc[17] *= val; pc[18] *= val;
    }
    // RF_TRANS_GHOST: like ref_gl1, the ghost's alpha is driven by the world
    // light at the entity (alpha = shadelight[0] * 0.5), not the entity color
    // alpha - so a ghost is more solid in lit areas and fainter in shadow.
    // shadelight[0] = red channel of the point-light sample, modulated by the
    // entity color (matching R_DrawFlexFrameLerp). Falls back to the entity
    // alpha if lighting data isn't available.
    if (e->flags & RF_TRANS_GHOST) {
        float sl[3];
        if (VK_LightPoint_SampleRGB(e->origin, sl)) {
            float r = sl[0] * ((float)e->color.r / 255.0f);
            float a = r * 0.5f;
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            pc[19] = a;
        }
    }
    Model_FillFogParams(pc);

    vkm_vert_t* vbuf = s_model_mapped[frame];
    qboolean pipeline_bound = false;
    VkPipeline bound_pipe = VK_NULL_HANDLE;  // currently-bound node pipeline
    VkDeviceSize vbo_base = 0; // bound once

    for (int mn = 0; mn < m->num_mesh_nodes; mn++) {
        // Per-node visibility: the engine hides equipment/body parts the
        // entity isn't currently showing via fmnodeinfo[].flags FMNI_NO_DRAW.
        if (e->fmnodeinfo && (e->fmnodeinfo[mn].flags & FMNI_NO_DRAW))
            continue;

        // Per-node skin selection. If the node requests a specific skin and
        // the entity provides a skin array, use it; else default.
        VkDescriptorSet node_desc = default_desc;
        if (e->fmnodeinfo && (e->fmnodeinfo[mn].flags & FMNI_USE_SKIN)) {
            const int si = e->fmnodeinfo[mn].skin;
            if (e->skins && si >= 0) {
                image_t* sk = (image_t*)e->skins[si];
                VkDescriptorSet d = VK_ImageDescriptor(sk);
                if (d != VK_NULL_HANDLE) node_desc = d;
            } else if (si >= 0 && si < m->num_skins) {
                if (m->skin_desc[si] != VK_NULL_HANDLE) node_desc = m->skin_desc[si];
            }
        }

        // Reflection: whole-entity RF_REFLECTION, or per-node FMNI_USE_REFLECT.
        // A reflective node draws with the reflect pipeline (sphere-map shader)
        // and binds the reflect texture in place of the skin. Falls back to the
        // normal path if the reflect texture isn't available.
        qboolean node_reflect = (e->flags & RF_REFLECTION) ? true : false;
        if (e->fmnodeinfo && (e->fmnodeinfo[mn].flags & FMNI_USE_REFLECT))
            node_reflect = true;

        VkPipeline node_pipe = pipe;
        if (node_reflect) {
            VkDescriptorSet rd = GetReflectDescriptor();
            if (rd != VK_NULL_HANDLE && vk_pipeline_3d.pipeline_reflect != VK_NULL_HANDLE) {
                node_desc = rd;
                node_pipe = vk_pipeline_3d.pipeline_reflect;
            }
        }

        if (node_desc == VK_NULL_HANDLE) continue; // nothing to draw with

        // Emit this node's glcmds into the dynamic VBO.
        const uint32_t node_start = s_model_cursor[frame];
        uint32_t cursor = node_start;

        const int* order = &m->glcmds[m->mesh_nodes[mn].start_glcmds];
        while (1) {
            int count = *order++;
            if (count == 0) break;
            qboolean fan = (count < 0);
            if (fan) count = -count;

            float tmp_uv[256][2];
            float tmp_pos[256][3];
            float tmp_nrm[256][3];
            int nprim = (count > 256) ? 256 : count;
            for (int c = 0; c < count; c++) {
                const float s = ((const float*)order)[0];
                const float t = ((const float*)order)[1];
                const int vi  = order[2];
                order += 3;
                if (c >= nprim) continue;
                tmp_uv[c][0] = s; tmp_uv[c][1] = t;

                float local[3], world[3];
                if (use_lerped && vi >= 0 && vi < VKM_MAX_VERTS) {
                    local[0] = s_lerped[vi][0];
                    local[1] = s_lerped[vi][1];
                    local[2] = s_lerped[vi][2];
                    // s_lerped already has the frame scale/translate baked in,
                    // so the entity scale must NOT be re-applied here.
                    XformPoint(&xform, 1.0f, local, world);
                } else {
                    DecodeLerpVert(cur, old, vi, backlerp, local);
                    XformPoint(&xform, scale, local, world);
                }
                tmp_pos[c][0]=world[0]; tmp_pos[c][1]=world[1]; tmp_pos[c][2]=world[2];

                // World-space normal (for env-map reflection). Decoded from the
                // anorms table and rotated by the entity transform.
                float nlocal[3], nworld[3];
                if (vi >= 0) {
                    DecodeLerpNormal(cur, old, vi, backlerp, nlocal);
                    XformNormal(&xform, nlocal, nworld);
                } else {
                    nworld[0]=0.0f; nworld[1]=0.0f; nworld[2]=1.0f;
                }
                tmp_nrm[c][0]=nworld[0]; tmp_nrm[c][1]=nworld[1]; tmp_nrm[c][2]=nworld[2];
            }

            for (int c = 2; c < nprim; c++) {
                int i0, i1, i2;
                if (fan) { i0 = 0; i1 = c-1; i2 = c; }
                else if (c & 1) { i0 = c-1; i1 = c-2; i2 = c; }
                else            { i0 = c-2; i1 = c-1; i2 = c; }

                const int idxs[3] = { i0, i1, i2 };
                for (int k = 0; k < 3; k++) {
                    if (cursor + 1 >= VKM_MAX_VERTS * 64) break;
                    const int si = idxs[k];
                    vbuf[cursor].x = tmp_pos[si][0];
                    vbuf[cursor].y = tmp_pos[si][1];
                    vbuf[cursor].z = tmp_pos[si][2];
                    vbuf[cursor].u = tmp_uv[si][0];
                    vbuf[cursor].v = tmp_uv[si][1];
                    vbuf[cursor].nx = tmp_nrm[si][0];
                    vbuf[cursor].ny = tmp_nrm[si][1];
                    vbuf[cursor].nz = tmp_nrm[si][2];
                    cursor++;
                }
            }
        }

        const uint32_t node_count = cursor - node_start;
        if (node_count == 0) continue;
        s_model_cursor[frame] = cursor;

        // Bind shared state once (push constants, vertex buffer, dlight set).
        if (!pipeline_bound) {
            vkCmdPushConstants(cb, vk_pipeline_3d.layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), pc);
            vkCmdBindVertexBuffers(cb, 0, 1, &s_model_vbo[frame].buffer, &vbo_base);
            // Dynamic-light UBO at set 1 (filled by the world pass earlier this
            // frame). The entity pipeline layout declares it.
            {
                VkDescriptorSet dlset = VK_World_CurrentDlightSet();
                if (dlset != VK_NULL_HANDLE)
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            vk_pipeline_3d.layout, 1, 1, &dlset, 0, NULL);
            }
            pipeline_bound = true;
            bound_pipe = VK_NULL_HANDLE;   // force a pipeline bind below
        }
        // Bind (or switch) the node's pipeline. Reflective nodes use the
        // reflect pipeline; everything else the entity's base pipeline.
        if (node_pipe != bound_pipe) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, node_pipe);
            bound_pipe = node_pipe;
        }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vk_pipeline_3d.layout, 0, 1, &node_desc, 0, NULL);
        vkCmdDraw(cb, node_count, 1, node_start, 0);
    }

    if (pipeline_bound)
        vk_state.pipeline_2d_bound = false;
}

// Reset the per-frame model VBO cursor at the start of each frame.
void VK_Model_BeginFrame(void)
{
    if (!s_model_vbo_ready) return;
    s_model_cursor[vk_state.current_frame] = 0;
}

// Publish the current refdef time so the skeletal reference lerp can stamp
// referenceInfo->lastUpdate (what RefPointsValid checks).
void VK_Model_SetFrameTime(float t)
{
    s_refdef_time = t;
}

// Publish the camera world position for underwater fog distance calculation.
void VK_Model_SetViewOrigin(const float org[3])
{
    s_view_origin[0] = org[0];
    s_view_origin[1] = org[1];
    s_view_origin[2] = org[2];
}
