//
// vk_sky.c - skybox: 6 textured quads centered on the camera.
//
// Q2/H2 skybox: 6 textures (rt/bk/lf/ft/up/dn) loaded from pics/skies/<name><suf>.m8.
// Draw order: each face is a quad of side 2*clipdist on the cube at the
// camera position. The MakeSkyVec axis remapping copied verbatim from GL1.
//

#include "vk_sky.h"
#include "vk_local.h"
#include "vk_buffer.h"
#include "vk_image.h"
#include "vk_pipeline3d.h"
#include "vk_pipeline_world.h"
#include "vk_world.h"

#include "client/ref.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

static char    s_sky_name[64] = {0};
static float   s_sky_rotate   = 0.0f;
static float   s_sky_axis[3]  = {0,0,1};
static image_t* s_sky_images[6] = {0};

#define SKY_CLIPDIST 2300.0f

// GL1's surface order: { 0, 2, 1, 3, 4, 5 } applied to suf[] order
// {rt,bk,lf,ft,up,dn}. So drawing face i samples sky_images[skytexorder[i]],
// where suf[] is loaded into images by name. We just store images in the
// final draw-axis order (axis 0=rt, 1=lf, 2=bk, 3=ft, 4=up, 5=dn).

// Cube face axis remap from (s,t,clipdist) -> world vec.
// (matches GL1 gl1_Sky.c R_MakeSkyVec)
static const int k_st_to_vec[6][3] = {
    {  3, -1,  2 },   // axis 0 = right face
    { -3,  1,  2 },   // axis 1 = left face
    {  1,  3,  2 },   // axis 2 = back face
    { -1, -3,  2 },   // axis 3 = front face
    { -2, -1,  3 },   // axis 4 = top
    {  2, -1, -3 }    // axis 5 = bottom
};

static void MakeSkyVec(float s, float t, int axis, float out_pos[3], float out_uv[2])
{
    float b[3] = { s * SKY_CLIPDIST, t * SKY_CLIPDIST, SKY_CLIPDIST };
    for (int i = 0; i < 3; i++) {
        int k = k_st_to_vec[axis][i];
        if (k < 0) out_pos[i] = -b[-k - 1];
        else       out_pos[i] =  b[ k - 1];
    }
    // UVs: s,t are -1..1; remap to 0..1, flip t.
    float su = (s + 1.0f) * 0.5f;
    float tv = (t + 1.0f) * 0.5f;
    // Tiny seam-avoidance clamp.
    const float lo = 1.0f / 512.0f, hi = 511.0f / 512.0f;
    if (su < lo) su = lo; else if (su > hi) su = hi;
    if (tv < lo) tv = lo; else if (tv > hi) tv = hi;
    out_uv[0] = su;
    out_uv[1] = 1.0f - tv;
}

// suffix order used by GL1 for loading sky images.
// Index suf[i] is loaded into image slot i. At draw time, face F uses
// image at skytexorder[F].
static const char* k_suf[6] = { "rt", "bk", "lf", "ft", "up", "dn" };
static const int   k_tex_order[6] = { 0, 2, 1, 3, 4, 5 };

void VK_Sky_Set(const char* name, float rotate, const float axis[3])
{
    if (!name) name = "";
    if (strcasecmp(s_sky_name, name) == 0) {
        s_sky_rotate = rotate;
        if (axis) { s_sky_axis[0]=axis[0]; s_sky_axis[1]=axis[1]; s_sky_axis[2]=axis[2]; }
        // Same sky as the previous map: we skip reloading, but we MUST re-touch
        // the sky images so they are stamped with the current registration
        // sequence. Otherwise R_EndRegistration's VK_FreeUnusedImages sees them
        // as stale, destroys their image views, and the still-bound sky
        // descriptors then reference a destroyed view -> GPU device-lost.
        for (int i = 0; i < 6; i++)
            if (s_sky_images[i]) VK_Image_Touch(s_sky_images[i]);
        return;
    }
    strncpy(s_sky_name, name, sizeof(s_sky_name) - 1);
    s_sky_name[sizeof(s_sky_name) - 1] = 0;
    s_sky_rotate = rotate;
    if (axis) { s_sky_axis[0]=axis[0]; s_sky_axis[1]=axis[1]; s_sky_axis[2]=axis[2]; }

    for (int i = 0; i < 6; i++) {
        char path[128];
        snprintf(path, sizeof(path), "pics/skies/%s%s.m8", name, k_suf[i]);
        s_sky_images[i] = VK_FindImage(path);
        if (!s_sky_images[i]) {
            snprintf(path, sizeof(path), "pics/skies/%s%s.m32", name, k_suf[i]);
            s_sky_images[i] = VK_FindImage(path);
        }
        if (!s_sky_images[i])
            ri.Con_Printf(PRINT_ALL, "vk: sky '%s%s' missing\n", name, k_suf[i]);
        // Descriptor is fetched from the image at draw time (VK_ImageWorldDescriptor).
    }
    ri.Con_Printf(PRINT_DEVELOPER, "vk: sky set to '%s'\n", name);
}

// Per-frame VBO holding 6 faces * 6 verts = 36 verts.
typedef struct { float x,y,z; float u,v; float nx,ny,nz; } sky_vert_t;
static vk_buffer_t s_sky_vbo[MAX_FRAMES_IN_FLIGHT];
static qboolean    s_vbo_ready = false;
static sky_vert_t* s_sky_mapped[MAX_FRAMES_IN_FLIGHT];

static qboolean EnsureVBO(void)
{
    if (s_vbo_ready) return true;
    const VkDeviceSize sz = sizeof(sky_vert_t) * 36;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (!VK_CreateBuffer(sz, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &s_sky_vbo[i])) return false;
        if (!VK_MapBuffer(&s_sky_vbo[i])) return false;
        s_sky_mapped[i] = (sky_vert_t*)s_sky_vbo[i].mapped;
    }
    s_vbo_ready = true;
    return true;
}

void VK_Sky_Shutdown(void)
{
    if (s_vbo_ready) {
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
            VK_DestroyBuffer(&s_sky_vbo[i]);
        s_vbo_ready = false;
    }
    memset(s_sky_images, 0, sizeof(s_sky_images));
    s_sky_name[0] = 0;
}

void VK_Sky_Render(const refdef_t* fd, const float vieworg[3], const float* mvp)
{
    (void)fd;
    if (!s_sky_name[0] || !vk_state.frame_started) return;
    if (!EnsureVBO()) return;

    const uint32_t frame = vk_state.current_frame;
    VkCommandBuffer cb = vk_state.command_buffers[frame];
    sky_vert_t* vb = s_sky_mapped[frame];

    // Resolve each sky face's descriptor once, from the image (eviction-safe;
    // NULL if the sky image was freed). Build and draw loops must agree on which
    // faces are skipped, since vertex offsets are face*6.
    VkDescriptorSet face_desc[6];
    for (int i = 0; i < 6; i++)
        face_desc[i] = VK_ImageWorldDescriptor(s_sky_images[i]);

    int v = 0;
    for (int face = 0; face < 6; face++) {
        const int img_idx = k_tex_order[face];
        if (face_desc[img_idx] == VK_NULL_HANDLE) { v += 6; continue; }
        float p[3], uv[2];
        const float corners[4][2] = {
            { -1.0f, -1.0f }, { -1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, -1.0f }
        };
        const int tri_idx[6] = {0,1,2, 0,2,3};
        for (int k = 0; k < 6; k++) {
            const float* c = corners[tri_idx[k]];
            MakeSkyVec(c[0], c[1], face, p, uv);
            vb[v].x = p[0] + vieworg[0];
            vb[v].y = p[1] + vieworg[1];
            vb[v].z = p[2] + vieworg[2];
            vb[v].u = uv[0]; vb[v].v = uv[1];
            vb[v].nx = 0.0f; vb[v].ny = 0.0f; vb[v].nz = 1.0f;  // unused (no reflection)
            v++;
        }
    }

    // Sky uses the opaque entity pipeline (1 sampler, mvp+tint+fog push).
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_3d.pipeline);
    {
        float pc[32];
        memcpy(pc, mvp, 64);
        pc[16] = pc[17] = pc[18] = pc[19] = 1.0f;   // tint
        // When a level/underwater fog is active, ref_gl1 keeps GL_FOG enabled
        // while drawing the sky, so the sky blends into the fog at the horizon
        // (in a heavy swamp fog the sky is almost entirely replaced by fog
        // colour). Feed the real fog params here, but force the fog distance to
        // the far plane (sky is the background) so the sky takes the maximum
        // fog amount. If no fog is active, FillSkyFog writes mode=-1 (off).
        VK_World_FillSkyFog(pc);   // writes pc[20..31] (fog_cam/color/extra)
        vkCmdPushConstants(cb, vk_pipeline_3d.layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), pc);
    }

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &s_sky_vbo[frame].buffer, &offset);

    // The entity pipeline layout declares set 1 (dlight UBO); bind a valid set
    // even though the sky shader ignores it (dlight disabled via fog_extra.z=0).
    {
        VkDescriptorSet dlset = VK_World_CurrentDlightSet();
        if (dlset != VK_NULL_HANDLE)
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    vk_pipeline_3d.layout, 1, 1, &dlset, 0, NULL);
    }

    for (int face = 0; face < 6; face++) {
        const int img_idx = k_tex_order[face];
        if (face_desc[img_idx] == VK_NULL_HANDLE) continue;
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vk_pipeline_3d.layout, 0, 1, &face_desc[img_idx], 0, NULL);
        vkCmdDraw(cb, 6, 1, face * 6, 0);
    }

    vk_state.pipeline_2d_bound = false;
}
