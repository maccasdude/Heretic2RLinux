// vk_lightpoint.c
//
// BSP point-lighting sample, ported from ref_gl1's R_RecursiveLightPoint /
// R_LightPoint (gl1_Light.c). This is needed for more than just visuals: the
// engine stores the light level at the player's position into the movement
// command (r_lightlevel cvar -> cmd.lightlevel -> client->light_level), and
// the monster AI uses client->light_level to decide whether it can see the
// player (g_AI.c FindTarget: if light_level <= 5 the player is invisible to
// monsters, and below SKILL_HARD there's a randomized threshold up to 77).
//
// Without this, r_lightlevel stays 0 and monsters never notice the player
// under the Vulkan renderer. We keep a compact copy of the BSP nodes, planes,
// leafs, faces, texinfo and lighting parsed straight from the map buffer so we
// can walk the tree and read the lightmap sample at a world point.

#include "vk_local.h"
#include "qcommon/qfiles.h"
#include "client/ref.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

extern refimport_t ri;

// Compact parsed structures (only the fields the light point walk needs).
typedef struct {
    float normal[3];
    float dist;
} lp_plane_t;

typedef struct {
    int   planenum;
    int   children[2];   // <0 => -(leaf+1)
    int   firstface;
    int   numfaces;
} lp_node_t;

typedef struct {
    short  texturemins[2];
    short  extents[2];
    int    lightofs;          // byte offset into lighting data, -1 = none
    int    texinfo;
    int    flags;             // texinfo flags (to skip warp/sky/nodraw)
    float  vecs[2][4];        // texinfo s/t vectors (copied for convenience)
    byte   styles0_valid;     // whether style 0 lightmap present
} lp_face_t;

static lp_plane_t* s_planes   = NULL;  static int s_num_planes = 0;
static lp_node_t*  s_nodes    = NULL;  static int s_num_nodes  = 0;
static lp_face_t*  s_faces    = NULL;  static int s_num_faces  = 0;
static byte*       s_lighting = NULL;  static int s_lighting_len = 0;
static int         s_num_leafs = 0;
static qboolean    s_ready    = false;

// Scratch outputs from the recursive walk.
static float s_pointcolor[3];
static float s_lightspot[3];

static const void* GetLump(const byte* buf, const lump_t* l, int* count, int stride)
{
    if (l->filelen <= 0) { if (count) *count = 0; return NULL; }
    if (count) *count = l->filelen / stride;
    return buf + l->fileofs;
}

void VK_LightPoint_Free(void)
{
    free(s_planes);  s_planes = NULL;  s_num_planes = 0;
    free(s_nodes);   s_nodes  = NULL;  s_num_nodes  = 0;
    free(s_faces);   s_faces  = NULL;  s_num_faces  = 0;
    free(s_lighting); s_lighting = NULL; s_lighting_len = 0;
    s_num_leafs = 0;
    s_ready = false;
}

// Parse the lumps we need straight from the BSP buffer. 'buf' must remain the
// raw IBSP image; we copy what we keep so the caller can free it afterwards.
void VK_LightPoint_Load(const byte* buf)
{
    VK_LightPoint_Free();
    if (!buf) return;

    const dheader_t* hdr = (const dheader_t*)buf;

    int np = 0, nn = 0, nf = 0, nti = 0, nleaf = 0;
    const dplane_t*  planes  = GetLump(buf, &hdr->lumps[LUMP_PLANES],  &np,   sizeof(dplane_t));
    const dnode_t*   nodes   = GetLump(buf, &hdr->lumps[LUMP_NODES],   &nn,   sizeof(dnode_t));
    const dface_t*   faces   = GetLump(buf, &hdr->lumps[LUMP_FACES],   &nf,   sizeof(dface_t));
    const texinfo_t* texinfo = GetLump(buf, &hdr->lumps[LUMP_TEXINFO], &nti,  sizeof(texinfo_t));
    GetLump(buf, &hdr->lumps[LUMP_LEAFS], &nleaf, sizeof(dleaf_t));

    const dvertex_t* verts    = GetLump(buf, &hdr->lumps[LUMP_VERTEXES], NULL, sizeof(dvertex_t));
    int nverts = hdr->lumps[LUMP_VERTEXES].filelen / (int)sizeof(dvertex_t);
    const dedge_t*   edges    = GetLump(buf, &hdr->lumps[LUMP_EDGES],    NULL, sizeof(dedge_t));
    int nedges = hdr->lumps[LUMP_EDGES].filelen / (int)sizeof(dedge_t);
    const int*       surfedge = GetLump(buf, &hdr->lumps[LUMP_SURFEDGES], NULL, sizeof(int));

    const byte* lighting = (const byte*)(buf + hdr->lumps[LUMP_LIGHTING].fileofs);
    int lighting_len = hdr->lumps[LUMP_LIGHTING].filelen;

    if (!planes || !nodes || !faces || !texinfo || np <= 0 || nn <= 0 || nf <= 0) {
        ri.Con_Printf(PRINT_ALL, "vk: lightpoint - missing BSP lumps, AI light disabled\n");
        return;
    }

    s_planes = (lp_plane_t*)calloc(np, sizeof(lp_plane_t));
    s_nodes  = (lp_node_t*) calloc(nn, sizeof(lp_node_t));
    s_faces  = (lp_face_t*) calloc(nf, sizeof(lp_face_t));
    if (lighting_len > 0) {
        s_lighting = (byte*)malloc(lighting_len);
        if (s_lighting) { memcpy(s_lighting, lighting, lighting_len); s_lighting_len = lighting_len; }
    }
    if (!s_planes || !s_nodes || !s_faces) { VK_LightPoint_Free(); return; }

    for (int i = 0; i < np; i++) {
        s_planes[i].normal[0] = planes[i].normal[0];
        s_planes[i].normal[1] = planes[i].normal[1];
        s_planes[i].normal[2] = planes[i].normal[2];
        s_planes[i].dist      = planes[i].dist;
    }
    s_num_planes = np;

    for (int i = 0; i < nn; i++) {
        s_nodes[i].planenum    = nodes[i].planenum;
        s_nodes[i].children[0] = nodes[i].children[0];
        s_nodes[i].children[1] = nodes[i].children[1];
        s_nodes[i].firstface   = nodes[i].firstface;
        s_nodes[i].numfaces    = nodes[i].numfaces;
    }
    s_num_nodes = nn;
    s_num_leafs = nleaf;

    // Faces: compute texturemins/extents (same math as the world loader) and
    // copy the texinfo vectors + flags so the walk is self-contained.
    for (int i = 0; i < nf; i++) {
        const dface_t* f = &faces[i];
        lp_face_t* o = &s_faces[i];
        o->texinfo  = f->texinfo;
        o->lightofs = f->lightofs;
        o->styles0_valid = (f->styles[0] == 0); // style 0 is the base lightmap

        const texinfo_t* ti = (f->texinfo >= 0 && f->texinfo < nti) ? &texinfo[f->texinfo] : NULL;
        if (!ti) { o->lightofs = -1; continue; }
        o->flags = ti->flags;
        memcpy(o->vecs, ti->vecs, sizeof(o->vecs));

        // CalcExtents
        float mins[2] = { 999999.0f, 999999.0f };
        float maxs[2] = { -99999.0f, -99999.0f };
        for (int e = 0; e < f->numedges; e++) {
            const int se = surfedge ? surfedge[f->firstedge + e] : 0;
            int vi;
            if (se >= 0) { if (se >= nedges) continue; vi = edges[se].v[0]; }
            else         { int idx = -se; if (idx >= nedges) continue; vi = edges[idx].v[1]; }
            if (vi < 0 || vi >= nverts) continue;
            const float* p = verts[vi].point;
            for (int j = 0; j < 2; j++) {
                const float val = p[0]*ti->vecs[j][0] + p[1]*ti->vecs[j][1] +
                                  p[2]*ti->vecs[j][2] + ti->vecs[j][3];
                if (val < mins[j]) mins[j] = val;
                if (val > maxs[j]) maxs[j] = val;
            }
        }
        for (int j = 0; j < 2; j++) {
            const int bmin = (int)floorf(mins[j] / 16.0f);
            const int bmax = (int)ceilf (maxs[j] / 16.0f);
            o->texturemins[j] = (short)(bmin * 16);
            o->extents[j]     = (short)((bmax - bmin) * 16);
        }
    }
    s_num_faces = nf;
    s_ready = true;

    ri.Con_Printf(PRINT_ALL, "vk: lightpoint ready (%d nodes, %d faces, %d lightbytes)\n",
                  s_num_nodes, s_num_faces, s_lighting_len);
}

// Bilinear lightmap sample at (ds,dt) within a face's style-0 lightmap.
static void SamplePointColor(const lp_face_t* f, int ds, int dt, float color[3])
{
    color[0] = color[1] = color[2] = 0.0f;
    if (f->lightofs < 0 || !s_lighting) return;

    const int smax = (f->extents[0] >> 4) + 1;
    const int tmax = (f->extents[1] >> 4) + 1;

    // Nearest-texel sample (clamped). The engine does a bilinear blend; nearest
    // is sufficient for the light-level value the AI consumes and avoids edge
    // reads. ds,dt are in [0..extents], so >>4 gives the luxel index.
    int ls = ds >> 4; if (ls < 0) ls = 0; if (ls >= smax) ls = smax - 1;
    int lt = dt >> 4; if (lt < 0) lt = 0; if (lt >= tmax) lt = tmax - 1;

    const long ofs = (long)f->lightofs + ((long)lt * smax + ls) * 3;
    if (ofs < 0 || ofs + 2 >= s_lighting_len) return;

    color[0] = (float)s_lighting[ofs + 0] / 255.0f;
    color[1] = (float)s_lighting[ofs + 1] / 255.0f;
    color[2] = (float)s_lighting[ofs + 2] / 255.0f;
}

static int RecursiveLightPoint(int nodenum, const float start[3], const float end[3])
{
    if (nodenum < 0)
        return -1;  // hit a leaf, no surface found on the way
    if (nodenum >= s_num_nodes)
        return -1;

    const lp_node_t* node = &s_nodes[nodenum];
    if (node->planenum < 0 || node->planenum >= s_num_planes)
        return -1;
    const lp_plane_t* plane = &s_planes[node->planenum];

    const float front = start[0]*plane->normal[0] + start[1]*plane->normal[1] +
                        start[2]*plane->normal[2] - plane->dist;
    const float back  = end[0]*plane->normal[0] + end[1]*plane->normal[1] +
                        end[2]*plane->normal[2] - plane->dist;
    const int side = (front < 0.0f);

    if ((back < 0.0f) == (qboolean)side) {
        int child = node->children[side];
        return RecursiveLightPoint(child, start, end);
    }

    const float frac = front / (front - back);
    float mid[3] = {
        start[0] + (end[0]-start[0]) * frac,
        start[1] + (end[1]-start[1]) * frac,
        start[2] + (end[2]-start[2]) * frac
    };

    // Front side first.
    int r = RecursiveLightPoint(node->children[side], start, mid);
    if (r >= 0) return r;

    if ((back < 0.0f) == (qboolean)side)
        return -1;

    s_lightspot[0] = mid[0]; s_lightspot[1] = mid[1]; s_lightspot[2] = mid[2];

    // Check this node's faces for an impact.
    for (int i = 0; i < node->numfaces; i++) {
        const int fi = node->firstface + i;
        if (fi < 0 || fi >= s_num_faces) continue;
        const lp_face_t* f = &s_faces[fi];

        if (f->lightofs < 0 || !f->styles0_valid)
            continue;
        // Skip warp/sky/nodraw - they have no usable base lightmap.
        if (f->flags & (SURF_WARP | SURF_SKY | SURF_NODRAW))
            continue;

        const int s = (int)(mid[0]*f->vecs[0][0] + mid[1]*f->vecs[0][1] +
                            mid[2]*f->vecs[0][2] + f->vecs[0][3]);
        const int t = (int)(mid[0]*f->vecs[1][0] + mid[1]*f->vecs[1][1] +
                            mid[2]*f->vecs[1][2] + f->vecs[1][3]);

        if (s < f->texturemins[0] || t < f->texturemins[1])
            continue;
        const int ds = s - f->texturemins[0];
        const int dt = t - f->texturemins[1];
        if (ds > f->extents[0] || dt > f->extents[1])
            continue;

        SamplePointColor(f, ds, dt, s_pointcolor);
        return 1;
    }

    // Back side.
    return RecursiveLightPoint(node->children[!side], mid, end);
}

// Public: sample the world lighting at point p. Returns the dominant component
// scaled like GL1 (so the caller can feed r_lightlevel the same way).
float VK_LightPoint_Sample(const float p[3])
{
    if (!s_ready || s_num_nodes <= 0)
        return 0.0f;

    const float end[3] = { p[0], p[1], p[2] - 3072.0f };
    s_pointcolor[0] = s_pointcolor[1] = s_pointcolor[2] = 0.0f;

    const int r = RecursiveLightPoint(0, p, end);
    float color[3];
    if (r == -1) {
        color[0] = color[1] = color[2] = 0.25f;  // matches GL1 fallback
    } else {
        color[0] = s_pointcolor[0];
        color[1] = s_pointcolor[1];
        color[2] = s_pointcolor[2];
    }

    float m = color[0];
    if (color[1] > m) m = color[1];
    if (color[2] > m) m = color[2];
    return m;
}

// Full-RGB world lighting sample at point p, matching ref_gl1 R_LightPoint
// (before entity-color modulation / minlight). Used for RF_TRANS_GHOST alpha,
// which GL1 derives from shadelight[0]. Returns false if lighting data isn't
// loaded (caller should fall back).
qboolean VK_LightPoint_SampleRGB(const float p[3], float out[3])
{
    if (!s_ready || s_num_nodes <= 0)
        return false;

    const float end[3] = { p[0], p[1], p[2] - 3072.0f };
    s_pointcolor[0] = s_pointcolor[1] = s_pointcolor[2] = 0.0f;

    const int r = RecursiveLightPoint(0, p, end);
    if (r == -1) {
        out[0] = out[1] = out[2] = 0.25f;  // matches GL1 fallback
    } else {
        out[0] = s_pointcolor[0];
        out[1] = s_pointcolor[1];
        out[2] = s_pointcolor[2];
    }
    return true;
}
