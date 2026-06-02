//
// vk_sprite.c - .sp2 sprites: billboarded textured quads.
//
// Format (qcommon/qfiles.h):
//   dsprite_t: ident('IDS2') + version(2) + numframes + dsprframe_t[]
//   dsprframe_t: width, height, origin_x, origin_y, name[64]
//
// One sprite has multiple frames (animation). Each frame names a texture file.
// At render time the engine picks e->frame and we draw a quad of that frame's
// dimensions, billboarded so up/right align with the camera, centered with a
// per-frame pixel offset (origin_x/origin_y).
//

#include "vk_sprite.h"
#include "vk_local.h"
#include "vk_buffer.h"
#include "vk_image.h"
#include "vk_pipeline3d.h"
#include "vk_pipeline_world.h"
#include "vk_world.h"

#include "qcommon/qfiles.h"
#include "client/ref.h"
#include "qcommon/Vector.h"
#include "qcommon/q_Sprite.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define VK_MAX_SPRITES         256
#define VK_MAX_SPRITE_FRAMES   64

typedef struct {
    int             w, h;
    int             origin_x, origin_y;
    image_t*        image;
    VkDescriptorSet descriptor;
} vk_sprite_frame_t;

struct vk_sprite_s {
    char              name[256];
    qboolean          used;
    int               num_frames;
    vk_sprite_frame_t frames[VK_MAX_SPRITE_FRAMES];
};

static struct vk_sprite_s s_sprites[VK_MAX_SPRITES];
static int                s_num_sprites = 0;

// Per-frame dynamic vertex buffer (4 verts per sprite -> reuse the model VBO
// pattern but with our own small ring). Keep it simple: a single VBO of
// vk_sprite_vert_t * 4096 verts, refilled each frame.
typedef struct { float x, y, z; float u, v; float nx, ny, nz; } vk_sprite_vert_t;

#define VK_SPRITE_MAX_VERTS  (4 * 1024)     // 1024 sprites/frame is plenty

static vk_buffer_t       s_sprite_vbo[MAX_FRAMES_IN_FLIGHT];
static vk_sprite_vert_t* s_sprite_mapped[MAX_FRAMES_IN_FLIGHT];
static uint32_t          s_sprite_cursor[MAX_FRAMES_IN_FLIGHT];
static qboolean          s_sprite_vbo_ready = false;

static qboolean EnsureSpriteVBO(void)
{
    if (s_sprite_vbo_ready) return true;
    const VkDeviceSize sz = sizeof(vk_sprite_vert_t) * VK_SPRITE_MAX_VERTS;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (!VK_CreateBuffer(sz, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &s_sprite_vbo[i]))
            return false;
        if (!VK_MapBuffer(&s_sprite_vbo[i])) return false;
        s_sprite_mapped[i] = (vk_sprite_vert_t*)s_sprite_vbo[i].mapped;
        s_sprite_cursor[i] = 0;
    }
    s_sprite_vbo_ready = true;
    return true;
}

// Reset per-frame cursor at the start of each frame.
void VK_Sprite_BeginFrame(void)
{
    if (!s_sprite_vbo_ready) return;
    s_sprite_cursor[vk_state.current_frame] = 0;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

static struct vk_sprite_s* SpriteFindCached(const char* name)
{
    for (int i = 0; i < s_num_sprites; i++)
        if (s_sprites[i].used && strcasecmp(s_sprites[i].name, name) == 0)
            return &s_sprites[i];
    return NULL;
}

static struct vk_sprite_s* SpriteAllocSlot(void)
{
    for (int i = 0; i < VK_MAX_SPRITES; i++) {
        if (!s_sprites[i].used) {
            if (i >= s_num_sprites) s_num_sprites = i + 1;
            memset(&s_sprites[i], 0, sizeof(s_sprites[i]));
            s_sprites[i].used = true;
            return &s_sprites[i];
        }
    }
    return NULL;
}

vk_sprite_t* VK_Sprite_TryLoad(const char* name)
{
    if (!name || !*name) return NULL;
    struct vk_sprite_s* hit = SpriteFindCached(name);
    if (hit) return hit;

    byte* buffer = NULL;
    int length = ri.FS_LoadFile(name, (void**)&buffer);
    if (!buffer || length < (int)sizeof(dsprite_t)) {
        if (buffer) ri.FS_FreeFile(buffer);
        return NULL;
    }

    const dsprite_t* sp = (const dsprite_t*)buffer;
    if (sp->ident != IDSPRITEHEADER || sp->version != SPRITE_VERSION) {
        ri.FS_FreeFile(buffer);
        return NULL;
    }

    struct vk_sprite_s* s = SpriteAllocSlot();
    if (!s) { ri.FS_FreeFile(buffer); return NULL; }
    strcpy_s(s->name, sizeof(s->name), name);

    int nf = sp->numframes;
    if (nf > VK_MAX_SPRITE_FRAMES) nf = VK_MAX_SPRITE_FRAMES;
    s->num_frames = nf;

    for (int i = 0; i < nf; i++) {
        const dsprframe_t* f = &sp->frames[i];
        s->frames[i].w        = f->width;
        s->frames[i].h        = f->height;
        s->frames[i].origin_x = f->origin_x;
        s->frames[i].origin_y = f->origin_y;

        // Frame texture names are stored relative to the Sprites/ directory
        // (e.g. "fx/halo_0.m8" means "Sprites/fx/halo_0.m8"). Matches GL1's
        // Mod_LoadSpriteModel which prepends "Sprites/".
        char frame_path[256];
        snprintf(frame_path, sizeof(frame_path), "Sprites/%s", f->name);

        s->frames[i].image    = VK_FindImage(frame_path);
        if (s->frames[i].image)
            s->frames[i].descriptor = VK_AllocWorldDescriptor(VK_ImageView(s->frames[i].image));
        else
            ri.Con_Printf(PRINT_ALL, "vk:   sprite '%s' frame %d: image '%s' NOT FOUND\n",
                          name, i, frame_path);
    }

    ri.FS_FreeFile(buffer);
    ri.Con_Printf(PRINT_ALL, "vk: sprite '%s': %d frames\n", name, nf);
    return s;
}

void VK_Sprite_FreeAll(void)
{
    memset(s_sprites, 0, sizeof(s_sprites));
    s_num_sprites = 0;

    if (s_sprite_vbo_ready) {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
            VK_DestroyBuffer(&s_sprite_vbo[i]);
        s_sprite_vbo_ready = false;
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void VK_Sprite_DrawEntity(const struct entity_s* e, const vk_sprite_t* s,
                          const float vup[3], const float vright[3],
                          const float vfwd[3], const float* mvp)
{
    if (!e || !s || !vk_state.frame_started) return;
    if (!EnsureSpriteVBO()) return;

    int fi = e->frame;
    if (fi < 0 || fi >= s->num_frames) fi = 0;
    if (s->num_frames > 0) fi %= s->num_frames;

    const vk_sprite_frame_t* f = &s->frames[fi];
    if (!f->image || f->descriptor == VK_NULL_HANDLE) return;

    const uint32_t frame = vk_state.current_frame;
    if (s_sprite_cursor[frame] + 6 > VK_SPRITE_MAX_VERTS) return;

    const float scale = (e->scale > 0.001f) ? e->scale : 1.0f;

    // Orientation. Camera-facing billboard by default; RF_FIXED sprites
    // (scorch marks, decals) lie flat on a surface using the entity's angles,
    // matching GL1's R_DrawSpriteModel.
    float up[3], right[3];
    if (e->flags & RF_FIXED) {
        vec3_t dir, fup;
        DirAndUpFromAngles((float*)e->angles, dir, fup);
        right[0] = fup[1]*dir[2] - fup[2]*dir[1];
        right[1] = fup[2]*dir[0] - fup[0]*dir[2];
        right[2] = fup[0]*dir[1] - fup[1]*dir[0];
        float rl = sqrtf(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]);
        if (rl > 1e-6f) { right[0]/=rl; right[1]/=rl; right[2]/=rl; }
        up[0]=fup[0]; up[1]=fup[1]; up[2]=fup[2];
    } else {
        up[0]=vup[0]; up[1]=vup[1]; up[2]=vup[2];
        right[0]=vright[0]; right[1]=vright[1]; right[2]=vright[2];
    }

    // Build the 4 quad corners (with per-corner UVs) based on the sprite type.
    // c[0..3] are TL,BL,BR,TR equivalents; uvq[0..3] their texture coords.
    float c[4][3];
    float uvq[4][2];

    switch (e->spriteType) {
        case SPRITE_DYNAMIC: {
            // 4 variable verts: each has its own (x,y) offset and (s,t).
            for (int i = 0; i < 4; i++) {
                for (int k = 0; k < 3; k++)
                    c[i][k] = e->origin[k] + (scale*e->verts[i].y)*up[k]
                                           + (scale*e->verts[i].x)*right[k];
                uvq[i][0] = e->verts[i].s;
                uvq[i][1] = e->verts[i].t;
            }
        } break;

        case SPRITE_LINE: {
            // Beam between startpos and endpos with a width (scale/scale2).
            // The width axis is perpendicular to the beam AND the view direction
            // so the beam keeps full width as the camera orbits it - ref_gl1
            // passes vpn (view forward) here, NOT the camera up. Using up made
            // the beam collapse to zero width at flat viewing angles (vines
            // vanishing when looked at edge-on along the camera-up axis).
            float diff[3] = { e->endpos[0]-e->startpos[0],
                              e->endpos[1]-e->startpos[1],
                              e->endpos[2]-e->startpos[2] };
            const float* axis = vfwd ? vfwd : up;   // view forward (vpn)
            // dir = diff x axis
            float dir[3] = {
                diff[1]*axis[2] - diff[2]*axis[1],
                diff[2]*axis[0] - diff[0]*axis[2],
                diff[0]*axis[1] - diff[1]*axis[0]
            };
            float dl = sqrtf(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
            if (dl > 1e-6f) { dir[0]/=dl; dir[1]/=dl; dir[2]/=dl; }
            const float so = scale * 0.5f;
            const float eo = ((e->scale2 > 0.001f) ? e->scale2 : scale) * 0.5f;
            const float tile = (e->tile > 0.0f) ? e->tile : 1.0f;
            const float t0 = e->tileoffset;
            const float t1 = e->tileoffset + tile;
            for (int k = 0; k < 3; k++) {
                c[0][k] = e->startpos[k] - so*dir[k];
                c[1][k] = e->startpos[k] + so*dir[k];
                c[2][k] = e->endpos[k]   + eo*dir[k];
                c[3][k] = e->endpos[k]   - eo*dir[k];
            }
            uvq[0][0]=0.0f; uvq[0][1]=t0;
            uvq[1][0]=1.0f; uvq[1][1]=t0;
            uvq[2][0]=1.0f; uvq[2][1]=t1;
            uvq[3][0]=0.0f; uvq[3][1]=t1;
        } break;

        case SPRITE_STANDARD:
        case SPRITE_EDICT:
        default: {
            const float xl =  (float)(-f->origin_x)        * scale;
            const float xr =  (float)( f->w - f->origin_x) * scale;
            const float yt =  (float)(-f->origin_y)        * scale;
            const float yb =  (float)( f->h - f->origin_y) * scale;
            for (int k = 0; k < 3; k++) {
                c[0][k] = e->origin[k] + yt*up[k] + xl*right[k]; // TL
                c[1][k] = e->origin[k] + yb*up[k] + xl*right[k]; // BL
                c[2][k] = e->origin[k] + yb*up[k] + xr*right[k]; // BR
                c[3][k] = e->origin[k] + yt*up[k] + xr*right[k]; // TR
            }
            uvq[0][0]=0.0f; uvq[0][1]=1.0f;
            uvq[1][0]=0.0f; uvq[1][1]=0.0f;
            uvq[2][0]=1.0f; uvq[2][1]=0.0f;
            uvq[3][0]=1.0f; uvq[3][1]=1.0f;
        } break;
    }

    vk_sprite_vert_t* vb = s_sprite_mapped[frame];
    uint32_t cursor = s_sprite_cursor[frame];
    const uint32_t start = cursor;

    // Two triangles from the 4 corners: 0,1,2 and 0,2,3.
    #define EMITC(I) do { \
        vb[cursor].x = c[I][0]; vb[cursor].y = c[I][1]; vb[cursor].z = c[I][2]; \
        vb[cursor].u = uvq[I][0]; vb[cursor].v = uvq[I][1]; \
        vb[cursor].nx = 0.0f; vb[cursor].ny = 0.0f; vb[cursor].nz = 1.0f; cursor++; \
    } while (0)

    EMITC(0); EMITC(1); EMITC(2);
    EMITC(0); EMITC(2); EMITC(3);
    #undef EMITC

    s_sprite_cursor[frame] = cursor;

    VkCommandBuffer cb = vk_state.command_buffers[frame];
    const qboolean nodepth  = (e->flags & RF_NODEPTHTEST) != 0;
    const qboolean additive = (e->flags & (RF_TRANS_ADD | RF_TRANS_ADD_ALPHA)) != 0;
    VkPipeline sp_pipe;
    if (additive)
        sp_pipe = nodepth ? vk_pipeline_3d.pipeline_additive_nodepth
                          : vk_pipeline_3d.pipeline_additive;
    else if (!nodepth && e->spriteType == SPRITE_LINE)
        // SPRITE_LINE segments (swing-vines, ropes, chains, tendrils) are
        // structural billboards that hang in the world and must occlude the
        // later depth-write-off water/effect passes, so they write depth (the
        // v91 alpha-test discard keeps their transparent texels from writing).
        // Soft point effects (splashes, ripples, steam) stay depth-write OFF so
        // they blend over the water instead of punching holes in it.
        sp_pipe = vk_pipeline_3d.pipeline_translucent_zwrite;
    else
        sp_pipe = nodepth ? vk_pipeline_3d.pipeline_translucent_nodepth
                          : vk_pipeline_3d.pipeline_translucent;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sp_pipe);
    {
        float pc[32];
        memcpy(pc, mvp, 64);
        pc[16] = (float)e->color.r / 255.0f;
        pc[17] = (float)e->color.g / 255.0f;
        pc[18] = (float)e->color.b / 255.0f;
        pc[19] = (float)e->color.a / 255.0f;
        if (pc[16]==0 && pc[17]==0 && pc[18]==0 && pc[19]==0) {
            pc[16]=pc[17]=pc[18]=pc[19]=1.0f;
        }
        // Additive sprites (ONE,ONE blend) ignore the alpha channel in the
        // blend, so a fading alpha (e.g. a lens flare being occlusion-faded by
        // the client, color.a dropping each frame) would NOT dim the glow - it
        // would stay full-bright and then blink out when the effect stops being
        // submitted. ref_gl1 scales the additive contribution by alpha; do the
        // same by folding alpha into the RGB tint and setting tint alpha to 1.
        if (additive) {
            pc[16] *= pc[19]; pc[17] *= pc[19]; pc[18] *= pc[19];
            pc[19] = 1.0f;
        }
        // Sprites are world-positioned effect billboards; ref_gl1 keeps fog
        // enabled while drawing them, so distant decals/effects (e.g. scorch
        // marks) fade into the fog instead of standing out as stark marks on
        // top of the haze. Use the real fog params (camera-relative distance).
        // Additive sprites must NOT be fogged toward the (often white) fog
        // colour - that would brighten rather than fade them - so only apply
        // fog to alpha-blended sprites.
        if (additive) {
            pc[20] = pc[21] = pc[22] = 0.0f; pc[23] = 0.0f;
            pc[24] = pc[25] = pc[26] = 0.0f; pc[27] = -1.0f;  // fog off
            pc[28] = pc[29] = pc[30] = pc[31] = 0.0f;
        } else {
            VK_World_FillEntityFog(pc);   // pc[20..31] real fog tail
            pc[30] = 0.0f;                // no dynamic lights on sprites
            pc[31] = 0.0f;                // not sky fog
        }
        vkCmdPushConstants(cb, vk_pipeline_3d.layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), pc);
    }
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk_pipeline_3d.layout, 0, 1, &f->descriptor, 0, NULL);
    {
        VkDescriptorSet dlset = VK_World_CurrentDlightSet();
        if (dlset != VK_NULL_HANDLE)
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    vk_pipeline_3d.layout, 1, 1, &dlset, 0, NULL);
    }
    VkDeviceSize offset = (VkDeviceSize)start * sizeof(vk_sprite_vert_t);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_sprite_vbo[frame].buffer, &offset);
    vkCmdDraw(cb, 6, 1, 0, 0);

    vk_state.pipeline_2d_bound = false;
}
