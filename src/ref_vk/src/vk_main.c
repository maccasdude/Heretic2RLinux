//
// vk_main.c - GetRefAPI entry point and refexport_t stubs.
//
// Session 1: minimal viable renderer module. The engine can load us,
// the menu can pick us, we initialize Vulkan, and we present a
// solid-coloured frame each tick. Everything else is a stub.
//

#include "vk_local.h"
#include "vk_draw.h"
#include "vk_image.h"
#include "vk_buffer.h"
#include "vk_pipeline.h"
#include "vk_world.h"
#include "vk_model.h"
#include "vk_sprite.h"
#include "vk_engine_model.h"
#include "vk_particles.h"
#include "vk_sky.h"
#include "vk_lightpoint.h"
#include "client/vid.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

refimport_t ri;

// Our own copy of viddef, populated by Vid_GetModeInfo via the import callback.
// The engine maintains its own viddef inside libquake2; both modules must stay
// in sync via the GLimp_InitGraphics call sequence.
viddef_t viddef;

static cvar_t* vid_mode_cvar;
static cvar_t* r_lightlevel_cvar;   // HACK shared with the server for AI sight
static cvar_t* s_farclip_cvar;      // r_farclipdist - far plane distance (match gl1)
static cvar_t* s_under_surface_cvar;   // cl_camera_under_surface - camera in water
static cvar_t* s_underwater_color_cvar; // r_underwater_color - underwater tint (0x70c06000)

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------

static qboolean R_Init(void)
{
    ri.Con_Printf(PRINT_ALL, "Refresh: " REF_TITLE "\n");

    // Need vid_mode to know what resolution the user picked.
    vid_mode_cvar = ri.Cvar_Get("vid_mode", "0", CVAR_ARCHIVE);

    // r_lightlevel: the renderer samples world lighting at the player position
    // and stores it here each frame; the client packs it into the movement
    // command and the server's monster AI uses it to decide if it can see the
    // player. Must match ref_gl1, or monsters never notice the player.
    r_lightlevel_cvar = ri.Cvar_Get("r_lightlevel", "0", 0);

    // Far clip distance, shared with ref_gl1's default so both renderers frame
    // the same depth range.
    s_farclip_cvar = ri.Cvar_Get("r_farclipdist", "4096.0", 0);

    int width  = 0;
    int height = 0;
    if (!ri.Vid_GetModeInfo(&width, &height, (int)vid_mode_cvar->value)) {
        ri.Con_Printf(PRINT_ALL, "vk: invalid vid_mode %d, falling back to mode 0\n",
                      (int)vid_mode_cvar->value);
        ri.Cvar_SetValue("vid_mode", 0);
        if (!ri.Vid_GetModeInfo(&width, &height, 0)) {
            ri.Con_Printf(PRINT_ALL, "vk: Vid_GetModeInfo(0) failed too\n");
            return false;
        }
    }

    viddef.width  = width;
    viddef.height = height;

    // This is the call that asks the engine to create the SDL window. The
    // engine then calls back into R_PrepareForWindow (for window flags) and
    // R_InitContext (with the freshly-made window pointer) - that's where
    // VK_InitContext runs to set up the Vulkan instance/device/swapchain.
    if (!ri.GLimp_InitGraphics(width, height)) {
        ri.Con_Printf(PRINT_ALL, "vk: GLimp_InitGraphics failed\n");
        return false;
    }

    if (!vk_state.initialized) {
        // InitContext should have logged its own error.
        ri.Con_Printf(PRINT_ALL, "vk: Vulkan context not initialized after GLimp_InitGraphics\n");
        return false;
    }

    ri.Con_Printf(PRINT_ALL, "vk: renderer ready (textured world + 2D; no lightmaps/entities yet)\n");
    return true;
}

static void R_Shutdown(void)
{
    // VK_ShutdownContext is called separately via the ShutdownContext entry.
    // R_Shutdown is for renderer-internal data (caches, textures, models)
    // which we don't have any of yet.
}

// ---------------------------------------------------------------------------
// Window plumbing
// ---------------------------------------------------------------------------

static int R_PrepareForWindow(void)
{
    // Tell the engine to create a Vulkan-capable window.
    return SDL_WINDOW_VULKAN;
}

static qboolean R_InitContext(void* sdl_window)
{
    return VK_InitContext((SDL_Window*)sdl_window);
}

static void R_ShutdownContext(void)
{
    VK_ShutdownContext();
}

// ---------------------------------------------------------------------------
// Stubs for everything else
// ---------------------------------------------------------------------------

static int s_reg_seq = 1;   // shared registration sequence (images + models)

static void              R_BeginRegistration(const char* map)
{
    if (!map || !*map) return;

    // Bump the registration sequence so everything (re)touched while this map
    // loads is stamped current; assets left stale belong to the previous level
    // and get evicted in R_EndRegistration. Push the new value to the image and
    // model caches before loading anything.
    s_reg_seq++;
    VK_Image_SetRegSeq(s_reg_seq);
    VK_Model_SetRegSeq(s_reg_seq);

    // New map: reset the engine-model wrapper list so handles are rebuilt fresh
    // (submodel indices point into the world we're about to load). Prevents the
    // wrapper list from growing every map load and overflowing.
    VK_EModel_BeginRegistration();

    // map is something like "maps/silverspring.bsp".
    char path[256];
    if (strstr(map, ".bsp") != NULL)
        snprintf(path, sizeof(path), "%s", map);
    else
        snprintf(path, sizeof(path), "maps/%s.bsp", map);

    if (!VK_World_LoadMap(path)) {
        ri.Con_Printf(PRINT_ALL, "vk: world load failed for '%s'; rendering will skip the 3D pass\n", path);
    }
}
static struct model_s*   R_RegisterModel(const char* name)
{
    return (struct model_s*)VK_EModel_Register(name);
}
static struct image_s*   R_RegisterSkin(const char* name, qboolean* retval)
{
    // Skins are referenced by full path (e.g. "players/male/Corvus.m8"), so
    // use VK_FindImage (no "pics/" prefix). Images get a descriptor at
    // creation time, so it's ready for model rendering.
    image_t* img = VK_FindImage(name);
    if (retval) *retval = (img != NULL);
    return (struct image_s*)img;
}
static struct image_s*   R_RegisterPic(const char* name)
{
    return (struct image_s*)VK_FindPic(name);
}
static void              R_SetSky(const char* name, float rotate, const vec3_t axis)
{
    VK_Sky_Set(name, rotate, axis);
}
static void              R_EndRegistration(void)
{
    // All of this map's models/skins/textures have now been registered and
    // stamped with the current sequence. Evict everything left over from the
    // previous level. Wait for the GPU to finish any in-flight frames first,
    // since we're about to destroy textures and free descriptor sets that
    // earlier frames referenced. A level load is already a stall, so the idle
    // wait here is not a hot-path cost. Matches ref_gl1 RI_EndRegistration
    // (Mod_Free unused + R_FreeUnusedImages).
    if (vk_state.device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(vk_state.device);

    VK_Model_FreeUnused();
    VK_FreeUnusedImages();
}
static int               R_GetReferencedID(const struct model_s* model)
{
    if (!model) return -1;
    const vk_engine_model_t* em = (const vk_engine_model_t*)model;
    if (em->type != VK_EMODEL_FLEX || !em->impl) return -1;
    return VK_Model_ReferenceType((const vk_model_t*)em->impl);
}

// Build MVP matching what VK_World_Render does, so models live in the same
// world space as the BSP geometry.
typedef float mat4_t[16];
static void m4_identity(mat4_t o) { memset(o,0,sizeof(mat4_t)); o[0]=o[5]=o[10]=o[15]=1; }
static void m4_mul(mat4_t out, const mat4_t a, const mat4_t b) {
    mat4_t r; for (int c=0;c<4;c++) for (int rr=0;rr<4;rr++) {
        float s=0; for (int k=0;k<4;k++) s+=a[k*4+rr]*b[c*4+k]; r[c*4+rr]=s;
    } memcpy(out,r,sizeof(r));
}
static void m4_persp(mat4_t o, float fy, float aspect, float zn, float zf) {
    const float f=1.0f/tanf(fy*0.5f); memset(o,0,sizeof(mat4_t));
    o[0]=f/aspect; o[5]=-f; o[10]=zf/(zn-zf); o[11]=-1; o[14]=(zn*zf)/(zn-zf);
}
static void m4_rot(mat4_t o, float deg, float ax, float ay, float az) {
    const float rad=deg*3.14159265358979f/180.0f, c=cosf(rad), s=sinf(rad), omc=1-c;
    float l=sqrtf(ax*ax+ay*ay+az*az); if (l>0.0001f) {ax/=l;ay/=l;az/=l;}
    m4_identity(o);
    o[0]=c+ax*ax*omc;    o[1]=ay*ax*omc+az*s; o[2]=az*ax*omc-ay*s;
    o[4]=ax*ay*omc-az*s; o[5]=c+ay*ay*omc;    o[6]=az*ay*omc+ax*s;
    o[8]=ax*az*omc+ay*s; o[9]=ay*az*omc-ax*s; o[10]=c+az*az*omc;
}
static void m4_trans(mat4_t o, float x, float y, float z) {
    m4_identity(o); o[12]=x; o[13]=y; o[14]=z;
}

static void BuildViewMVP(mat4_t out, const refdef_t* fd)
{
    mat4_t view, proj, m, tmp, r;
    // View - same sequence as VK_World_Render's make_view_matrix.
    m4_rot(m, -90, 1,0,0);
    m4_rot(r,  90, 0,0,1); m4_mul(tmp,m,r); memcpy(m,tmp,sizeof(m));
    m4_rot(r, -fd->viewangles[2], 1,0,0); m4_mul(tmp,m,r); memcpy(m,tmp,sizeof(m));
    m4_rot(r, -fd->viewangles[0], 0,1,0); m4_mul(tmp,m,r); memcpy(m,tmp,sizeof(m));
    m4_rot(r, -fd->viewangles[1], 0,0,1); m4_mul(tmp,m,r); memcpy(m,tmp,sizeof(m));
    m4_trans(r, -fd->vieworg[0], -fd->vieworg[1], -fd->vieworg[2]);
    m4_mul(view, m, r);

    const float aspect = (fd->width>0 && fd->height>0) ? (float)fd->width/(float)fd->height : 1.0f;
    // Match ref_gl1's R_SetPerspective: near plane 1.0 (H2 lowered it from
    // Quake2's 4.0 - the comment in gl1_Main.c reads "// Q2: 4.0"), far plane
    // from r_farclipdist (default 4096). A near plane of 4.0 clips world
    // surfaces within 4 units of the camera, so wall edges the camera is right
    // up against vanish (you see through them) - exactly the VK-only clipping.
    const float zfar = (s_farclip_cvar && s_farclip_cvar->value > 0.0f)
                       ? s_farclip_cvar->value : 4096.0f;
    m4_persp(proj, fd->fov_y * 3.14159265358979f / 180.0f, aspect, 1.0f, zfar);
    m4_mul(out, proj, view);
}

extern void VK_Model_BeginFrame(void);   // forward decl (in vk_model.c)
extern void VK_Model_SetFrameTime(float t); // forward decl (in vk_model.c)
extern void VK_Model_SetViewOrigin(const float org[3]); // forward decl (in vk_model.c)
extern void VK_Sprite_BeginFrame(void);  // forward decl (in vk_sprite.c)

// Compute view-aligned up/right vectors for sprite billboarding from the
// camera's viewangles (radians? In refdef_t.viewangles, GL1's R_SetupFrame
// passes them as degrees to AngleVectors. Check the engine convention.)
static void ViewUpRight(const refdef_t* fd, float up[3], float right[3], float fwd[3])
{
    // viewangles[0]=pitch, [1]=yaw, [2]=roll. GL1 treats these as DEGREES
    // when passed to AngleVectors (per R_SetupFrame in gl1_Main.c).
    const float DEG2RAD = 3.14159265358979f / 180.0f;
    const float p = fd->viewangles[0] * DEG2RAD;
    const float y = fd->viewangles[1] * DEG2RAD;
    const float r = fd->viewangles[2] * DEG2RAD;

    const float cp = cosf(p), sp = sinf(p);
    const float cy = cosf(y), sy = sinf(y);
    const float cr = cosf(r), sr = sinf(r);

    // AngleVectors: forward, right, up (right-handed Quake basis).
    // forward = ( cp*cy,  cp*sy, -sp)
    // right   = (-sr*sp*cy + cr*sy, -sr*sp*sy - cr*cy, -sr*cp) * (-1?)
    // Quake's "right" returned by AngleVectors points to the player's LEFT
    // mathematically; the math below mirrors AngleVectors exactly.
    right[0] = -sr*sp*cy + -cr*-sy;
    right[1] = -sr*sp*sy + -cr*cy;
    right[2] = -sr*cp;
    up[0]    =  cr*sp*cy + -sr*-sy;
    up[1]    =  cr*sp*sy + -sr*cy;
    up[2]    =  cr*cp;
    if (fwd) {                       // view forward (vpn) for oriented line sprites
        fwd[0] = cp*cy;
        fwd[1] = cp*sy;
        fwd[2] = -sp;
    }
}

static void DrawOneEntity(entity_t* e, const float* mvp,
                          const float vup[3], const float vright[3], const float vfwd[3])
{
    if (!e || !e->model || !*e->model) return;
    const vk_engine_model_t* em = (const vk_engine_model_t*)(*e->model);
    switch (em->type) {
        case VK_EMODEL_FLEX:
            VK_Model_DrawEntity(e, mvp);
            break;
        case VK_EMODEL_SPRITE:
            VK_Sprite_DrawEntity(e, (const vk_sprite_t*)em->impl, vup, vright, vfwd, mvp);
            break;
        case VK_EMODEL_SUBMODEL:
            VK_World_RenderSubmodel(em->submodel_index, mvp, e->origin, e->angles, e->frame);
            break;
        default:
            break;
    }
}

static void DrawEntityList(entity_t** ents, int count, const float* mvp,
                           const float vup[3], const float vright[3], const float vfwd[3])
{
    for (int i = 0; i < count; i++)
        DrawOneEntity(ents[i], mvp, vup, vright, vfwd);
}

// Context + callback for interleaving alpha entities with the water pass.
typedef struct {
    const float* mvp;
    const float* vup;
    const float* vright;
    const float* vfwd;
} AlphaEntDrawCtx;

static void DrawAlphaEntityCB(struct entity_s* e, void* user)
{
    const AlphaEntDrawCtx* c = (const AlphaEntDrawCtx*)user;
    DrawOneEntity((entity_t*)e, c->mvp, c->vup, c->vright, c->vfwd);
}

static int               R_RenderFrame(const refdef_t* fd)
{
    if (!fd) return 0;
    if (fd->rdflags & RDF_NOWORLDMODEL) return 0;

    VK_Model_BeginFrame();
    VK_Model_SetFrameTime(fd->time);
    VK_Model_SetViewOrigin(fd->vieworg);
    VK_Sprite_BeginFrame();
    VK_Particles_BeginFrame();

    // Animated lightstyles: re-bake + upload any world surfaces whose flicker/
    // pulse styles changed this frame (before the world is drawn).
    VK_World_UpdateLightstyles(fd);

    mat4_t mvp;
    BuildViewMVP(mvp, fd);

    // Sky first (it'll write depth at far plane; world will overwrite).
    VK_Sky_Render(fd, fd->vieworg, mvp);

    VK_World_Render(fd);

    float vup[3], vright[3], vfwd[3];
    ViewUpRight(fd, vup, vright, vfwd);

    if (fd->num_entities > 0 && fd->entities)
        DrawEntityList(fd->entities, fd->num_entities, mvp, vup, vright, vfwd);

    // Translucent world surfaces (water/glass/forcefields) interleaved with the
    // translucent ENTITY billboards/sprites by depth, back-to-front - matching
    // ref_gl1 R_SortAndDrawAlphaSurfaces, which merges alpha entities + alpha
    // surfaces into one depth-sorted list. This is what keeps a crosshair or
    // splash in front of the opaque swamp water drawn ON TOP of it, while
    // submerged things behind clear (TRANS33/66) water still draw behind it.
    // The client has already sorted alpha_entities back-to-front.
    {
        AlphaEntDrawCtx ctx = { mvp, vup, vright, vfwd };
        VK_World_RenderWater(fd, fd->alpha_entities, fd->num_alpha_entities,
                             DrawAlphaEntityCB, &ctx);
    }

    // Particles after all entities (they're alpha/additive blended).
    VK_Particles_Render(fd, vup, vright, mvp);

    // Full-screen screen-flash overlay (underwater tint, damage flash, item
    // pickup, powerups). Ported from ref_gl1 RI_RenderFrame + R_ScreenFlash:
    // when the camera is under a water surface the color is r_underwater_color
    // (default 0x70c06000 - blue at ~44% alpha); otherwise it's whatever the
    // client's screen-flash system reports (damage/pickup/powerup). Drawn as a
    // fullscreen blended quad, exactly like GL1's Draw_FadeScreen.
    {
        if (!s_under_surface_cvar) s_under_surface_cvar = ri.Cvar_Get("cl_camera_under_surface", "0", 0);
        if (!s_underwater_color_cvar) s_underwater_color_cvar = ri.Cvar_Get("r_underwater_color", "0x70c06000", 0);

        paletteRGBA_t color;
        if (s_under_surface_cvar && s_under_surface_cvar->value != 0.0f)
            color.c = (uint)strtoul(s_underwater_color_cvar->string, NULL, 0);
        else
            color.c = (uint)(ri.Is_Screen_Flashing ? ri.Is_Screen_Flashing() : 0);

        if (color.a != 0)
            VK_Draw_FadeScreen(color);

        if (ri.Deactivate_Screen_Flash)
            ri.Deactivate_Screen_Flash();
    }

    // Sample world lighting at the player's position and publish it via
    // r_lightlevel (the server's monster AI reads this to decide whether the
    // player is visible). Matches ref_gl1's R_SetLightLevel.
    if (r_lightlevel_cvar) {
        const float lv = VK_LightPoint_Sample(fd->clientmodelorg);
        ri.Cvar_SetValue("r_lightlevel", lv * 150.0f);
    }

    return 0;
}

static void              R_DrawGetPicSize(int* w, int* h, const char* name)                   { VK_Draw_GetPicSize(w, h, name); }
static void              R_DrawPic(int x, int y, int s, const char* n, float a)               { VK_Draw_Pic(x, y, s, n, a); }
static void              R_DrawStretchPic(int x, int y, int w, int h, const char* n, float a, DrawStretchPicScaleMode_t m) { VK_Draw_StretchPic(x, y, w, h, n, a, m); }
static void              R_DrawChar(int x, int y, int s, int c, paletteRGBA_t col, qboolean sh){ VK_Draw_Char(x, y, s, c, col, sh); }
static void              R_DrawTileClear(int x, int y, int w, int h, const char* n)           { VK_Draw_TileClear(x, y, w, h, n); }
static void              R_DrawFill(int x, int y, int w, int h, paletteRGBA_t col)            { VK_Draw_Fill(x, y, w, h, col); }
static void              R_DrawFadeScreen(paletteRGBA_t col)                                  { VK_Draw_FadeScreen(col); }
static void              R_DrawBigFont(int x, int y, const char* text, float alpha)           { VK_Draw_BigFont(x, y, text, alpha); }
static int               R_BF_Strlen(const char* text)                                        { return VK_BF_Strlen(text); }
static void              R_BookDrawPic(const char* n, float scale, float alpha)               { VK_BookDrawPic(n, scale, alpha); }

// Cinematic: H2 hands us a stream of 8-bit indexed frames + a current palette
// every tick. We keep one streaming linear-tiled texture sized to the
// cinematic and re-upload on each frame, then stretch-blit it across the
// viewport in EndFrame (so menus drawn on top of the cinematic compose right).
//
// vk_main.c-local because the cinematic state is owned here rather than in
// the draw module.
static vk_texture_t  cinematic_tex   = {0};
static VkDescriptorSet cinematic_desc = VK_NULL_HANDLE;
static int           cinematic_w = 0, cinematic_h = 0;
static qboolean      cinematic_active = false;

static void              R_DrawInitCinematic(int w, int h)
{
    if (cinematic_active) {
        // size change - tear down first
        if (cinematic_desc) {
            vkFreeDescriptorSets(vk_state.device, vk_pipeline_2d.descriptor_pool, 1, &cinematic_desc);
            cinematic_desc = VK_NULL_HANDLE;
        }
        VK_DestroyTexture(&cinematic_tex);
        cinematic_active = false;
    }
    if (w <= 0 || h <= 0) return;
    if (!VK_CreateStreamingTextureRGBA((uint32_t)w, (uint32_t)h, &cinematic_tex))
        return;
    cinematic_desc = VK_AllocDescriptorSetFor(cinematic_tex.view);
    if (cinematic_desc == VK_NULL_HANDLE) {
        VK_DestroyTexture(&cinematic_tex);
        return;
    }
    cinematic_w      = w;
    cinematic_h      = h;
    cinematic_active = true;
}
static void              R_DrawCloseCinematic(void)
{
    if (!cinematic_active) return;
    vkDeviceWaitIdle(vk_state.device);
    if (cinematic_desc) {
        // Descriptor sets can't be freed individually unless the pool was
        // created with FREE_DESCRIPTOR_SET; ours isn't, so we just orphan it
        // for now. The pool is large enough for our use.
        cinematic_desc = VK_NULL_HANDLE;
    }
    VK_DestroyTexture(&cinematic_tex);
    cinematic_active = false;
    cinematic_w = cinematic_h = 0;
}
static void              R_DrawCinematic(const byte* data, const paletteRGB_t* pal)
{
    if (!cinematic_active || !data || !pal) return;
    if (!vk_state.frame_started) return;

    // Expand indexed -> RGBA.
    const int npix = cinematic_w * cinematic_h;
    static byte* rgba_buf = NULL;
    static int   rgba_buf_size = 0;
    if (rgba_buf_size < npix * 4) {
        rgba_buf = realloc(rgba_buf, (size_t)npix * 4);
        rgba_buf_size = npix * 4;
    }
    if (!rgba_buf) return;

    for (int i = 0; i < npix; i++) {
        const paletteRGB_t* p = &pal[data[i]];
        rgba_buf[i*4 + 0] = p->r;
        rgba_buf[i*4 + 1] = p->g;
        rgba_buf[i*4 + 2] = p->b;
        rgba_buf[i*4 + 3] = 255;
    }
    VK_UpdateStreamingTexture(&cinematic_tex, rgba_buf);

    // Stretch-blit across the viewport via the 2D pipeline. We can't call
    // VK_Draw_StretchPic directly since the cinematic isn't a named image -
    // emit a single quad with the cinematic descriptor.
    extern void VK_DrawDirectQuad(VkDescriptorSet desc, float x, float y, float w, float h,
                                  float u0, float v0, float u1, float v1,
                                  paletteRGBA_t color);
    paletteRGBA_t white = { 255, 255, 255, 255 };
    VK_DrawDirectQuad(cinematic_desc,
                      0.0f, 0.0f,
                      (float)vk_state.swapchain_extent.width,
                      (float)vk_state.swapchain_extent.height,
                      0.0f, 0.0f, 1.0f, 1.0f, white);
}
static void              R_Draw_Name(const vec3_t origin, const char* n, paletteRGBA_t col)   { (void)origin;(void)n;(void)col; }

static void              R_BeginFrame(float cs)                                               { VK_BeginFrame_impl(cs); }
static void              R_EndFrame(void)                                                     { VK_EndFrame_impl(); }

static int               R_FindSurface(const vec3_t start, const vec3_t end, struct Surface_s* surface) { (void)start;(void)end;(void)surface; return 0; }

#ifdef _DEBUG
static void              R_AddDebugBox(const vec3_t c, float s, paletteRGBA_t col, float lt)                                  { (void)c;(void)s;(void)col;(void)lt; }
static void              R_AddDebugBbox(const vec3_t mn, const vec3_t mx, paletteRGBA_t col, float lt)                        { (void)mn;(void)mx;(void)col;(void)lt; }
static void              R_AddDebugEntityBbox(const edict_t* e, paletteRGBA_t col)                                            { (void)e;(void)col; }
static void              R_AddDebugLabel(const vec3_t o, paletteRGBA_t col, float lt, const char* l)                          { (void)o;(void)col;(void)lt;(void)l; }
static void              R_AddDebugEntityLabel(const edict_t* e, paletteRGBA_t col, const char* l)                            { (void)e;(void)col;(void)l; }
static void              R_AddDebugLine(const vec3_t s, const vec3_t e, paletteRGBA_t col, float lt)                          { (void)s;(void)e;(void)col;(void)lt; }
static void              R_AddDebugArrow(const vec3_t s, const vec3_t e, paletteRGBA_t col, float lt)                         { (void)s;(void)e;(void)col;(void)lt; }
static void              R_AddDebugDirection(const vec3_t s, const vec3_t d, float sz, paletteRGBA_t col, float lt)           { (void)s;(void)d;(void)sz;(void)col;(void)lt; }
static void              R_AddDebugAngles(const vec3_t s, const vec3_t a, float sz, paletteRGBA_t col, float lt)              { (void)s;(void)a;(void)sz;(void)col;(void)lt; }
static void              R_AddDebugAnglesRad(const vec3_t s, const vec3_t a, float sz, paletteRGBA_t col, float lt)           { (void)s;(void)a;(void)sz;(void)col;(void)lt; }
static void              R_AddDebugMarker(const vec3_t c, float sz, paletteRGBA_t col, float lt)                              { (void)c;(void)sz;(void)col;(void)lt; }
static void              R_FreeDebugPrimitives(void)                                                                          { }
#endif

// ---------------------------------------------------------------------------
// GetRefAPI - the one symbol the engine dlsym's
// ---------------------------------------------------------------------------

REF_DECLSPEC refexport_t GetRefAPI(const refimport_t rimp)
{
    refexport_t re;

    ri = rimp;
    memset(&re, 0, sizeof(re));

    re.api_version = REF_API_VERSION;
    re.title       = REF_TITLE;

    re.BeginRegistration = R_BeginRegistration;
    re.RegisterModel     = R_RegisterModel;
    re.RegisterSkin      = R_RegisterSkin;
    re.RegisterPic       = R_RegisterPic;
    re.SetSky            = R_SetSky;
    re.EndRegistration   = R_EndRegistration;
    re.GetReferencedID   = R_GetReferencedID;

    re.RenderFrame       = R_RenderFrame;

    re.DrawGetPicSize    = R_DrawGetPicSize;
    re.DrawPic           = R_DrawPic;
    re.DrawStretchPic    = R_DrawStretchPic;
    re.DrawChar          = R_DrawChar;
    re.DrawTileClear     = R_DrawTileClear;
    re.DrawFill          = R_DrawFill;
    re.DrawFadeScreen    = R_DrawFadeScreen;

    re.DrawBigFont       = R_DrawBigFont;
    re.BF_Strlen         = R_BF_Strlen;
    re.BookDrawPic       = R_BookDrawPic;

    re.DrawInitCinematic  = R_DrawInitCinematic;
    re.DrawCloseCinematic = R_DrawCloseCinematic;
    re.DrawCinematic      = R_DrawCinematic;
    re.Draw_Name          = R_Draw_Name;

    re.Init     = R_Init;
    re.Shutdown = R_Shutdown;

    re.BeginFrame = R_BeginFrame;
    re.EndFrame   = R_EndFrame;
    re.FindSurface = R_FindSurface;

    re.PrepareForWindow  = R_PrepareForWindow;
    re.InitContext       = R_InitContext;
    re.ShutdownContext   = R_ShutdownContext;

#ifdef _DEBUG
    re.AddDebugBox          = R_AddDebugBox;
    re.AddDebugBbox         = R_AddDebugBbox;
    re.AddDebugEntityBbox   = R_AddDebugEntityBbox;
    re.AddDebugLabel        = R_AddDebugLabel;
    re.AddDebugEntityLabel  = R_AddDebugEntityLabel;
    re.AddDebugLine         = R_AddDebugLine;
    re.AddDebugArrow        = R_AddDebugArrow;
    re.AddDebugDirection    = R_AddDebugDirection;
    re.AddDebugAngles       = R_AddDebugAngles;
    re.AddDebugAnglesRad    = R_AddDebugAnglesRad;
    re.AddDebugMarker       = R_AddDebugMarker;
    re.FreeDebugPrimitives  = R_FreeDebugPrimitives;
#endif

    return re;
}
