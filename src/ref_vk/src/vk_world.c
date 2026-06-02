//
// vk_world.c - BSP load + textured + lightmapped world rendering.
//
// Adds lightmap support over the previous textured-only version:
//   - Parse LIGHTING lump (RGB triplets per luxel)
//   - Per-surface CalcSurfaceExtents (texturemins, extents)
//   - Pack each surface's lightmap into a single big atlas with row-best-fit
//   - Two UVs per vertex: diffuse (tiled) + lightmap (atlas)
//   - World uses vk_pipeline_world (2 samplers, no tint)
//
// Still NOT done: PVS culling, frustum culling, sub-models, sky, water.
//

#include "vk_world.h"
#include "vk_buffer.h"
#include "vk_image.h"
#include "vk_pipeline3d.h"
#include "vk_pipeline_world.h"
#include "vk_lightpoint.h"
#include "vk_local.h"

#include "qcommon/qfiles.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef struct {
    float x, y, z;
    float du, dv;    // diffuse UV
    float lu, lv;    // lightmap UV
} world_vert_t;

typedef struct {
    VkDescriptorSet descriptor;
    uint32_t        first_vertex;
    uint32_t        num_vertices;
    // Texture animation chain (func_button/door frame swaps). frame_desc[0] is
    // the base texture (== descriptor); index by entity frame % num_frames.
    VkDescriptorSet frame_desc[8];
    int             num_frames;
    int             anim_speed;     // SURF_ANIMSPEED fps (>0 = time-animated, e.g. forcefields)
    // Surface rendering class. 0 = opaque lightmapped (default pass). Warp/trans
    // surfaces are drawn in a separate translucent pass with the warp shader.
    int             surf_flags;     // subset of SURF_* (WARP/TRANS33/TRANS66/FLOWING/UNDULATE)
    float           alpha;          // 1.0 opaque, 0.33/0.66 for trans surfaces
    float           center[3];      // world-space centroid (for back-to-front alpha sort)
} world_batch_t;

#define WORLD_MAX_BATCHES        2048
#define VK_MAX_TEXTURES_GUESS    1024

// --- PVS / BSP-tree culling -------------------------------------------------
// To skip drawing geometry not potentially visible from the camera's leaf, we
// keep the BSP node/leaf tree, the leaf->face lists, and the visibility (PVS)
// lump. Each frame: find the view leaf, decompress its cluster PVS, mark the
// faces in visible leaves, then draw only those (as merged runs within each
// per-texture batch). Mirrors ref_gl1 R_MarkLeaves + the recursive world walk.
typedef struct {
    int      planenum;        // index into s_pvs_planes
    int      children[2];     // >=0 node index; <0 => leaf index = -(child)-1
    int      parent;          // parent node index, -1 for root
    float    mins[3], maxs[3];
} pvs_node_t;

typedef struct {
    int      cluster;         // -1 = no cluster (solid)
    int      parent;          // parent node index
    int      first_leafface;  // index into s_pvs_leaffaces
    int      num_leaffaces;
    int      visframe;        // last frame this leaf was marked visible
    float    mins[3], maxs[3];
} pvs_leaf_t;

// Per-face record: which texture batch it belongs to and its vertex sub-range
// inside that batch's contiguous range. Lets us draw only visible faces.
typedef struct {
    int      batch;           // index into s_batches (-1 = not drawn / culled type)
    uint32_t first_vertex;
    uint32_t num_vertices;
    int      visframe;        // last frame this face was marked visible
    float    center[3];       // world-space centroid (alpha back-to-front sort)
    int      surf_flags;      // this face's OWN SURF_* flags (warp/flow/etc) -
                              // ref_gl1 evaluates flow/warp per-face, not per
                              // texture-batch, so we store the face's own flags
                              // to avoid one flowing face making its whole
                              // texture batch flow.
} pvs_face_t;

typedef struct { float normal[3]; float dist; } pvs_plane_t;


// Lightmap atlas. Single big texture. 2048x2048 = 4 MB RGB. We use a simple
// shelf-pack allocator (row-by-row with current-row remainder tracking).
#define LM_ATLAS_W   2048
#define LM_ATLAS_H   2048

static qboolean      s_loaded           = false;
static char          s_loaded_name[256] = {0};
static byte*         s_bsp_buffer       = NULL;

static vk_buffer_t   s_vbo              = {0};
static uint32_t      s_total_verts      = 0;

static world_batch_t s_batches[WORLD_MAX_BATCHES];
static int           s_num_batches      = 0;

// PVS / BSP-tree culling state.
static pvs_node_t*   s_pvs_nodes        = NULL;  static int s_pvs_num_nodes = 0;
static pvs_leaf_t*   s_pvs_leaves       = NULL;  static int s_pvs_num_leaves = 0;
static pvs_plane_t*  s_pvs_planes       = NULL;  static int s_pvs_num_planes = 0;
static int*          s_pvs_leaffaces    = NULL;  static int s_pvs_num_leaffaces = 0;
static pvs_face_t*   s_pvs_faces        = NULL;  static int s_pvs_num_faces = 0;
// Faces in emission order; each batch owns a contiguous [start,count) slice.
static int*          s_pvs_face_order   = NULL;  static int s_pvs_face_order_n = 0;
static int           s_batch_face_start[WORLD_MAX_BATCHES];
static int           s_batch_face_count[WORLD_MAX_BATCHES];
static byte*         s_pvs_visdata      = NULL;  // raw visibility lump (compressed)
static int           s_pvs_num_clusters = 0;
static int*          s_pvs_cluster_ofs  = NULL;  // bitofs[cluster] for DVIS_PVS
static int           s_pvs_visframe     = 0;
static int           s_pvs_old_viewleaf = -1;
static cvar_t*       s_r_novis          = NULL;
static qboolean      s_pvs_ready        = false;
static float         s_view_origin_world[3] = {0,0,0};  // camera pos this frame (for submodel fog)
static float         s_frame_time = 0.0f;                // refdef time this frame (warp/flow anim)

// Forward decls (defined below, used earlier in VK_World_Free / render).
static void VK_PVS_Free(void);
static void VK_PVS_Mark(const float vieworg[3]);

// Translucency alpha cvars, shared with ref_gl1 (gl_trans33/gl_trans66). Looked
// up lazily on first render so water/glass alpha matches the GL renderer and
// responds to the same console vars.
extern refimport_t ri;
static cvar_t* s_gl_trans33 = NULL;
static cvar_t* s_gl_trans66 = NULL;

// Underwater fog cvars (match ref_gl1 R_WaterFog). Looked up lazily.
static cvar_t* s_uw_cam      = NULL;  // cl_camera_under_surface
static cvar_t* s_uw_mode     = NULL;  // r_fog_underwater_mode (0 LINEAR,1 EXP,2 EXP2)
static cvar_t* s_uw_density  = NULL;  // r_fog_underwater_density
static cvar_t* s_uw_start    = NULL;  // r_fog_underwater_startdist
static cvar_t* s_uw_col_r    = NULL;
static cvar_t* s_uw_col_g    = NULL;
static cvar_t* s_uw_col_b    = NULL;
static cvar_t* s_farclip     = NULL;  // r_farclipdist

// Global level fog (r_fog), separate from underwater fog. Set per-level (e.g.
// Darkmire swamp via trigger_fogdensity / worldspawn). Same modes/math.
static cvar_t* s_fog         = NULL;  // r_fog (on/off)
static cvar_t* s_fog_mode    = NULL;  // r_fog_mode (0 LINEAR,1 EXP,2 EXP2)
static cvar_t* s_fog_density = NULL;  // r_fog_density
static cvar_t* s_fog_start   = NULL;  // r_fog_startdist
static cvar_t* s_fog_col_r   = NULL;
static cvar_t* s_fog_col_g   = NULL;
static cvar_t* s_fog_col_b   = NULL;

// Fill the 48-byte fog tail (3 vec4 = 12 floats) of a push constant, starting
// at float index 'off'. World layout: off=16 (after mat4). Warp layout: off=24
// (after mat4 + params + params2). Reads the underwater-fog cvars; when the
// camera is under a water surface sets up GL-equivalent distance fog, else
// disables fog (mode = -1). 'cam' is the camera world position.
static void FillFogParamsAt(float* pc, int off, const float cam[3])
{
    if (!s_uw_cam)     s_uw_cam     = ri.Cvar_Get("cl_camera_under_surface", "0", 0);
    if (!s_uw_mode)    s_uw_mode    = ri.Cvar_Get("r_fog_underwater_mode", "1", 0);
    if (!s_uw_density) s_uw_density = ri.Cvar_Get("r_fog_underwater_density", "0.0015", 0);
    if (!s_uw_start)   s_uw_start   = ri.Cvar_Get("r_fog_underwater_startdist", "100.0", 0);
    if (!s_uw_col_r)   s_uw_col_r   = ri.Cvar_Get("r_fog_underwater_color_r", "1.0", 0);
    if (!s_uw_col_g)   s_uw_col_g   = ri.Cvar_Get("r_fog_underwater_color_g", "1.0", 0);
    if (!s_uw_col_b)   s_uw_col_b   = ri.Cvar_Get("r_fog_underwater_color_b", "1.0", 0);
    if (!s_farclip)    s_farclip    = ri.Cvar_Get("r_farclipdist", "4096.0", 0);
    if (!s_fog)         s_fog         = ri.Cvar_Get("r_fog", "0", 0);
    if (!s_fog_mode)    s_fog_mode    = ri.Cvar_Get("r_fog_mode", "1", 0);
    if (!s_fog_density) s_fog_density = ri.Cvar_Get("r_fog_density", "0.004", 0);
    if (!s_fog_start)   s_fog_start   = ri.Cvar_Get("r_fog_startdist", "50.0", 0);
    if (!s_fog_col_r)   s_fog_col_r   = ri.Cvar_Get("r_fog_color_r", "1.0", 0);
    if (!s_fog_col_g)   s_fog_col_g   = ri.Cvar_Get("r_fog_color_g", "1.0", 0);
    if (!s_fog_col_b)   s_fog_col_b   = ri.Cvar_Get("r_fog_color_b", "1.0", 0);

    const qboolean underwater = (s_uw_cam && s_uw_cam->value != 0.0f);
    const qboolean globalfog  = !underwater && (s_fog && s_fog->value != 0.0f);

    // Fog camera position (xyz) - the shader computes fog distance as
    // length(in_pos - fog_cam.xyz). This was accidentally dropped in the v84
    // global-fog refactor, leaving fog_cam.xyz at stale/zero values so the
    // distance was measured from the world origin - whiting out everything in
    // maps far from origin (e.g. Darkmire). Restore it.
    pc[off+0] = cam[0]; pc[off+1] = cam[1]; pc[off+2] = cam[2];

    if (underwater) {
        int mode = s_uw_mode ? (int)s_uw_mode->value : 1;
        if (mode < 0) mode = 0; if (mode > 2) mode = 2;
        pc[off+3] = s_uw_density ? s_uw_density->value : 0.0015f;
        pc[off+4] = s_uw_col_r ? s_uw_col_r->value : 1.0f;
        pc[off+5] = s_uw_col_g ? s_uw_col_g->value : 1.0f;
        pc[off+6] = s_uw_col_b ? s_uw_col_b->value : 1.0f;
        pc[off+7] = (float)mode;
        pc[off+8] = s_uw_start ? s_uw_start->value : 100.0f;
    } else if (globalfog) {
        // ref_gl1 R_Fog: level-wide fog from the r_fog_* cvars.
        int mode = s_fog_mode ? (int)s_fog_mode->value : 1;
        if (mode < 0) mode = 0; if (mode > 2) mode = 2;
        pc[off+3] = s_fog_density ? s_fog_density->value : 0.004f;
        pc[off+4] = s_fog_col_r ? s_fog_col_r->value : 1.0f;
        pc[off+5] = s_fog_col_g ? s_fog_col_g->value : 1.0f;
        pc[off+6] = s_fog_col_b ? s_fog_col_b->value : 1.0f;
        pc[off+7] = (float)mode;
        pc[off+8] = s_fog_start ? s_fog_start->value : 50.0f;
    } else {
        pc[off+3] = 0.0015f;
        pc[off+4] = pc[off+5] = pc[off+6] = 0.0f;
        pc[off+7] = -1.0f;             // fog off
        pc[off+8] = 100.0f;
    }

    pc[off+9]  = s_farclip  ? s_farclip->value  : 4096.0f;
    pc[off+10] = 0.0f; pc[off+11] = 0.0f;
}

static void FillFogParams(float* pc /* >=24 floats */, const float cam[3])
{
    FillFogParamsAt(pc, 16, cam);
}

// Fill the entity-layout fog tail (pc[20..31]: fog_cam@20, fog_color@24,
// fog_extra@28) for the SKY. ref_gl1 keeps fog enabled while drawing the sky,
// so in a fogged level the sky blends into the fog colour at the horizon. The
// sky's geometry sits ~1 unit from the camera, so a normal distance-based fog
// wouldn't touch it; we set fog_extra.w = 1 as a "sky fog" flag that the entity
// shader reads to apply the full fog colour. When no fog is active, mode = -1.
void VK_World_FillSkyFog(float* pc /* >=32 floats */)
{
    // Reuse FillFogParamsAt at offset 20 (matches entity layout: cam@20,
    // color@24, extra@28). cam pos is irrelevant for the sky (we force fog via
    // the flag), so pass the stored view origin.
    FillFogParamsAt(pc, 20, s_view_origin_world);
    // fog_extra.w = sky marker. Always 1 so the entity shader skips the alpha
    // test for the sky (an opaque background) regardless of fog. The shader
    // applies full fog colour only when fog is actually active (mode = fog_color.w
    // >= 0); with fog off (mode < 0) fog_factor returns 1.0 and the sky shows
    // through unchanged.
    pc[31] = 1.0f;
}

// Fill the entity-layout fog tail (cam@20, color@24, extra@28) with the real
// per-frame fog for world-positioned billboards (alpha sprites, scorch decals)
// so they fade into the fog by true distance instead of standing out.
void VK_World_FillEntityFog(float* pc /* >=32 floats */)
{
    FillFogParamsAt(pc, 20, s_view_origin_world);
    pc[31] = 0.0f;   // not sky fog
}

// Build this frame's dynamic-light UBO from the refdef and return the set to
// bind at set = 1 (or VK_NULL_HANDLE if none). gl_modulate scales intensity to
// match ref_gl1.
static cvar_t* s_gl_modulate = NULL;
static VkDescriptorSet BuildAndBindDlights(const refdef_t* fd)
{
    if (!s_gl_modulate) s_gl_modulate = ri.Cvar_Get("gl_modulate", "1", 0);
    const float modulate = s_gl_modulate ? s_gl_modulate->value : 1.0f;

    int n = fd->num_dlights;
    if (n < 0) n = 0;
    if (n > 32) n = 32;

    float origins[32*3], intens[32], colors[32*3];
    for (int i = 0; i < n; i++) {
        const dlight_t* d = &fd->dlights[i];
        origins[i*3+0] = d->origin[0];
        origins[i*3+1] = d->origin[1];
        origins[i*3+2] = d->origin[2];
        intens[i]      = d->intensity;
        colors[i*3+0]  = (float)d->color.r;
        colors[i*3+1]  = (float)d->color.g;
        colors[i*3+2]  = (float)d->color.b;
    }
    return VK_World_UpdateDlights(n, origins, intens, colors, modulate);
}

// Inline brush submodels (*1, *2, ... = doors, lifts, etc). Submodel 0 is
// the world itself. Each has its own batch range in a shared submodel VBO.
#define WORLD_MAX_SUBMODELS  256

typedef struct {
    int            first_face;
    int            num_faces;
    float          origin[3];
    // Batches for this submodel (index range into s_sub_batches).
    int            first_batch;
    int            num_batches;
} world_submodel_t;

static world_submodel_t s_submodels[WORLD_MAX_SUBMODELS];
static int              s_num_submodels = 0;

static vk_buffer_t      s_sub_vbo       = {0};
static uint32_t         s_sub_total_verts = 0;
static world_batch_t    s_sub_batches[WORLD_MAX_BATCHES];
static int              s_num_sub_batches = 0;

// Lightmap atlas resources.
static vk_texture_t  s_lm_tex          = {0};   // image + view + memory
static byte*         s_lm_cpu          = NULL;  // CPU staging while packing (LM_ATLAS_W*LM_ATLAS_H*4)
static int           s_lm_row_y        = 0;     // current row's top
static int           s_lm_row_x        = 0;     // next free x in current row
static int           s_lm_row_h        = 0;     // current row's height (max h of allocs in this row)

// --- Animated lightstyles (flickering/pulsing/switchable lights) -------------
// ref_gl1 rebuilds each lit surface's lightmap every frame from up to 4 styles
// scaled by the live lightstyles[].rgb intensities (R_BuildLightMap). We bake a
// static atlas, so we retain each animated surface's raw multi-style samples and
// its atlas slot, and re-bake + re-upload only the surfaces whose styles changed.
#define LM_MAXSTYLES 4
typedef struct {
    int   lm_x, lm_y, lm_w, lm_h;     // atlas slot (luxels)
    byte  styles[LM_MAXSTYLES];       // style indices (255 = unused)
    int   num_styles;                 // how many styles (>=2 means animated-capable)
    const byte* samples;              // -> into s_lightsamples, num_styles*w*h*3 bytes
} lm_anim_surf_t;

static lm_anim_surf_t* s_anim_surfs   = NULL;  // surfaces with >1 style (animated)
static int             s_num_anim     = 0;
static byte*           s_lightsamples = NULL;  // retained copy of the BSP lighting lump
static cvar_t*         s_gl_modulate_lm = NULL;

// Live lightstyle intensities, captured from the refdef each frame.
static float           s_lightstyles[256][3];
static float           s_lightstyles_prev[256][3];
static qboolean        s_lightstyles_valid = false;


// ---------------------------------------------------------------------------
// Lightmap atlas allocation (simple shelf-pack)
// ---------------------------------------------------------------------------

static qboolean LMAtlas_Alloc(int w, int h, int* out_x, int* out_y)
{
    if (w > LM_ATLAS_W || h > LM_ATLAS_H) return false;
    if (s_lm_row_x + w > LM_ATLAS_W) {
        // Start a new row.
        s_lm_row_y += s_lm_row_h;
        s_lm_row_x  = 0;
        s_lm_row_h  = 0;
    }
    if (s_lm_row_y + h > LM_ATLAS_H) return false;
    *out_x = s_lm_row_x;
    *out_y = s_lm_row_y;
    s_lm_row_x += w;
    if (h > s_lm_row_h) s_lm_row_h = h;
    return true;
}

static void LMAtlas_Copy(int x, int y, int w, int h, const byte* rgb) __attribute__((unused));
static void LMAtlas_Copy(int x, int y, int w, int h, const byte* rgb)
{
    if (!s_lm_cpu) return;
    for (int row = 0; row < h; row++) {
        byte* dst = s_lm_cpu + ((y + row) * LM_ATLAS_W + x) * 4;
        const byte* src = rgb + row * w * 3;
        for (int col = 0; col < w; col++) {
            dst[col*4+0] = src[col*3+0];
            dst[col*4+1] = src[col*3+1];
            dst[col*4+2] = src[col*3+2];
            dst[col*4+3] = 255;
        }
    }
}

// Fill an unallocated luxel block with a fixed gray so missing-lightmap
// surfaces don't appear pitch black.
static void LMAtlas_FillSolid(int x, int y, int w, int h, byte g)
{
    if (!s_lm_cpu) return;
    for (int row = 0; row < h; row++) {
        byte* dst = s_lm_cpu + ((y + row) * LM_ATLAS_W + x) * 4;
        for (int col = 0; col < w; col++) {
            dst[col*4+0] = g; dst[col*4+1] = g; dst[col*4+2] = g; dst[col*4+3] = 255;
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const void* GetLumpData(const byte* buf, const lump_t* l)
{
    return buf + l->fileofs;
}

qboolean VK_World_IsLoaded(void) { return s_loaded; }

void VK_World_Free(void)
{
    if (s_vbo.buffer) {
        vkDeviceWaitIdle(vk_state.device);
        VK_DestroyBuffer(&s_vbo);
    }
    if (s_sub_vbo.buffer) {
        VK_DestroyBuffer(&s_sub_vbo);
    }
    if (s_lm_tex.image) {
        VK_DestroyTexture(&s_lm_tex);
    }
    if (s_bsp_buffer) {
        ri.FS_FreeFile(s_bsp_buffer);
        s_bsp_buffer = NULL;
    }
    VK_LightPoint_Free();
    VK_PVS_Free();
    free(s_lm_cpu);
    s_lm_cpu = NULL;
    free(s_lightsamples);
    s_lightsamples = NULL;
    free(s_anim_surfs);
    s_anim_surfs = NULL;
    s_num_anim = 0;
    s_lightstyles_valid = false;
    s_lm_row_x = s_lm_row_y = s_lm_row_h = 0;
    s_loaded = false;
    s_total_verts = 0;
    s_num_batches = 0;
    s_num_submodels = 0;
    s_num_sub_batches = 0;
    s_sub_total_verts = 0;
    s_loaded_name[0] = 0;
}

// ---------------------------------------------------------------------------
// World texture cache (one entry per unique texinfo name)
// ---------------------------------------------------------------------------

typedef struct {
    char            tex_name[64];
    image_t*        image;
    VkDescriptorSet descriptor;   // bound to (image, lightmap atlas)
    int             width, height;
} world_tex_t;

static world_tex_t s_world_textures[VK_MAX_TEXTURES_GUESS];
static int         s_num_world_textures = 0;

// Note: descriptors get allocated AFTER the lightmap atlas image view is
// known, so we resolve images first then bind descriptors in a second pass.

static world_tex_t* WorldTex_Resolve(const char* tex_name)
{
    for (int i = 0; i < s_num_world_textures; i++)
        if (strcasecmp(s_world_textures[i].tex_name, tex_name) == 0)
            return &s_world_textures[i];

    if (s_num_world_textures >= VK_MAX_TEXTURES_GUESS)
        return NULL;

    world_tex_t* wt = &s_world_textures[s_num_world_textures];
    memset(wt, 0, sizeof(*wt));
    strncpy(wt->tex_name, tex_name, sizeof(wt->tex_name) - 1);

    char path[128];
    snprintf(path, sizeof(path), "textures/%s.m8", tex_name);
    wt->image = VK_FindImage(path);
    if (!wt->image) {
        snprintf(path, sizeof(path), "textures/%s.m32", tex_name);
        wt->image = VK_FindImage(path);
    }
    if (!wt->image) {
        ri.Con_Printf(PRINT_ALL, "vk: world texture '%s' missing\n", tex_name);
        return NULL;
    }
    wt->width  = VK_ImageWidth(wt->image);
    wt->height = VK_ImageHeight(wt->image);

    s_num_world_textures++;
    return wt;
}

// ---------------------------------------------------------------------------
// CalcSurfaceExtents
// ---------------------------------------------------------------------------

typedef struct {
    short texturemins[2];
    short extents[2];
    int   lm_x, lm_y;
    int   lm_w, lm_h;
    qboolean has_lightmap;
} surface_lm_t;

static void CalcExtents(const dface_t* f, const dvertex_t* verts, const dedge_t* edges,
                        const int* surfedge, const texinfo_t* ti, int num_verts,
                        int num_edges_lump, surface_lm_t* out)
{
    float mins[2] = { 999999.0f, 999999.0f };
    float maxs[2] = { -99999.0f, -99999.0f };

    for (int i = 0; i < f->numedges; i++) {
        const int e = surfedge[f->firstedge + i];
        int vi;
        if (e >= 0) {
            if (e >= num_edges_lump) continue;
            vi = edges[e].v[0];
        } else {
            const int idx = -e;
            if (idx >= num_edges_lump) continue;
            vi = edges[idx].v[1];
        }
        if (vi < 0 || vi >= num_verts) continue;
        const float* p = verts[vi].point;
        for (int j = 0; j < 2; j++) {
            const float val = p[0]*ti->vecs[j][0] + p[1]*ti->vecs[j][1] +
                              p[2]*ti->vecs[j][2] + ti->vecs[j][3];
            if (val < mins[j]) mins[j] = val;
            if (val > maxs[j]) maxs[j] = val;
        }
    }
    for (int i = 0; i < 2; i++) {
        const int bmin = (int)floorf(mins[i] / 16.0f);
        const int bmax = (int)ceilf (maxs[i] / 16.0f);
        out->texturemins[i] = (short)(bmin * 16);
        out->extents[i]     = (short)((bmax - bmin) * 16);
    }
}

// Emit one BSP face's triangles into 'out' (fan triangulation). Returns the
// number of vertices written (0 if the face was skipped). Shared by world and
// submodel geometry building.
static int EmitFaceEx(const dface_t* f, const texinfo_t* ti, const world_tex_t* wt,
                    const surface_lm_t* sm,
                    const dvertex_t* verts, const dedge_t* edges, const int* surfedge,
                    int num_verts, world_vert_t* out, qboolean warp);

// --- Warp surface subdivision (port of ref_gl1 R_SubdivideSurface) ---
// Warp (water/lava) surfaces are split into a grid of <=64-unit polygons so the
// per-vertex turbsin warp samples at fine intervals (gentle ripple). Without
// this the warp is computed over the whole surface at once -> chaotic swirl.
#define SUBDIVIDE_SIZE 64.0f

static void BoundPoly(int numverts, const float* verts, vec3_t mins, vec3_t maxs)
{
    mins[0]=mins[1]=mins[2]=  9999.0f;
    maxs[0]=maxs[1]=maxs[2]= -9999.0f;
    const float* v = verts;
    for (int i = 0; i < numverts; i++)
        for (int j = 0; j < 3; j++, v++) {
            if (*v < mins[j]) mins[j] = *v;
            if (*v > maxs[j]) maxs[j] = *v;
        }
}

// Emits triangles for one subdivided polygon into out[]; returns vert count.
// Each grid poly becomes a fan (center + ring), output as a triangle list with
// raw texel s,t in du,dv (the warp shader divides by 64).
static int EmitWarpPoly(const texinfo_t* ti, const world_tex_t* wt, const surface_lm_t* sm,
                        int numverts, const float* poly, world_vert_t* out)
{
    (void)wt;
    // Build the fan: center vertex + numverts ring verts.
    world_vert_t center; vec3_t total = {0,0,0};
    float total_s = 0.0f, total_t = 0.0f;
    world_vert_t ring[66];

    for (int i = 0; i < numverts; i++) {
        const float* p = &poly[i*3];
        const float s = p[0]*ti->vecs[0][0] + p[1]*ti->vecs[0][1] + p[2]*ti->vecs[0][2] + ti->vecs[0][3];
        const float t = p[0]*ti->vecs[1][0] + p[1]*ti->vecs[1][1] + p[2]*ti->vecs[1][2] + ti->vecs[1][3];
        ring[i].x=p[0]; ring[i].y=p[1]; ring[i].z=p[2];
        ring[i].du=s;   ring[i].dv=t;        // raw texel s,t
        ring[i].lu=0.5f/LM_ATLAS_W; ring[i].lv=0.5f/LM_ATLAS_H; // fullbright, no lightmap
        total[0]+=p[0]; total[1]+=p[1]; total[2]+=p[2];
        total_s+=s; total_t+=t;
    }
    center.x=total[0]/numverts; center.y=total[1]/numverts; center.z=total[2]/numverts;
    center.du=total_s/numverts; center.dv=total_t/numverts;
    center.lu=0.5f/LM_ATLAS_W;  center.lv=0.5f/LM_ATLAS_H;

    int c = 0;
    for (int i = 0; i < numverts; i++) {
        out[c++] = center;
        out[c++] = ring[i];
        out[c++] = ring[(i+1) % numverts];
    }
    return c;
}

static int SubdividePolygon(const texinfo_t* ti, const world_tex_t* wt, const surface_lm_t* sm,
                            int numverts, float* verts, world_vert_t* out)
{
    if (numverts > 60) return 0;

    vec3_t mins, maxs;
    BoundPoly(numverts, verts, mins, maxs);

    for (int i = 0; i < 3; i++) {
        float m = (mins[i] + maxs[i]) * 0.5f;
        m = SUBDIVIDE_SIZE * floorf(m / SUBDIVIDE_SIZE + 0.5f);
        if (maxs[i] - m < 8.0f || m - mins[i] < 8.0f) continue;

        float dist[66];
        float* v = verts + i;
        for (int j = 0; j < numverts; j++, v += 3) dist[j] = *v - m;
        dist[numverts] = dist[0];
        v -= i;
        // wrap: copy first vert past the end
        for (int k = 0; k < 3; k++) verts[numverts*3 + k] = verts[k];

        vec3_t front[66], back[66];
        int fcount = 0, bcount = 0;
        v = verts;
        for (int j = 0; j < numverts; j++, v += 3) {
            if (dist[j] >= 0) { VectorCopy(v, front[fcount]); fcount++; }
            if (dist[j] <= 0) { VectorCopy(v, back[bcount]); bcount++; }
            if (dist[j] == 0.0f || dist[j+1] == 0.0f) continue;
            if ((dist[j] > 0) != (dist[j+1] > 0)) {
                const float frac = dist[j] / (dist[j] - dist[j+1]);
                for (int k = 0; k < 3; k++)
                    front[fcount][k] = back[bcount][k] = v[k] + frac * (v[3+k] - v[k]);
                fcount++; bcount++;
            }
        }
        int n = 0;
        n += SubdividePolygon(ti, wt, sm, fcount, front[0], out + n);
        n += SubdividePolygon(ti, wt, sm, bcount, back[0], out + n);
        return n;
    }

    return EmitWarpPoly(ti, wt, sm, numverts, verts, out);
}

static int EmitFace(const dface_t* f, const texinfo_t* ti, const world_tex_t* wt,
                    const surface_lm_t* sm,
                    const dvertex_t* verts, const dedge_t* edges, const int* surfedge,
                    int num_verts, world_vert_t* out)
{
    return EmitFaceEx(f, ti, wt, sm, verts, edges, surfedge, num_verts, out, false);
}

// Extended emit: when 'warp' is true, store RAW texel-space s,t in du,dv (the
// warp shader applies the turbsin distortion then divides by 64, matching
// ref_gl1 R_EmitWaterPolys). Opaque surfaces store normalized UV as before.
static int EmitFaceEx(const dface_t* f, const texinfo_t* ti, const world_tex_t* wt,
                    const surface_lm_t* sm,
                    const dvertex_t* verts, const dedge_t* edges, const int* surfedge,
                    int num_verts, world_vert_t* out, qboolean warp)
{
    #define MAX_FACE_EDGES 256

    // Warp surfaces: gather the polygon and subdivide into a <=64-unit grid so
    // the per-vertex turbsin warp ripples gently (matches ref_gl1).
    if (warp) {
        float poly[66*3];
        int nv = 0;
        for (int e = 0; e < f->numedges && nv < 64; e++) {
            const int se = surfedge[f->firstedge + e];
            int vi;
            if (se >= 0) vi = edges[se].v[0];
            else         vi = edges[-se].v[1];
            if (vi < 0 || vi >= num_verts) continue;
            const float* p = verts[vi].point;
            poly[nv*3+0]=p[0]; poly[nv*3+1]=p[1]; poly[nv*3+2]=p[2];
            nv++;
        }
        if (nv < 3) return 0;
        return SubdividePolygon(ti, wt, sm, nv, poly, out);
    }

    world_vert_t ring[MAX_FACE_EDGES];
    int n = 0;
    for (int e = 0; e < f->numedges && n < MAX_FACE_EDGES; e++) {
        const int se = surfedge[f->firstedge + e];
        int vi;
        if (se >= 0) vi = edges[se].v[0];
        else         vi = edges[-se].v[1];
        if (vi < 0 || vi >= num_verts) continue;

        const float* p = verts[vi].point;
        const float s = (p[0]*ti->vecs[0][0] + p[1]*ti->vecs[0][1] +
                         p[2]*ti->vecs[0][2] + ti->vecs[0][3]);
        const float t = (p[0]*ti->vecs[1][0] + p[1]*ti->vecs[1][1] +
                         p[2]*ti->vecs[1][2] + ti->vecs[1][3]);

        float du, dv;
        if (warp) {
            // Raw texel-space coords; warp shader does (s+turbsin)/64.
            du = s; dv = t;
        } else {
            du = s / (float)(wt->width  > 0 ? wt->width  : 64);
            dv = t / (float)(wt->height > 0 ? wt->height : 64);
        }

        float lu, lv;
        if (sm->has_lightmap) {
            lu = (s - sm->texturemins[0]) + (sm->lm_x * 16.0f) + 8.0f;
            lv = (t - sm->texturemins[1]) + (sm->lm_y * 16.0f) + 8.0f;
            lu /= (LM_ATLAS_W * 16.0f);
            lv /= (LM_ATLAS_H * 16.0f);
        } else {
            lu = 0.5f / LM_ATLAS_W;
            lv = 0.5f / LM_ATLAS_H;
        }

        ring[n].x = p[0]; ring[n].y = p[1]; ring[n].z = p[2];
        ring[n].du = du; ring[n].dv = dv;
        ring[n].lu = lu; ring[n].lv = lv;
        n++;
    }
    if (n < 3) return 0;

    int c = 0;
    for (int tri = 1; tri < n - 1; tri++) {
        out[c++] = ring[0];
        out[c++] = ring[tri];
        out[c++] = ring[tri+1];
    }
    return c;
}

// Bake one surface's lightmap into the CPU atlas at its slot, combining all of
// its styles scaled by the given lightstyle intensities and gl_modulate, with
// the desaturating peak clamp - a port of ref_gl1 R_BuildLightMap. 'styles' are
// the surface's style indices, 'samples' points at the first style's luxels
// (num_styles consecutive w*h*3 blocks). intensities is lightstyles[][3] (rgb).
static void BakeSurfaceLightmap(int lm_x, int lm_y, int w, int h,
                                const byte styles[LM_MAXSTYLES], int num_styles,
                                const byte* samples,
                                const float intensities[256][3], float modulate)
{
    if (!s_lm_cpu || !samples || w <= 0 || h <= 0) return;
    const int size = w * h;

    static float bl[34 * 34 * 3];
    if (size > 34 * 34) return;  // safety; H2 luxel blocks are <= 17x17
    for (int i = 0; i < size * 3; i++) bl[i] = 0.0f;

    const byte* lm = samples;
    for (int s = 0; s < num_styles; s++) {
        const int style = styles[s];
        float scale[3];
        if (style >= 0 && style < 256) {
            scale[0] = modulate * intensities[style][0];
            scale[1] = modulate * intensities[style][1];
            scale[2] = modulate * intensities[style][2];
        } else {
            scale[0] = scale[1] = scale[2] = modulate;
        }
        for (int i = 0; i < size; i++) {
            bl[i*3+0] += (float)lm[i*3+0] * scale[0];
            bl[i*3+1] += (float)lm[i*3+1] * scale[1];
            bl[i*3+2] += (float)lm[i*3+2] * scale[2];
        }
        lm += size * 3;
    }

    for (int t = 0; t < h; t++) {
        byte* dst = s_lm_cpu + (((lm_y + t) * LM_ATLAS_W) + lm_x) * 4;
        const float* b = &bl[t * w * 3];
        for (int j = 0; j < w; j++, dst += 4, b += 3) {
            int r = (int)b[0], g = (int)b[1], bb = (int)b[2];
            if (r < 0) r = 0;
            if (g < 0) g = 0;
            if (bb < 0) bb = 0;
            int mx = r; if (g > mx) mx = g; if (bb > mx) mx = bb;
            if (mx > 255) {
                const float ts = 255.0f / (float)mx;
                r = (int)(r * ts); g = (int)(g * ts); bb = (int)(bb * ts);
            }
            dst[0] = (byte)r; dst[1] = (byte)g; dst[2] = (byte)bb; dst[3] = 255;
        }
    }
}

// ---------------------------------------------------------------------------
// PVS / BSP-tree culling
// ---------------------------------------------------------------------------

static void VK_PVS_Free(void)
{
    free(s_pvs_nodes);     s_pvs_nodes = NULL;     s_pvs_num_nodes = 0;
    free(s_pvs_leaves);    s_pvs_leaves = NULL;    s_pvs_num_leaves = 0;
    free(s_pvs_planes);    s_pvs_planes = NULL;    s_pvs_num_planes = 0;
    free(s_pvs_leaffaces); s_pvs_leaffaces = NULL; s_pvs_num_leaffaces = 0;
    free(s_pvs_faces);     s_pvs_faces = NULL;     s_pvs_num_faces = 0;
    free(s_pvs_face_order); s_pvs_face_order = NULL; s_pvs_face_order_n = 0;
    free(s_pvs_visdata);   s_pvs_visdata = NULL;
    free(s_pvs_cluster_ofs); s_pvs_cluster_ofs = NULL;
    s_pvs_num_clusters = 0;
    s_pvs_visframe = 0;
    s_pvs_old_viewleaf = -1;
    s_pvs_ready = false;
}

// Recursively assign parent indices over the node tree (root = node 0).
static void PVS_SetParents(int nodenum, int parent)
{
    if (nodenum < 0) {
        const int leaf = -nodenum - 1;
        if (leaf >= 0 && leaf < s_pvs_num_leaves) s_pvs_leaves[leaf].parent = parent;
        return;
    }
    if (nodenum >= s_pvs_num_nodes) return;
    s_pvs_nodes[nodenum].parent = parent;
    PVS_SetParents(s_pvs_nodes[nodenum].children[0], nodenum);
    PVS_SetParents(s_pvs_nodes[nodenum].children[1], nodenum);
}

// Load the BSP tree (nodes, leaves, leaffaces) and visibility lump from the raw
// IBSP buffer for PVS culling. 'num_faces' must match the face array used to
// build s_pvs_faces. Returns false (PVS disabled, draw-all) on any problem.
static qboolean VK_PVS_Load(const byte* buf, int num_faces)
{
    const dheader_t* hdr = (const dheader_t*)buf;
    const lump_t* l_nodes  = &hdr->lumps[LUMP_NODES];
    const lump_t* l_leafs  = &hdr->lumps[LUMP_LEAFS];
    const lump_t* l_lfaces = &hdr->lumps[LUMP_LEAFFACES];
    const lump_t* l_planes = &hdr->lumps[LUMP_PLANES];
    const lump_t* l_vis    = &hdr->lumps[LUMP_VISIBILITY];

    const dnode_t*  nodes  = (const dnode_t*) GetLumpData(buf, l_nodes);
    const dleaf_t*  leaves = (const dleaf_t*) GetLumpData(buf, l_leafs);
    const ushort*   lfaces = (const ushort*)  GetLumpData(buf, l_lfaces);
    const dplane_t* planes = (const dplane_t*)GetLumpData(buf, l_planes);

    const int nn  = l_nodes->filelen  / (int)sizeof(dnode_t);
    const int nl  = l_leafs->filelen  / (int)sizeof(dleaf_t);
    const int nlf = l_lfaces->filelen / (int)sizeof(ushort);
    const int np  = l_planes->filelen / (int)sizeof(dplane_t);

    if (nn <= 0 || nl <= 0 || np <= 0) return false;

    s_pvs_nodes     = (pvs_node_t*)calloc(nn,  sizeof(pvs_node_t));
    s_pvs_leaves    = (pvs_leaf_t*)calloc(nl,  sizeof(pvs_leaf_t));
    s_pvs_planes    = (pvs_plane_t*)calloc(np, sizeof(pvs_plane_t));
    s_pvs_leaffaces = (int*)calloc(nlf > 0 ? nlf : 1, sizeof(int));
    s_pvs_faces     = (pvs_face_t*)calloc(num_faces > 0 ? num_faces : 1, sizeof(pvs_face_t));
    s_pvs_face_order = (int*)calloc(num_faces > 0 ? num_faces : 1, sizeof(int));
    if (!s_pvs_nodes || !s_pvs_leaves || !s_pvs_planes || !s_pvs_leaffaces || !s_pvs_faces || !s_pvs_face_order) {
        VK_PVS_Free(); return false;
    }
    s_pvs_num_nodes = nn; s_pvs_num_leaves = nl; s_pvs_num_planes = np;
    s_pvs_num_leaffaces = nlf; s_pvs_num_faces = num_faces;

    for (int i = 0; i < np; i++) {
        s_pvs_planes[i].normal[0] = planes[i].normal[0];
        s_pvs_planes[i].normal[1] = planes[i].normal[1];
        s_pvs_planes[i].normal[2] = planes[i].normal[2];
        s_pvs_planes[i].dist      = planes[i].dist;
    }
    for (int i = 0; i < nn; i++) {
        s_pvs_nodes[i].planenum    = nodes[i].planenum;
        s_pvs_nodes[i].children[0] = nodes[i].children[0];
        s_pvs_nodes[i].children[1] = nodes[i].children[1];
        s_pvs_nodes[i].parent      = -1;
        for (int k = 0; k < 3; k++) {
            s_pvs_nodes[i].mins[k] = (float)nodes[i].mins[k];
            s_pvs_nodes[i].maxs[k] = (float)nodes[i].maxs[k];
        }
    }
    for (int i = 0; i < nl; i++) {
        s_pvs_leaves[i].cluster        = leaves[i].cluster;
        s_pvs_leaves[i].first_leafface = leaves[i].firstleafface;
        s_pvs_leaves[i].num_leaffaces  = leaves[i].numleaffaces;
        s_pvs_leaves[i].parent         = -1;
        s_pvs_leaves[i].visframe       = -1;
        for (int k = 0; k < 3; k++) {
            s_pvs_leaves[i].mins[k] = (float)leaves[i].mins[k];
            s_pvs_leaves[i].maxs[k] = (float)leaves[i].maxs[k];
        }
    }
    for (int i = 0; i < nlf; i++)
        s_pvs_leaffaces[i] = lfaces[i];

    PVS_SetParents(0, -1);

    // Visibility lump: header (numclusters + bitofs[][2]) then compressed data.
    if (l_vis->filelen > (int)sizeof(int)) {
        const dvis_t* vis = (const dvis_t*)GetLumpData(buf, l_vis);
        s_pvs_num_clusters = vis->numclusters;
        if (s_pvs_num_clusters > 0) {
            s_pvs_cluster_ofs = (int*)calloc(s_pvs_num_clusters, sizeof(int));
            s_pvs_visdata = (byte*)malloc(l_vis->filelen);
            if (s_pvs_cluster_ofs && s_pvs_visdata) {
                memcpy(s_pvs_visdata, vis, l_vis->filelen);
                // bitofs is a flat [numclusters][2] table right after the
                // numclusters int; index it explicitly rather than via the
                // struct's fixed [8][2] declaration.
                const int* bitofs = (const int*)(s_pvs_visdata + sizeof(int));
                for (int i = 0; i < s_pvs_num_clusters; i++)
                    s_pvs_cluster_ofs[i] = bitofs[i * 2 + DVIS_PVS];
            } else {
                free(s_pvs_cluster_ofs); s_pvs_cluster_ofs = NULL;
                free(s_pvs_visdata);     s_pvs_visdata = NULL;
                s_pvs_num_clusters = 0;
            }
        }
    }

    s_pvs_ready = true;
    return true;
}

// Decompress the PVS bit-vector for a cluster into 'out' (RLE-zero expansion,
// standard Quake2 format). Returns out. If no vis data, marks all visible.
static byte* PVS_DecompressVis(int cluster, byte* out, int out_bytes)
{
    const int row = (s_pvs_num_clusters + 7) >> 3;  // bytes per cluster row
    if (cluster < 0 || !s_pvs_visdata || !s_pvs_cluster_ofs ||
        cluster >= s_pvs_num_clusters) {
        memset(out, 0xff, out_bytes);
        return out;
    }
    const byte* in = s_pvs_visdata + s_pvs_cluster_ofs[cluster];
    int written = 0;
    while (written < row && written < out_bytes) {
        if (*in) {                       // literal byte
            out[written++] = *in++;
            continue;
        }
        // zero run: next byte is the count of zero bytes
        in++;
        int c = *in++;
        if (written + c > out_bytes) c = out_bytes - written;
        while (c-- > 0) out[written++] = 0;
    }
    return out;
}

// Walk the BSP tree to find which leaf contains point p. Returns leaf index.
static int PVS_FindLeaf(const float p[3])
{
    if (!s_pvs_ready || s_pvs_num_nodes == 0) return -1;
    int nodenum = 0;
    while (nodenum >= 0) {
        const pvs_node_t* n = &s_pvs_nodes[nodenum];
        const pvs_plane_t* pl = &s_pvs_planes[n->planenum];
        const float d = p[0]*pl->normal[0] + p[1]*pl->normal[1] + p[2]*pl->normal[2] - pl->dist;
        nodenum = n->children[(d < 0.0f) ? 1 : 0];
    }
    return -nodenum - 1;   // leaf index
}

// --- Frustum culling -------------------------------------------------------
// 4 side planes (left/right/top/bottom) through the camera origin, mirroring
// ref_gl1 R_SetFrustum. A leaf box entirely behind any plane is off-screen.
typedef struct { float normal[3]; float dist; } frustum_plane_t;
static frustum_plane_t s_frustum[4];
static qboolean        s_frustum_valid = false;
static cvar_t*         s_r_nocull = NULL;

// Rodrigues rotation of 'point' around unit 'axis' by 'degrees' (ref_gl1
// RotatePointAroundVector equivalent).
static void RotateAroundVector(float out[3], const float axis[3],
                               const float point[3], float degrees)
{
    const float rad = degrees * (3.14159265358979f / 180.0f);
    const float c = cosf(rad), s = sinf(rad);
    const float d = axis[0]*point[0] + axis[1]*point[1] + axis[2]*point[2];
    // out = point*c + axis*d*(1-c) + (axis x point)*s
    const float cx = axis[1]*point[2] - axis[2]*point[1];
    const float cy = axis[2]*point[0] - axis[0]*point[2];
    const float cz = axis[0]*point[1] - axis[1]*point[0];
    out[0] = point[0]*c + axis[0]*d*(1.0f-c) + cx*s;
    out[1] = point[1]*c + axis[1]*d*(1.0f-c) + cy*s;
    out[2] = point[2]*c + axis[2]*d*(1.0f-c) + cz*s;
}

// Build the 4 frustum planes from the camera basis + field of view.
static void VK_Frustum_Setup(const float origin[3], const float fwd[3],
                             const float right[3], const float up[3],
                             float fov_x_deg, float fov_y_deg)
{
    RotateAroundVector(s_frustum[0].normal, up,    fwd, -(90.0f - fov_x_deg * 0.5f)); // right plane
    RotateAroundVector(s_frustum[1].normal, up,    fwd,  (90.0f - fov_x_deg * 0.5f)); // left plane
    RotateAroundVector(s_frustum[2].normal, right, fwd,  (90.0f - fov_y_deg * 0.5f)); // top plane
    RotateAroundVector(s_frustum[3].normal, right, fwd, -(90.0f - fov_y_deg * 0.5f)); // bottom plane
    for (int i = 0; i < 4; i++)
        s_frustum[i].dist = origin[0]*s_frustum[i].normal[0] +
                            origin[1]*s_frustum[i].normal[1] +
                            origin[2]*s_frustum[i].normal[2];
    s_frustum_valid = true;
}

// Return true if the box is entirely outside the frustum (cull it). Mirrors
// ref_gl1 R_CullBox (BoxOnPlaneSide == 2 on any plane = fully behind).
static qboolean VK_Frustum_CullBox(const float mins[3], const float maxs[3])
{
    if (!s_frustum_valid) return false;
    if (!s_r_nocull) s_r_nocull = ri.Cvar_Get("r_nocull", "0", 0);
    if (s_r_nocull && s_r_nocull->value != 0.0f) return false;

    for (int i = 0; i < 4; i++) {
        const frustum_plane_t* p = &s_frustum[i];
        // Pick the box corner most in the direction of the plane normal (the
        // "positive" corner). If even that is behind the plane, the whole box
        // is behind -> cull.
        float px = (p->normal[0] < 0) ? mins[0] : maxs[0];
        float py = (p->normal[1] < 0) ? mins[1] : maxs[1];
        float pz = (p->normal[2] < 0) ? mins[2] : maxs[2];
        const float d = px*p->normal[0] + py*p->normal[1] + pz*p->normal[2] - p->dist;
        if (d < 0.0f) return true;   // fully behind this plane
    }
    return false;
}

// Mark the faces in all leaves whose cluster is in the view cluster's PVS.
// Called once per frame before drawing. Sets s_pvs_faces[].visframe.
static void VK_PVS_Mark(const float vieworg[3])
{
    if (!s_pvs_ready) return;
    if (!s_r_novis) s_r_novis = ri.Cvar_Get("r_novis", "0", 0);

    const int viewleaf = PVS_FindLeaf(vieworg);
    const int viewcluster = (viewleaf >= 0 && viewleaf < s_pvs_num_leaves)
                            ? s_pvs_leaves[viewleaf].cluster : -1;

    const qboolean novis = (s_r_novis && s_r_novis->value != 0.0f) ||
                           viewcluster == -1 || s_pvs_visdata == NULL;

    // Run every frame: the frustum changes whenever the camera rotates (not just
    // when it crosses into a new leaf), so we can't cache by leaf like a pure-PVS
    // pass would. The per-leaf work below is cheap (a bitset test + box test).
    s_pvs_visframe++;
    s_pvs_old_viewleaf = viewleaf;

    if (novis) {
        // No PVS: still apply frustum culling per leaf (skip leaves fully
        // off-screen), but mark every face if frustum is also unavailable.
        if (!s_frustum_valid) {
            for (int i = 0; i < s_pvs_num_faces; i++)
                s_pvs_faces[i].visframe = s_pvs_visframe;
            return;
        }
        for (int i = 0; i < s_pvs_num_leaves; i++) {
            const pvs_leaf_t* leaf = &s_pvs_leaves[i];
            if (VK_Frustum_CullBox(leaf->mins, leaf->maxs)) continue;
            for (int k = 0; k < leaf->num_leaffaces; k++) {
                const int lf = leaf->first_leafface + k;
                if (lf < 0 || lf >= s_pvs_num_leaffaces) continue;
                const int fi = s_pvs_leaffaces[lf];
                if (fi >= 0 && fi < s_pvs_num_faces)
                    s_pvs_faces[fi].visframe = s_pvs_visframe;
            }
        }
        return;
    }

    static byte visrow[(MAX_MAP_LEAFS + 7) / 8];
    PVS_DecompressVis(viewcluster, visrow, sizeof(visrow));

    for (int i = 0; i < s_pvs_num_leaves; i++) {
        const pvs_leaf_t* leaf = &s_pvs_leaves[i];
        const int cl = leaf->cluster;
        if (cl < 0) continue;
        if (!(visrow[cl >> 3] & (1 << (cl & 7)))) continue;   // not in PVS
        if (VK_Frustum_CullBox(leaf->mins, leaf->maxs)) continue; // off-screen

        // Leaf is potentially visible AND on-screen: mark its faces.
        for (int k = 0; k < leaf->num_leaffaces; k++) {
            const int lf = leaf->first_leafface + k;
            if (lf < 0 || lf >= s_pvs_num_leaffaces) continue;
            const int fi = s_pvs_leaffaces[lf];
            if (fi >= 0 && fi < s_pvs_num_faces)
                s_pvs_faces[fi].visframe = s_pvs_visframe;
        }
    }
}

qboolean VK_World_LoadMap(const char* name)
{
    if (s_loaded && strcasecmp(s_loaded_name, name) == 0)
        return true;

    VK_World_Free();
    s_num_world_textures = 0;

    int len = ri.FS_LoadFile(name, (void**)&s_bsp_buffer);
    if (s_bsp_buffer == NULL || len < (int)sizeof(dheader_t)) {
        ri.Con_Printf(PRINT_ALL, "vk: world '%s' not found or too small (%d)\n", name, len);
        return false;
    }
    const dheader_t* hdr = (const dheader_t*)s_bsp_buffer;
    if (hdr->ident != IDBSPHEADER || hdr->version != BSPVERSION) {
        ri.Con_Printf(PRINT_ALL, "vk: world '%s' bad header\n", name);
        VK_World_Free(); return false;
    }

    const lump_t* l_verts    = &hdr->lumps[LUMP_VERTEXES];
    const lump_t* l_edges    = &hdr->lumps[LUMP_EDGES];
    const lump_t* l_surfedge = &hdr->lumps[LUMP_SURFEDGES];
    const lump_t* l_faces    = &hdr->lumps[LUMP_FACES];
    const lump_t* l_texinfo  = &hdr->lumps[LUMP_TEXINFO];
    const lump_t* l_lighting = &hdr->lumps[LUMP_LIGHTING];
    const lump_t* l_models   = &hdr->lumps[LUMP_MODELS];

    const dvertex_t* verts    = GetLumpData(s_bsp_buffer, l_verts);
    const dedge_t*   edges    = GetLumpData(s_bsp_buffer, l_edges);
    const int*       surfedge = GetLumpData(s_bsp_buffer, l_surfedge);
    const dface_t*   faces    = GetLumpData(s_bsp_buffer, l_faces);
    const texinfo_t* texinfo  = GetLumpData(s_bsp_buffer, l_texinfo);
    const byte*      lighting = GetLumpData(s_bsp_buffer, l_lighting);
    const dmodel_t*  models   = GetLumpData(s_bsp_buffer, l_models);

    const int num_verts    = l_verts->filelen   / (int)sizeof(dvertex_t);
    const int num_edges    = l_edges->filelen   / (int)sizeof(dedge_t);
    const int num_faces    = l_faces->filelen   / (int)sizeof(dface_t);
    const int num_texinfo  = l_texinfo->filelen / (int)sizeof(texinfo_t);
    const int lighting_len = l_lighting->filelen;
    const int num_models   = l_models->filelen  / (int)sizeof(dmodel_t);

    ri.Con_Printf(PRINT_ALL, "vk: BSP '%s': %d verts, %d faces, %d texinfo, %d lightbytes, %d models\n",
                  name, num_verts, num_faces, num_texinfo, lighting_len, num_models);

    // Record submodels (index 0 = world, 1+ = inline brush models). Mark which
    // faces belong to a submodel >= 1 so we can exclude them from the static
    // world and draw them as movable entities instead.
    s_num_submodels = (num_models < WORLD_MAX_SUBMODELS) ? num_models : WORLD_MAX_SUBMODELS;
    qboolean* face_is_submodel = (qboolean*)calloc(num_faces, sizeof(qboolean));
    for (int i = 0; i < s_num_submodels; i++) {
        s_submodels[i].first_face  = models[i].firstface;
        s_submodels[i].num_faces   = models[i].numfaces;
        s_submodels[i].origin[0]   = models[i].origin[0];
        s_submodels[i].origin[1]   = models[i].origin[1];
        s_submodels[i].origin[2]   = models[i].origin[2];
        s_submodels[i].first_batch = 0;
        s_submodels[i].num_batches = 0;
        if (i >= 1 && face_is_submodel) {
            for (int fnum = 0; fnum < models[i].numfaces; fnum++) {
                const int fi = models[i].firstface + fnum;
                if (fi >= 0 && fi < num_faces) face_is_submodel[fi] = true;
            }
        }
    }

    // Allocate the atlas CPU buffer (RGBA, alpha=255).
    s_lm_cpu = (byte*)calloc(LM_ATLAS_W * LM_ATLAS_H * 4, 1);
    if (!s_lm_cpu) { VK_World_Free(); return false; }
    // Reserve a 1x1 mid-gray fallback at (0,0). Set alpha=255 too.
    s_lm_cpu[0] = 128; s_lm_cpu[1] = 128; s_lm_cpu[2] = 128; s_lm_cpu[3] = 255;
    s_lm_row_x = 1; s_lm_row_y = 0; s_lm_row_h = 1;

    // Retain a copy of the lighting lump so animated lightstyles can re-bake
    // surfaces each frame (the source BSP buffer may be freed after load).
    if (lighting && lighting_len > 0) {
        s_lightsamples = (byte*)malloc(lighting_len);
        if (s_lightsamples) memcpy(s_lightsamples, lighting, lighting_len);
    }
    // Worst case every face is animated; allocated lazily below.
    s_anim_surfs = (lm_anim_surf_t*)calloc(num_faces, sizeof(lm_anim_surf_t));
    s_num_anim = 0;

    // Load the BSP tree + visibility for PVS culling. This allocates s_pvs_faces
    // (indexed by face) which the emit loop fills with per-face vertex ranges.
    // On failure PVS stays disabled and we draw everything (correct, just slower).
    VK_PVS_Load(s_bsp_buffer, num_faces);

    // Per-surface lightmap info (one entry per face, indexed by face index).
    surface_lm_t* slm = (surface_lm_t*)calloc(num_faces, sizeof(surface_lm_t));
    if (!slm) { VK_World_Free(); return false; }

    // First pass: compute extents, allocate lightmap atlas slots, copy data.
    uint32_t total_drawable_verts = 0;
    for (int i = 0; i < num_faces; i++) {
        const dface_t* f = &faces[i];
        if (f->numedges < 3) continue;
        if (f->texinfo < 0 || f->texinfo >= num_texinfo) continue;
        const texinfo_t* ti = &texinfo[f->texinfo];
        if (ti->flags & (SURF_NODRAW | SURF_SKY)) continue;
        if (ti->flags & SURF_WARP) {
            // Warp surfaces get subdivided into a <=64-unit grid; each cell
            // becomes a fan. Estimate generously from the surface's bounding
            // area: (cells+pad) * (avg ring fan verts). Use the texture-space
            // extents already computed below is not available yet, so bound by
            // world-space size of the face here.
            vec3_t fmins = {99999,99999,99999}, fmaxs = {-99999,-99999,-99999};
            for (int e = 0; e < f->numedges; e++) {
                const int se = surfedge[f->firstedge + e];
                int vi = (se >= 0) ? edges[se].v[0] : edges[-se].v[1];
                if (vi < 0 || vi >= num_verts) continue;
                const float* p = verts[vi].point;
                for (int k = 0; k < 3; k++) {
                    if (p[k] < fmins[k]) fmins[k] = p[k];
                    if (p[k] > fmaxs[k]) fmaxs[k] = p[k];
                }
            }
            // Cells along the two largest axes; each cell ~ a quad fan = up to
            // 6 ring verts -> 18 triangle-list verts. Pad heavily for safety.
            float dx = fmaxs[0]-fmins[0], dy = fmaxs[1]-fmins[1], dz = fmaxs[2]-fmins[2];
            int cx = (int)(dx / SUBDIVIDE_SIZE) + 2;
            int cy = (int)(dy / SUBDIVIDE_SIZE) + 2;
            int cz = (int)(dz / SUBDIVIDE_SIZE) + 2;
            // Use the two largest dimensions as the grid.
            int a = cx, b = cy;
            if (cz > a || cz > b) { if (a < b) a = cz; else b = cz; }
            total_drawable_verts += (uint32_t)(a * b) * 24 + 64;
        } else {
            total_drawable_verts += (uint32_t)(f->numedges - 2) * 3;
        }

        CalcExtents(f, verts, edges, surfedge, ti, num_verts, num_edges, &slm[i]);

        const int sw = (slm[i].extents[0] >> 4) + 1;
        const int sh = (slm[i].extents[1] >> 4) + 1;

        // Water/lava/trans surfaces are fullbright in H2 (SURF_FULLBRIGHT set
        // includes WARP/TRANS33/TRANS66). Don't apply a lightmap to them - it
        // makes them look wrong. Point them at the gray fallback.
        const qboolean fullbright = (ti->flags & (SURF_WARP | SURF_TRANS33 | SURF_TRANS66)) != 0;

        // Surfaces with light samples + within atlas bounds get a real slot.
        // Otherwise point them at the gray 1x1 fallback at (0,0).
        if (!fullbright && f->lightofs != -1 && sw > 0 && sh > 0 &&
            f->lightofs + sw*sh*3 <= lighting_len &&
            LMAtlas_Alloc(sw, sh, &slm[i].lm_x, &slm[i].lm_y))
        {
            slm[i].lm_w = sw;
            slm[i].lm_h = sh;
            slm[i].has_lightmap = true;

            // Count styles (styles[] entries != 255), matching the on-disk
            // [numstyles * surfsize] sample layout at lightofs.
            int num_styles = 0;
            for (int s = 0; s < LM_MAXSTYLES; s++) {
                if (f->styles[s] == 255) break;
                num_styles++;
            }
            if (num_styles < 1) num_styles = 1;

            // Bounds-check all style blocks before using them.
            const qboolean styles_fit =
                (f->lightofs + num_styles * sw * sh * 3 <= lighting_len);
            if (!styles_fit) num_styles = 1;

            // Initial bake with default intensities (style live values default
            // to 1.0). Build from all styles so the static look matches a frame
            // with the lights at rest.
            static float def_intens[256][3];
            static qboolean def_init = false;
            if (!def_init) {
                for (int s = 0; s < 256; s++) def_intens[s][0]=def_intens[s][1]=def_intens[s][2]=1.0f;
                def_init = true;
            }
            if (!s_gl_modulate_lm) s_gl_modulate_lm = ri.Cvar_Get("gl_modulate", "1", 0);
            const float modulate = s_gl_modulate_lm ? s_gl_modulate_lm->value : 1.0f;

            BakeSurfaceLightmap(slm[i].lm_x, slm[i].lm_y, sw, sh,
                                f->styles, num_styles, lighting + f->lightofs,
                                def_intens, modulate);

            // Register for per-frame animation if it has any animated style.
            // Style 0 is the constant "normal" map; styles 1+ are animated.
            // We register surfaces with >1 style OR a single non-zero style.
            qboolean animated = (num_styles > 1);
            for (int s = 0; s < num_styles; s++)
                if (f->styles[s] != 0 && f->styles[s] != 255) animated = true;
            if (animated && s_lightsamples) {
                lm_anim_surf_t* a = &s_anim_surfs[s_num_anim++];
                a->lm_x = slm[i].lm_x; a->lm_y = slm[i].lm_y;
                a->lm_w = sw; a->lm_h = sh;
                a->num_styles = num_styles;
                for (int s = 0; s < LM_MAXSTYLES; s++) a->styles[s] = f->styles[s];
                a->samples = s_lightsamples + f->lightofs;
            }
        } else {
            slm[i].lm_x = 0; slm[i].lm_y = 0;
            slm[i].lm_w = 1; slm[i].lm_h = 1;
            slm[i].has_lightmap = false;
        }
    }

    if (total_drawable_verts == 0) {
        free(slm); VK_World_Free(); return false;
    }

    // Resolve textures.
    for (int i = 0; i < num_texinfo; i++) {
        const int flags = texinfo[i].flags;
        if (flags & (SURF_NODRAW | SURF_SKY)) continue;
        WorldTex_Resolve(texinfo[i].texture);
    }

    // Upload lightmap atlas to GPU.
    if (!VK_CreateTextureRGBA(LM_ATLAS_W, LM_ATLAS_H, s_lm_cpu, &s_lm_tex)) {
        ri.Con_Printf(PRINT_ALL, "vk: lightmap atlas upload failed\n");
        free(slm); VK_World_Free(); return false;
    }

    // Allocate per-texture descriptors now that the atlas view exists.
    for (int i = 0; i < s_num_world_textures; i++) {
        world_tex_t* wt = &s_world_textures[i];
        wt->descriptor = VK_AllocWorldPairDescriptor(VK_ImageView(wt->image), s_lm_tex.view);
    }

    // Build VBO grouped by texture.
    world_vert_t* out = malloc(total_drawable_verts * sizeof(world_vert_t));
    if (!out) { free(slm); VK_World_Free(); return false; }

    uint32_t cursor = 0;
    s_num_batches = 0;

    for (int wt_i = 0; wt_i < s_num_world_textures; wt_i++) {
        world_tex_t* wt = &s_world_textures[wt_i];
        const uint32_t batch_start = cursor;
        const int batch_face_start = s_pvs_face_order_n;

        // Surface class for this texture's batch. Determined from the texinfo
        // flags of its faces (water textures are uniformly WARP, trans textures
        // uniformly TRANS33/66).
        int   batch_surf = 0;
        float batch_alpha = 1.0f;

        for (int i = 0; i < num_faces; i++) {
            if (face_is_submodel && face_is_submodel[i]) continue; // drawn as entity
            const dface_t* f = &faces[i];
            if (f->numedges < 3) continue;
            if (f->texinfo < 0 || f->texinfo >= num_texinfo) continue;
            const texinfo_t* ti = &texinfo[f->texinfo];
            if (ti->flags & (SURF_NODRAW | SURF_SKY)) continue;
            if (strcasecmp(ti->texture, wt->tex_name) != 0) continue;

            const qboolean is_warp = (ti->flags & SURF_WARP) != 0;
            batch_surf |= (ti->flags & (SURF_WARP | SURF_TRANS33 | SURF_TRANS66 |
                                        SURF_FLOWING | SURF_UNDULATE));
            if (ti->flags & SURF_TRANS33) batch_alpha = 0.33f;
            else if (ti->flags & SURF_TRANS66) batch_alpha = 0.66f;

            const uint32_t face_start = cursor;
            cursor += EmitFaceEx(f, ti, wt, &slm[i], verts, edges, surfedge, num_verts,
                                 out + cursor, is_warp);
            // Record this face's vertex sub-range + owning batch for PVS culling.
            if (s_pvs_faces && i < s_pvs_num_faces) {
                s_pvs_faces[i].batch        = s_num_batches; // batch appended below
                s_pvs_faces[i].first_vertex = face_start;
                s_pvs_faces[i].num_vertices = cursor - face_start;
                s_pvs_faces[i].visframe     = -1;
                // This face's OWN warp/flow/trans flags (per-face, like GL).
                s_pvs_faces[i].surf_flags   = (ti->flags & (SURF_WARP | SURF_TRANS33 |
                                               SURF_TRANS66 | SURF_FLOWING | SURF_UNDULATE));
                // World-space centroid of this face (for alpha back-to-front sort).
                double fx = 0, fy = 0, fz = 0;
                const uint32_t fn = cursor - face_start;
                for (uint32_t v = face_start; v < cursor; v++) {
                    fx += out[v].x; fy += out[v].y; fz += out[v].z;
                }
                if (fn > 0) {
                    s_pvs_faces[i].center[0] = (float)(fx / fn);
                    s_pvs_faces[i].center[1] = (float)(fy / fn);
                    s_pvs_faces[i].center[2] = (float)(fz / fn);
                }
                if (s_pvs_face_order && s_pvs_face_order_n < s_pvs_num_faces)
                    s_pvs_face_order[s_pvs_face_order_n++] = i;
            }
        }
        const uint32_t batch_count = cursor - batch_start;
        if (batch_count > 0 && s_num_batches < WORLD_MAX_BATCHES) {
            s_batches[s_num_batches].descriptor   = wt->descriptor;
            s_batches[s_num_batches].first_vertex = batch_start;
            s_batches[s_num_batches].num_vertices = batch_count;
            s_batches[s_num_batches].surf_flags   = batch_surf;
            s_batches[s_num_batches].alpha        = batch_alpha;
            // Texture animation chain (e.g. animated waterfall/flowing textures
            // that cycle frames over time via SURF_ANIMSPEED, and func_* frame
            // swaps). Built the same way as for submodels below.
            s_batches[s_num_batches].num_frames   = 1;
            s_batches[s_num_batches].frame_desc[0] = wt->descriptor;
            s_batches[s_num_batches].anim_speed   = 0;
            {
                int start_ti = -1;
                for (int i2 = 0; i2 < num_faces; i2++) {
                    if (face_is_submodel && face_is_submodel[i2]) continue;
                    const dface_t* f2 = &faces[i2];
                    if (f2->numedges < 3) continue;
                    if (f2->texinfo < 0 || f2->texinfo >= num_texinfo) continue;
                    if (strcasecmp(texinfo[f2->texinfo].texture, wt->tex_name) != 0) continue;
                    start_ti = f2->texinfo;
                    break;
                }
                if (start_ti >= 0) {
                    if (texinfo[start_ti].flags & SURF_ANIMSPEED)
                        s_batches[s_num_batches].anim_speed = texinfo[start_ti].value;
                    int cur = texinfo[start_ti].nexttexinfo;
                    int guard = 0;
                    while (cur > 0 && cur < num_texinfo && cur != start_ti &&
                           s_batches[s_num_batches].num_frames < 8 && guard++ < 64) {
                        world_tex_t* fwt = WorldTex_Resolve(texinfo[cur].texture);
                        s_batches[s_num_batches].frame_desc[s_batches[s_num_batches].num_frames++] =
                            (fwt && fwt->descriptor) ? fwt->descriptor : wt->descriptor;
                        cur = texinfo[cur].nexttexinfo;
                    }
                }
            }
            s_batch_face_start[s_num_batches] = batch_face_start;
            s_batch_face_count[s_num_batches] = s_pvs_face_order_n - batch_face_start;
            // World-space centroid of this batch's verts (for alpha sorting).
            double cx = 0, cy = 0, cz = 0;
            for (uint32_t v = batch_start; v < cursor; v++) {
                cx += out[v].x; cy += out[v].y; cz += out[v].z;
            }
            s_batches[s_num_batches].center[0] = (float)(cx / batch_count);
            s_batches[s_num_batches].center[1] = (float)(cy / batch_count);
            s_batches[s_num_batches].center[2] = (float)(cz / batch_count);
            s_num_batches++;
        }
    }
    // ----- Build inline submodel geometry (doors/lifts) into s_sub_vbo -----
    // Each submodel >= 1 gets its own batches. Vertices stay in world space
    // (BSP coords); at render time we offset by the entity origin so a door
    // can move. Group each submodel's faces by texture for batching.
    s_num_sub_batches = 0;
    s_sub_total_verts = 0;
    world_vert_t* sub_out = NULL;

    uint32_t sub_total = 0;
    for (int i = 1; i < s_num_submodels; i++) {
        for (int fnum = 0; fnum < s_submodels[i].num_faces; fnum++) {
            const int fi = s_submodels[i].first_face + fnum;
            if (fi < 0 || fi >= num_faces) continue;
            const dface_t* f = &faces[fi];
            if (f->numedges < 3) continue;
            if (f->texinfo < 0 || f->texinfo >= num_texinfo) continue;
            const texinfo_t* ti = &texinfo[f->texinfo];
            if (ti->flags & (SURF_NODRAW | SURF_SKY)) continue;
            if (ti->flags & SURF_WARP) {
                // Warp surfaces subdivide into a <=64-unit grid; estimate from
                // the face's world-space bounding box (same as the world path).
                vec3_t fmins = {99999,99999,99999}, fmaxs = {-99999,-99999,-99999};
                for (int e = 0; e < f->numedges; e++) {
                    const int se = surfedge[f->firstedge + e];
                    int vi = (se >= 0) ? edges[se].v[0] : edges[-se].v[1];
                    if (vi < 0 || vi >= num_verts) continue;
                    const float* p = verts[vi].point;
                    for (int k = 0; k < 3; k++) {
                        if (p[k] < fmins[k]) fmins[k] = p[k];
                        if (p[k] > fmaxs[k]) fmaxs[k] = p[k];
                    }
                }
                float dx = fmaxs[0]-fmins[0], dy = fmaxs[1]-fmins[1], dz = fmaxs[2]-fmins[2];
                int cx = (int)(dx / SUBDIVIDE_SIZE) + 2;
                int cy = (int)(dy / SUBDIVIDE_SIZE) + 2;
                int cz = (int)(dz / SUBDIVIDE_SIZE) + 2;
                int a = cx, b = cy;
                if (cz > a || cz > b) { if (a < b) a = cz; else b = cz; }
                sub_total += (uint32_t)(a * b) * 24 + 64;
            } else {
                sub_total += (uint32_t)(f->numedges - 2) * 3;
            }
        }
    }

    if (sub_total > 0) sub_out = malloc(sub_total * sizeof(world_vert_t));

    uint32_t sub_cursor = 0;
    if (sub_out) {
        for (int i = 1; i < s_num_submodels; i++) {
            s_submodels[i].first_batch = s_num_sub_batches;
            s_submodels[i].num_batches = 0;

            for (int wt_i = 0; wt_i < s_num_world_textures; wt_i++) {
                world_tex_t* wt = &s_world_textures[wt_i];
                const uint32_t bstart = sub_cursor;

                int   sub_surf  = 0;
                float sub_alpha = 1.0f;
                for (int fnum = 0; fnum < s_submodels[i].num_faces; fnum++) {
                    const int fi = s_submodels[i].first_face + fnum;
                    if (fi < 0 || fi >= num_faces) continue;
                    const dface_t* f = &faces[fi];
                    if (f->numedges < 3) continue;
                    if (f->texinfo < 0 || f->texinfo >= num_texinfo) continue;
                    const texinfo_t* ti = &texinfo[f->texinfo];
                    if (ti->flags & (SURF_NODRAW | SURF_SKY)) continue;
                    if (strcasecmp(ti->texture, wt->tex_name) != 0) continue;

                    sub_surf |= (ti->flags & (SURF_WARP | SURF_TRANS33 | SURF_TRANS66 |
                                              SURF_FLOWING | SURF_UNDULATE));
                    if (ti->flags & SURF_TRANS33) sub_alpha = 0.33f;
                    else if (ti->flags & SURF_TRANS66) sub_alpha = 0.66f;

                    // SURF_WARP submodels (forcefields, energy barriers) must be
                    // emitted like world warp surfaces: subdivided grid + raw
                    // texel UVs, so the warp shader's turbulence ripples them
                    // correctly. Plain EmitFace (normalized UVs, no subdivision)
                    // fed to the warp shader produced a washed-out smear.
                    const qboolean sub_is_warp = (ti->flags & SURF_WARP) != 0;
                    sub_cursor += EmitFaceEx(f, ti, wt, &slm[fi], verts, edges, surfedge,
                                             num_verts, sub_out + sub_cursor, sub_is_warp);
                }

                const uint32_t bcount = sub_cursor - bstart;
                if (bcount > 0 && s_num_sub_batches < WORLD_MAX_BATCHES) {
                    world_batch_t* wb = &s_sub_batches[s_num_sub_batches];
                    wb->descriptor   = wt->descriptor;
                    wb->first_vertex = bstart;
                    wb->num_vertices = bcount;
                    wb->surf_flags   = sub_surf;
                    wb->alpha        = sub_alpha;

                    // Resolve the texture animation chain for this batch so
                    // func_button/door frame swaps work. Find the texinfo that
                    // produced this batch's texture, then walk nexttexinfo,
                    // collecting each frame's descriptor.
                    wb->num_frames = 1;
                    wb->frame_desc[0] = wt->descriptor;
                    wb->anim_speed = 0;
                    {
                        // Find a representative texinfo for this submodel+texture.
                        int start_ti = -1;
                        for (int fnum = 0; fnum < s_submodels[i].num_faces; fnum++) {
                            const int fi = s_submodels[i].first_face + fnum;
                            if (fi < 0 || fi >= num_faces) continue;
                            const dface_t* f = &faces[fi];
                            if (f->texinfo < 0 || f->texinfo >= num_texinfo) continue;
                            if (strcasecmp(texinfo[f->texinfo].texture, wt->tex_name) != 0) continue;
                            start_ti = f->texinfo;
                            break;
                        }
                        if (start_ti >= 0) {
                            // SURF_ANIMSPEED: texinfo 'value'/num_frames holds the
                            // animation fps; frame is driven by time, not entity
                            // frame (forcefields, energy barriers, etc).
                            if (texinfo[start_ti].flags & SURF_ANIMSPEED)
                                wb->anim_speed = texinfo[start_ti].value;
                            int cur = texinfo[start_ti].nexttexinfo;
                            int guard = 0;
                            while (cur > 0 && cur < num_texinfo && cur != start_ti &&
                                   wb->num_frames < 8 && guard++ < 64) {
                                world_tex_t* fwt = WorldTex_Resolve(texinfo[cur].texture);
                                wb->frame_desc[wb->num_frames++] =
                                    (fwt && fwt->descriptor) ? fwt->descriptor : wt->descriptor;
                                cur = texinfo[cur].nexttexinfo;
                            }
                        }
                    }

                    s_num_sub_batches++;
                    s_submodels[i].num_batches++;

                    // Diagnostic: log translucent/animated submodel batches (e.g.
                    // the forcefield) so their real flags/anim/alpha can be seen.
                    if (sub_surf != 0) {
                        ri.Con_Printf(PRINT_ALL,
                            "vk: SUBMODEL trans batch tex='%s' surf=0x%x alpha=%.2f frames=%d animspeed=%d\n",
                            wt->tex_name, sub_surf, sub_alpha, wb->num_frames, wb->anim_speed);
                    }
                }
            }
        }
        s_sub_total_verts = sub_cursor;
    }

    free(slm);
    free(face_is_submodel);

    if (cursor == 0) { free(out); free(sub_out); VK_World_Free(); return false; }

    const VkDeviceSize bytes = (VkDeviceSize)cursor * sizeof(world_vert_t);

    vk_buffer_t staging = {0};
    if (!VK_CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &staging)) {
        free(out); VK_World_Free(); return false;
    }
    VK_MapBuffer(&staging);
    memcpy(staging.mapped, out, (size_t)bytes);
    free(out);

    if (!VK_CreateBuffer(bytes,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &s_vbo)) {
        VK_DestroyBuffer(&staging); VK_World_Free(); return false;
    }

    VkCommandBuffer cb = VK_BeginOneShotCmd();
    VkBufferCopy copy = { 0, 0, bytes };
    vkCmdCopyBuffer(cb, staging.buffer, s_vbo.buffer, 1, &copy);
    VK_EndOneShotCmd(cb);
    VK_DestroyBuffer(&staging);

    // Upload submodel geometry, if any.
    if (sub_out && sub_cursor > 0) {
        const VkDeviceSize sbytes = (VkDeviceSize)sub_cursor * sizeof(world_vert_t);
        vk_buffer_t sstaging = {0};
        if (VK_CreateBuffer(sbytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &sstaging)) {
            VK_MapBuffer(&sstaging);
            memcpy(sstaging.mapped, sub_out, (size_t)sbytes);
            if (VK_CreateBuffer(sbytes,
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &s_sub_vbo)) {
                VkCommandBuffer cb2 = VK_BeginOneShotCmd();
                VkBufferCopy scopy = { 0, 0, sbytes };
                vkCmdCopyBuffer(cb2, sstaging.buffer, s_sub_vbo.buffer, 1, &scopy);
                VK_EndOneShotCmd(cb2);
            }
            VK_DestroyBuffer(&sstaging);
        }
    }
    free(sub_out);

    s_total_verts = cursor;
    strcpy_s(s_loaded_name, sizeof(s_loaded_name), name);
    s_loaded = true;

    // Parse lighting lumps for point-sampling (drives r_lightlevel -> AI sight)
    // before we release the BSP image.
    VK_LightPoint_Load(s_bsp_buffer);

    ri.FS_FreeFile(s_bsp_buffer);
    s_bsp_buffer = NULL;

    // Keep the CPU atlas (and the retained light samples) alive if any surfaces
    // have animated lightstyles - the per-frame update re-bakes into it. If
    // nothing animates, free them now to save memory.
    if (s_num_anim == 0) {
        free(s_lm_cpu);     s_lm_cpu = NULL;
        free(s_lightsamples); s_lightsamples = NULL;
        free(s_anim_surfs); s_anim_surfs = NULL;
    }

    ri.Con_Printf(PRINT_ALL, "vk: world '%s' loaded: %u verts, %d textures, %d batches, %d submodels (%u sub-verts), lightmap atlas %dx%d\n",
                  name, s_total_verts, s_num_world_textures, s_num_batches,
                  s_num_submodels - 1, s_sub_total_verts, LM_ATLAS_W, LM_ATLAS_H);
    return true;
}

// ---------------------------------------------------------------------------
// Matrix math (column-major; Vulkan clip space: depth 0..1, y-down)
// ---------------------------------------------------------------------------

typedef float mat4_t[16];

static void mat4_identity(mat4_t out)
{
    memset(out, 0, sizeof(mat4_t));
    out[0] = out[5] = out[10] = out[15] = 1.0f;
}

static void mat4_multiply(mat4_t out, const mat4_t a, const mat4_t b)
{
    mat4_t r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k*4 + rr] * b[c*4 + k];
            r[c*4 + rr] = s;
        }
    memcpy(out, r, sizeof(r));
}

static void mat4_perspective(mat4_t out, float fov_y_rad, float aspect, float zn, float zf)
{
    const float f = 1.0f / tanf(fov_y_rad * 0.5f);
    memset(out, 0, sizeof(mat4_t));
    out[0]  =  f / aspect;
    out[5]  = -f;
    out[10] = zf / (zn - zf);
    out[11] = -1.0f;
    out[14] = (zn * zf) / (zn - zf);
}

static void mat4_rotate(mat4_t out, float deg, float ax, float ay, float az)
{
    const float rad = deg * (3.14159265358979f / 180.0f);
    const float c = cosf(rad), s = sinf(rad);
    const float omc = 1.0f - c;
    const float len = sqrtf(ax*ax + ay*ay + az*az);
    if (len > 0.0001f) { ax/=len; ay/=len; az/=len; }
    mat4_identity(out);
    out[0]=c+ax*ax*omc;    out[1]=ay*ax*omc+az*s; out[2]=az*ax*omc-ay*s;
    out[4]=ax*ay*omc-az*s; out[5]=c+ay*ay*omc;    out[6]=az*ay*omc+ax*s;
    out[8]=ax*az*omc+ay*s; out[9]=ay*az*omc-ax*s; out[10]=c+az*az*omc;
}

static void mat4_translate(mat4_t out, float x, float y, float z)
{
    mat4_identity(out);
    out[12] = x; out[13] = y; out[14] = z;
}

static void make_view_matrix(mat4_t out, const vec3_t origin, const vec3_t angles)
{
    mat4_t m, tmp, r;
    mat4_rotate(m, -90.0f, 1,0,0);
    mat4_rotate(r,  90.0f, 0,0,1); mat4_multiply(tmp, m, r); memcpy(m, tmp, sizeof(m));
    mat4_rotate(r, -angles[2], 1,0,0); mat4_multiply(tmp, m, r); memcpy(m, tmp, sizeof(m));
    mat4_rotate(r, -angles[0], 0,1,0); mat4_multiply(tmp, m, r); memcpy(m, tmp, sizeof(m));
    mat4_rotate(r, -angles[1], 0,0,1); mat4_multiply(tmp, m, r); memcpy(m, tmp, sizeof(m));
    mat4_translate(r, -origin[0], -origin[1], -origin[2]);
    mat4_multiply(out, m, r);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Animated lightstyles: re-bake + upload surfaces whose styles changed.
// Call once per frame BEFORE the world is drawn. Captures the refdef's live
// lightstyle intensities, and for each registered animated surface whose styles
// changed since last frame, rebuilds its atlas slot and uploads just that slot.
// ---------------------------------------------------------------------------
// Upload the given dirty animated-surface slots from the CPU atlas to the GPU
// atlas in a single batched transfer: one staging buffer holding all regions
// packed back-to-back, one command buffer with one copy per region, one fence.
static void VK_World_UploadLightmapRegions(const int* dirty, int ndirty)
{
    if (ndirty <= 0 || !s_lm_tex.image || !s_lm_cpu) return;

    // Build the region list (slots are already re-baked into s_lm_cpu) and hand
    // it to the batched texture-update path in vk_buffer.c (which owns the image
    // layout transitions). Source is the full CPU atlas, src_w = LM_ATLAS_W.
    vk_tex_region_t* regions = (vk_tex_region_t*)calloc(ndirty, sizeof(vk_tex_region_t));
    if (!regions) return;
    for (int k = 0; k < ndirty; k++) {
        const lm_anim_surf_t* a = &s_anim_surfs[dirty[k]];
        regions[k].x = a->lm_x; regions[k].y = a->lm_y;
        regions[k].w = a->lm_w; regions[k].h = a->lm_h;
        regions[k].src_x = a->lm_x; regions[k].src_y = a->lm_y;
    }
    VK_UpdateTextureRegions(&s_lm_tex, s_lm_cpu, LM_ATLAS_W, regions, ndirty);
    free(regions);
}

void VK_World_UpdateLightstyles(const refdef_t* fd)
{
    if (!s_loaded || !fd || !fd->lightstyles) return;
    if (!s_anim_surfs || s_num_anim == 0 || !s_lm_cpu || !s_lm_tex.image) return;

    if (!s_gl_modulate_lm) s_gl_modulate_lm = ri.Cvar_Get("gl_modulate", "1", 0);
    const float modulate = s_gl_modulate_lm ? s_gl_modulate_lm->value : 1.0f;

    for (int i = 0; i < 256; i++) {
        s_lightstyles[i][0] = fd->lightstyles[i].rgb[0];
        s_lightstyles[i][1] = fd->lightstyles[i].rgb[1];
        s_lightstyles[i][2] = fd->lightstyles[i].rgb[2];
    }

    const qboolean first = !s_lightstyles_valid;

    // Collect the surfaces that changed this frame, re-baking them into the CPU
    // atlas, then upload them all in ONE batched transfer (one staging buffer,
    // one command submit, one fence wait) to avoid a GPU round-trip per surface.
    int* dirty = (int*)alloca(sizeof(int) * s_num_anim);
    int  ndirty = 0;

    for (int i = 0; i < s_num_anim; i++) {
        const lm_anim_surf_t* a = &s_anim_surfs[i];
        qboolean changed = first;
        if (!changed) {
            for (int s = 0; s < a->num_styles; s++) {
                const int st = a->styles[s];
                if (st < 0 || st >= 256) continue;
                if (s_lightstyles[st][0] != s_lightstyles_prev[st][0] ||
                    s_lightstyles[st][1] != s_lightstyles_prev[st][1] ||
                    s_lightstyles[st][2] != s_lightstyles_prev[st][2]) {
                    changed = true; break;
                }
            }
        }
        if (!changed) continue;

        BakeSurfaceLightmap(a->lm_x, a->lm_y, a->lm_w, a->lm_h,
                            a->styles, a->num_styles, a->samples,
                            s_lightstyles, modulate);
        dirty[ndirty++] = i;
    }

    if (ndirty > 0)
        VK_World_UploadLightmapRegions(dirty, ndirty);

    memcpy(s_lightstyles_prev, s_lightstyles, sizeof(s_lightstyles));
    s_lightstyles_valid = true;
}

// Pick the descriptor for a world batch this frame, honoring its texture
// animation chain. SURF_ANIMSPEED textures (animated waterfalls/flowing
// textures) cycle by wall-clock time; func_* swaps would use entity frame, but
// world batches have no entity, so non-ANIMSPEED multi-frame chains just stay on
// frame 0. Matches ref_gl1 R_TextureAnimation.
static VkDescriptorSet WorldBatchFrameDesc(const world_batch_t* wb)
{
    if (wb->num_frames <= 1) return wb->descriptor;
    int fr = 0;
    if (wb->anim_speed > 0)
        fr = (int)(s_frame_time * (float)wb->anim_speed);
    fr %= wb->num_frames;
    if (fr < 0) fr += wb->num_frames;
    VkDescriptorSet d = wb->frame_desc[fr];
    return (d != VK_NULL_HANDLE) ? d : wb->descriptor;
}

void VK_World_Render(const refdef_t* fd)
{
    if (!s_loaded || s_total_verts == 0 || !fd) return;
    if (!vk_state.frame_started) return;

    s_view_origin_world[0] = fd->vieworg[0];
    s_view_origin_world[1] = fd->vieworg[1];
    s_view_origin_world[2] = fd->vieworg[2];
    s_frame_time = fd->time;

    const uint32_t frame = vk_state.current_frame;
    VkCommandBuffer cb = vk_state.command_buffers[frame];

    const float aspect = (fd->width > 0 && fd->height > 0)
                       ? (float)fd->width / (float)fd->height : 1.0f;
    const float fov_y  = fd->fov_y * (3.14159265358979f / 180.0f);

    mat4_t view, proj, mvp;
    make_view_matrix(view, fd->vieworg, fd->viewangles);
    // Match ref_gl1 R_SetPerspective: near 1.0 (H2 lowered from Q2's 4.0), far
    // 4096 (r_farclipdist default). A 4.0 near plane clips world surfaces the
    // camera is close to, making nearby walls see-through under VK.
    mat4_perspective(proj, fov_y, aspect, 1.0f, 4096.0f);
    mat4_multiply(mvp, proj, view);

    VkViewport vp = {0};
    vp.x = (float)fd->x; vp.y = (float)fd->y;
    vp.width = (float)fd->width; vp.height = (float)fd->height;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    VkRect2D sc = {0};
    sc.offset.x = fd->x; sc.offset.y = fd->y;
    sc.extent.width = (uint32_t)fd->width; sc.extent.height = (uint32_t)fd->height;
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_world.pipeline);
    {
        float wpc[28];
        memcpy(wpc, mvp, sizeof(mvp));        // mat4 [0..15]
        FillFogParams(wpc, fd->vieworg);      // fog tail [16..27]
        wpc[26] = 1.0f;                       // fog_extra.z: enable dynamic lights
        wpc[27] = 0.0f;                       // fog_extra.w: unused (world)
        vkCmdPushConstants(cb, vk_pipeline_world.layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(wpc), wpc);
    }

    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &s_vbo.buffer, &zero);

    // Dynamic lights: fill this frame's UBO and bind it at set = 1.
    VkDescriptorSet dlset = BuildAndBindDlights(fd);
    if (dlset != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vk_pipeline_world.layout, 1, 1, &dlset, 0, NULL);

    // Build the view frustum (4 side planes) for off-screen leaf culling, then
    // mark which faces are visible this frame (PVS + frustum).
    {
        const float DEG2RAD = 3.14159265358979f / 180.0f;
        const float p = fd->viewangles[0]*DEG2RAD, y = fd->viewangles[1]*DEG2RAD, r = fd->viewangles[2]*DEG2RAD;
        const float cp=cosf(p),sp=sinf(p), cy=cosf(y),sy=sinf(y), cr=cosf(r),sr=sinf(r);
        // AngleVectors basis (matches make_view_matrix / ViewUpRight).
        float fwd[3]   = { cp*cy, cp*sy, -sp };
        float right[3] = { -sr*sp*cy + -cr*-sy, -sr*sp*sy + -cr*cy, -sr*cp };
        float up[3]    = {  cr*sp*cy + -sr*-sy,  cr*sp*sy + -sr*cy,  cr*cp };
        const float fov_y_deg = fd->fov_y;
        const float fov_x_deg = 2.0f * atanf(tanf(fov_y_deg * 0.5f * DEG2RAD) * aspect) / DEG2RAD;
        VK_Frustum_Setup(fd->vieworg, fwd, right, up, fov_x_deg, fov_y_deg);
    }
    VK_PVS_Mark(fd->vieworg);

    if (s_pvs_ready && s_pvs_faces && s_pvs_face_order) {
        // Visible-only pass: for each opaque batch, walk just its own faces (in
        // emission order) and draw merged runs of visible ones. O(total faces).
        for (int b = 0; b < s_num_batches; b++) {
            if (s_batches[b].descriptor == VK_NULL_HANDLE) continue;
            if (s_batches[b].surf_flags != 0) continue;   // warp/trans later
            const int fstart = s_batch_face_start[b];
            const int fcount = s_batch_face_count[b];
            qboolean bound = false;
            uint32_t run_start = 0, run_count = 0;
            for (int k = 0; k < fcount; k++) {
                const pvs_face_t* pf = &s_pvs_faces[s_pvs_face_order[fstart + k]];
                const qboolean vis = (pf->num_vertices > 0 &&
                                      pf->visframe == s_pvs_visframe);
                if (vis && run_count > 0 && pf->first_vertex == run_start + run_count) {
                    run_count += pf->num_vertices;
                    continue;
                }
                if (run_count > 0) {
                    if (!bound) {
                        VkDescriptorSet bdesc = WorldBatchFrameDesc(&s_batches[b]);
                        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk_pipeline_world.layout, 0, 1, &bdesc, 0, NULL);
                        bound = true;
                    }
                    vkCmdDraw(cb, run_count, 1, run_start, 0);
                    run_count = 0;
                }
                if (vis) { run_start = pf->first_vertex; run_count = pf->num_vertices; }
            }
            if (run_count > 0) {
                if (!bound) {
                    VkDescriptorSet bdesc = WorldBatchFrameDesc(&s_batches[b]);
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        vk_pipeline_world.layout, 0, 1, &bdesc, 0, NULL);
                }
                vkCmdDraw(cb, run_count, 1, run_start, 0);
            }
        }
    } else {
        // No PVS data (or disabled): draw every opaque batch wholesale.
        for (int i = 0; i < s_num_batches; i++) {
            if (s_batches[i].descriptor == VK_NULL_HANDLE) continue;
            if (s_batches[i].surf_flags != 0) continue;
            VkDescriptorSet bdesc = WorldBatchFrameDesc(&s_batches[i]);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    vk_pipeline_world.layout, 0, 1, &bdesc, 0, NULL);
            vkCmdDraw(cb, s_batches[i].num_vertices, 1, s_batches[i].first_vertex, 0);
        }
    }

    vk_state.pipeline_2d_bound = false;
}

// Translucent / warp world surfaces (water, lava, glass). MUST be called AFTER
// all opaque geometry (world + entities + submodels), matching ref_gl1's
// R_SortAndDrawAlphaSurfaces which runs after R_DrawEntitiesOnList with
// depthMask off. Drawing this inside VK_World_Render (before entities) made
// submerged/behind entities paint over the water instead of being seen through
// it, because the water pass doesn't write depth.
void VK_World_RenderWater(const refdef_t* fd,
                          struct entity_s** alpha_ents, int num_alpha_ents,
                          VK_DrawAlphaEntityFn draw_entity, void* user)
{
    // If there are no translucent world surfaces, we still need to draw the
    // alpha entities (they were deferred from the opaque pass for interleaving).
    if (!s_loaded || s_total_verts == 0 || !fd) {
        if (draw_entity && alpha_ents)
            for (int i = 0; i < num_alpha_ents; i++)
                if (alpha_ents[i]) draw_entity(alpha_ents[i], user);
        return;
    }
    if (!vk_state.frame_started) return;

    // Any translucent/warp batches at all?
    int have = 0;
    for (int i = 0; i < s_num_batches; i++)
        if (s_batches[i].descriptor != VK_NULL_HANDLE && s_batches[i].surf_flags != 0) { have = 1; break; }
    if (!have) {
        // No translucent world surfaces this frame; just draw the deferred
        // alpha entities (already sorted back-to-front by the client).
        if (draw_entity && alpha_ents)
            for (int i = 0; i < num_alpha_ents; i++)
                if (alpha_ents[i]) draw_entity(alpha_ents[i], user);
        return;
    }

    const uint32_t frame = vk_state.current_frame;
    VkCommandBuffer cb = vk_state.command_buffers[frame];

    const float aspect = (fd->width > 0 && fd->height > 0)
                       ? (float)fd->width / (float)fd->height : 1.0f;
    const float fov_y  = fd->fov_y * (3.14159265358979f / 180.0f);

    mat4_t view, proj, mvp;
    make_view_matrix(view, fd->vieworg, fd->viewangles);
    mat4_perspective(proj, fov_y, aspect, 1.0f, 4096.0f);
    mat4_multiply(mvp, proj, view);

    VkViewport vp = {0};
    vp.x = (float)fd->x; vp.y = (float)fd->y;
    vp.width = (float)fd->width; vp.height = (float)fd->height;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    VkRect2D sc = {0};
    sc.offset.x = fd->x; sc.offset.y = fd->y;
    sc.extent.width = (uint32_t)fd->width; sc.extent.height = (uint32_t)fd->height;
    vkCmdSetScissor(cb, 0, 1, &sc);

    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &s_vbo.buffer, &zero);

    float warp_pc[36];
    memcpy(warp_pc, mvp, sizeof(mvp));   // [0..15]
    FillFogParamsAt(warp_pc, 24, fd->vieworg);   // fog tail [24..35]

    if (!s_gl_trans33) s_gl_trans33 = ri.Cvar_Get("gl_trans33", "0.33", 0);
    if (!s_gl_trans66) s_gl_trans66 = ri.Cvar_Get("gl_trans66", "0.66", 0);

    // The warp pipeline layout includes set = 1 (dlight UBO); bind a valid set
    // even though warp surfaces are fullbright and the shader ignores it.
    VkDescriptorSet dlset = BuildAndBindDlights(fd);

    // Collect translucent FACES and sort back-to-front (farthest first) so
    // overlapping water/glass blend correctly even when they share a texture -
    // matching ref_gl1 R_SortAndDrawAlphaSurfaces' per-surface depth sort.
    // Translucent surfaces are few, so per-face sort + rebind is cheap.
    static int   face_order[4096];
    static float face_dist[4096];
    int nfaces = 0;
    if (s_pvs_faces && s_pvs_face_order) {
        for (int b = 0; b < s_num_batches && nfaces < 4096; b++) {
            if (s_batches[b].descriptor == VK_NULL_HANDLE) continue;
            if (s_batches[b].surf_flags == 0) continue;   // opaque
            const int fstart = s_batch_face_start[b];
            const int fcount = s_batch_face_count[b];
            for (int k = 0; k < fcount && nfaces < 4096; k++) {
                const int fi = s_pvs_face_order[fstart + k];
                const pvs_face_t* pf = &s_pvs_faces[fi];
                if (pf->num_vertices == 0) continue;
                const float dx = pf->center[0] - fd->vieworg[0];
                const float dy = pf->center[1] - fd->vieworg[1];
                const float dz = pf->center[2] - fd->vieworg[2];
                face_order[nfaces] = fi;
                face_dist[nfaces]  = dx*dx + dy*dy + dz*dz;
                nfaces++;
            }
        }
    }
    // Insertion sort by descending distance (farthest first).
    for (int a = 1; a < nfaces; a++) {
        const int   oi = face_order[a];
        const float od = face_dist[a];
        int b = a - 1;
        while (b >= 0 && face_dist[b] < od) {
            face_dist[b+1] = face_dist[b]; face_order[b+1] = face_order[b]; b--;
        }
        face_dist[b+1] = od; face_order[b+1] = oi;
    }

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_world.warp_pipeline);
    if (dlset != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vk_pipeline_world.warp_layout, 1, 1, &dlset, 0, NULL);

    // Interleave the deferred alpha entities (sprites/effects, already sorted
    // back-to-front by the client) with the back-to-front water faces, matching
    // ref_gl1 R_SortAndDrawAlphaSurfaces. We walk faces farthest-first; before
    // each face we flush any entities that are FARTHER than it (so they render
    // behind the water), then after all faces we draw the remaining nearer
    // entities on top. This is what puts the crosshair/splashes on top of the
    // opaque swamp water while still keeping submerged things behind clear water.
    // Entities use linear depth (e->depth); face_dist is squared distance, so
    // compare against ent_depth*ent_depth. Switching to draw an entity rebinds
    // its own pipeline, so we must re-bind the warp pipeline afterwards.
    int ei = 0;
    VkDescriptorSet bound_desc = VK_NULL_HANDLE;
    qboolean warp_bound = true;

    #define FLUSH_ENTS_FARTHER_THAN(SQDIST) \
        do { \
            while (draw_entity && ei < num_alpha_ents) { \
                struct entity_s* ae = alpha_ents[ei]; \
                if (!ae) { ei++; continue; } \
                const float ed = ae->depth; \
                if (ed * ed <= (SQDIST)) break;  /* nearer than this face -> later */ \
                draw_entity(ae, user); ei++; \
                warp_bound = false; bound_desc = VK_NULL_HANDLE; \
            } \
            if (!warp_bound) { \
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_world.warp_pipeline); \
                vkCmdBindVertexBuffers(cb, 0, 1, &s_vbo.buffer, &zero); \
                if (dlset != VK_NULL_HANDLE) \
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, \
                                            vk_pipeline_world.warp_layout, 1, 1, &dlset, 0, NULL); \
                warp_bound = true; \
            } \
        } while (0)

    for (int oi = 0; oi < nfaces; oi++) {
        const pvs_face_t* pf = &s_pvs_faces[face_order[oi]];
        const int b  = pf->batch;
        if (b < 0 || b >= s_num_batches) continue;
        const int sf = s_batches[b].surf_flags;

        // Draw any entities behind this face first.
        FLUSH_ENTS_FARTHER_THAN(face_dist[oi]);

        float alpha = 1.0f;
        if (sf & SURF_TRANS33) alpha = s_gl_trans33 ? s_gl_trans33->value : 0.33f;
        else if (sf & SURF_TRANS66) alpha = s_gl_trans66 ? s_gl_trans66->value : 0.66f;

        // Use this FACE's own flags for warp/flow/undulate (ref_gl1 evaluates
        // them per-face). The batch-level OR could otherwise make a still
        // turbulent pool flow just because a sibling inflow face is SURF_FLOWING.
        const int ff = pf->surf_flags ? pf->surf_flags : sf;
        warp_pc[16] = fd->time;
        warp_pc[17] = alpha;
        warp_pc[18] = (ff & SURF_FLOWING)  ? 1.0f : 0.0f;
        warp_pc[19] = (ff & SURF_UNDULATE) ? 1.0f : 0.0f;
        warp_pc[20] = (ff & SURF_WARP)     ? 1.0f : 0.0f;
        warp_pc[21] = warp_pc[22] = warp_pc[23] = 0.0f;
        vkCmdPushConstants(cb, vk_pipeline_world.warp_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(warp_pc), warp_pc);

        VkDescriptorSet wdesc = WorldBatchFrameDesc(&s_batches[b]);
        if (wdesc != bound_desc) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    vk_pipeline_world.warp_layout, 0, 1,
                                    &wdesc, 0, NULL);
            bound_desc = wdesc;
        }
        vkCmdDraw(cb, pf->num_vertices, 1, pf->first_vertex, 0);
    }

    // Remaining entities are nearer than all water -> draw on top.
    while (draw_entity && ei < num_alpha_ents) {
        if (alpha_ents[ei]) draw_entity(alpha_ents[ei], user);
        ei++;
    }
    #undef FLUSH_ENTS_FARTHER_THAN

    vk_state.pipeline_2d_bound = false;
}

int VK_World_NumSubmodels(void)
{
    return (s_num_submodels > 0) ? (s_num_submodels - 1) : 0;
}

qboolean VK_World_RenderSubmodel(int index, const float* mvp,
                                 const float origin[3], const float angles[3],
                                 int ent_frame)
{
    if (!s_loaded || index < 1 || index >= s_num_submodels) return false;
    if (!vk_state.frame_started || s_sub_total_verts == 0) return false;
    if (!s_sub_vbo.buffer) return false;

    const world_submodel_t* sm = &s_submodels[index];
    if (sm->num_batches <= 0) return true; // nothing to draw, but valid

    const uint32_t frame = vk_state.current_frame;
    VkCommandBuffer cb = vk_state.command_buffers[frame];

    // Build model matrix M = T(origin) * R(angles). Brush models use a
    // different sign convention than alias models: R_RotateForEntity is called
    // with pitch and roll pre-negated ("stupid quake bug"), so the net is
    //   Rz(+yaw) Ry(+pitch) Rx(+roll), angles in radians.
    const float yaw   =  angles ? angles[1] : 0.0f;
    const float pitch =  angles ? angles[0] : 0.0f;
    const float roll  =  angles ? angles[2] : 0.0f;
    const float cy=cosf(yaw),  sy=sinf(yaw);
    const float cp=cosf(pitch),sp=sinf(pitch);
    const float cr=cosf(roll), sr=sinf(roll);

    // R = Rz * Ry * Rx (3x3). Column-major 4x4 model matrix.
    const float a00= cy*cp, a01=-sy, a02= cy*sp;
    const float a10= sy*cp, a11= cy, a12= sy*sp;
    const float a20=-sp,    a21= 0,  a22= cp;
    float R[9] = {
        a00,            a10,            a20,
        a01*cr+a02*sr,  a11*cr+a12*sr,  a21*cr+a22*sr,
        a01*-sr+a02*cr, a11*-sr+a12*cr, a21*-sr+a22*cr
    };
    // model matrix (column-major)
    mat4_t M = {
        R[0], R[1], R[2], 0.0f,
        R[3], R[4], R[5], 0.0f,
        R[6], R[7], R[8], 0.0f,
        origin ? origin[0] : 0.0f, origin ? origin[1] : 0.0f, origin ? origin[2] : 0.0f, 1.0f
    };

    mat4_t m;
    // m = mvp * M
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += mvp[k*4 + r] * M[c*4 + k];
            m[c*4 + r] = s;
        }

    // Camera in submodel local space, for correct fog distance (verts are
    // model-space). R is orthonormal so inverse == transpose.
    float rel[3] = {
        s_view_origin_world[0] - (origin ? origin[0] : 0.0f),
        s_view_origin_world[1] - (origin ? origin[1] : 0.0f),
        s_view_origin_world[2] - (origin ? origin[2] : 0.0f)
    };
    float cam_local[3] = {
        R[0]*rel[0] + R[1]*rel[1] + R[2]*rel[2],
        R[3]*rel[0] + R[4]*rel[1] + R[5]*rel[2],
        R[6]*rel[0] + R[7]*rel[1] + R[8]*rel[2]
    };

    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &s_sub_vbo.buffer, &zero);

    if (!s_gl_trans33) s_gl_trans33 = ri.Cvar_Get("gl_trans33", "0.33", 0);
    if (!s_gl_trans66) s_gl_trans66 = ri.Cvar_Get("gl_trans66", "0.66", 0);

    VkDescriptorSet dlset = VK_World_CurrentDlightSet();

    // Two phases: opaque batches on the world pipeline, then translucent/warp
    // batches (e.g. forcefields, energy barriers, water brushes) on the warp
    // pipeline with alpha blending - matching how world surfaces are split.
    for (int phase = 0; phase < 2; phase++) {
        const qboolean warp_phase = (phase == 1);
        qboolean pipe_bound = false;

        for (int b = sm->first_batch; b < sm->first_batch + sm->num_batches; b++) {
            world_batch_t* wb = &s_sub_batches[b];
            const qboolean is_trans = (wb->surf_flags != 0);
            if (is_trans != warp_phase) continue;   // this phase only

            VkDescriptorSet desc = wb->descriptor;
            if (wb->num_frames > 1) {
                int fr;
                if (wb->anim_speed > 0) {
                    // SURF_ANIMSPEED: time-driven cycling (forcefields etc),
                    // matching ref_gl1 R_TextureAnimation's SURF_ANIMSPEED path
                    // (frame = num_frames * time), here num_frames is the fps.
                    fr = (int)(s_frame_time * (float)wb->anim_speed);
                } else {
                    fr = (ent_frame > 0) ? ent_frame : 0;
                }
                fr %= wb->num_frames;
                if (fr < 0) fr = 0;
                if (wb->frame_desc[fr] != VK_NULL_HANDLE) desc = wb->frame_desc[fr];
            }
            if (desc == VK_NULL_HANDLE) continue;

            if (!pipe_bound) {
                if (warp_phase) {
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      vk_pipeline_world.warp_pipeline);
                } else {
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      vk_pipeline_world.pipeline);
                    float spc[28];
                    memcpy(spc, m, sizeof(m));
                    FillFogParamsAt(spc, 16, cam_local);
                    spc[26] = 0.0f;   // dlights off (model space)
                    vkCmdPushConstants(cb, vk_pipeline_world.layout,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(spc), spc);
                }
                pipe_bound = true;
            }

            if (warp_phase) {
                const int sf = wb->surf_flags;
                float alpha = 1.0f;
                if (sf & SURF_TRANS33) alpha = s_gl_trans33 ? s_gl_trans33->value : 0.33f;
                else if (sf & SURF_TRANS66) alpha = s_gl_trans66 ? s_gl_trans66->value : 0.66f;

                float wpc[36];
                memcpy(wpc, m, sizeof(m));   // [0..15] local model->clip
                FillFogParamsAt(wpc, 24, cam_local);
                wpc[16] = s_frame_time; // time (warp/flow anim, matches world warp pass)
                wpc[17] = alpha;
                wpc[18] = (sf & SURF_FLOWING)  ? 1.0f : 0.0f;
                wpc[19] = (sf & SURF_UNDULATE) ? 1.0f : 0.0f;
                wpc[20] = (sf & SURF_WARP)     ? 1.0f : 0.0f;
                wpc[21] = wpc[22] = wpc[23] = 0.0f;
                vkCmdPushConstants(cb, vk_pipeline_world.warp_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(wpc), wpc);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        vk_pipeline_world.warp_layout, 0, 1, &desc, 0, NULL);
                if (dlset != VK_NULL_HANDLE)
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            vk_pipeline_world.warp_layout, 1, 1, &dlset, 0, NULL);
            } else {
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        vk_pipeline_world.layout, 0, 1, &desc, 0, NULL);
                if (dlset != VK_NULL_HANDLE)
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            vk_pipeline_world.layout, 1, 1, &dlset, 0, NULL);
            }
            vkCmdDraw(cb, wb->num_vertices, 1, wb->first_vertex, 0);
        }
    }

    vk_state.pipeline_2d_bound = false;
    return true;
}
