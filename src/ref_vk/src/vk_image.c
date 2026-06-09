//
// vk_image.c - Texture cache + .m32/.m8 loaders.
//

#include "vk_local.h"
#include "vk_buffer.h"
#include "vk_pipeline.h"
#include "vk_pipeline3d.h"   // VK_AllocWorldDescriptor / VK_FreeWorldDescriptor
#include "vk_image.h"

#include "qcommon/qfiles.h"   // miptex_t, miptex32_t

#include <string.h>
#include <stdlib.h>
#include <math.h>

// We define our own image_s. The engine treats it as opaque - it just gets a
// pointer back from RegisterPic / RegisterSkin and hands it to draw calls.
// vk_image_t* is what the engine's `struct image_s*` actually points to.
typedef struct image_s {
    char            name[256];
    vk_texture_t    tex;
    VkDescriptorSet descriptor;        // 2D pipeline set (pics, fonts, cinematic)
    VkDescriptorSet world_descriptor;  // 3D pipeline set (model skins, sprites, sky); lazy
    int             width, height;
    qboolean        has_alpha;   // texture has any non-opaque texel (matches GL image->has_alpha)
    int             reg_seq;     // registration sequence this image was last used in
    qboolean        permanent;   // never evicted (conchars, fonts, reflect, particle, white)
    qboolean        used;
} image_t;

// Strictly an array of slots. RegisterPic returns &textures[i].
static image_t   textures[VK_MAX_TEXTURES];
static int       texture_count = 0;

// Registration sequence: bumped at the start of each map load. Every image
// touched during a map's registration (and every frame, via FindByName) is
// stamped with the current value; VK_FreeUnusedImages() evicts images whose
// stamp is stale (loaded for a level we've since left). Mirrors ref_gl1's
// registration_sequence / R_FreeUnusedImages.
static int       s_image_reg_seq = 1;

void VK_Image_SetRegSeq(int seq) { s_image_reg_seq = seq; }

// Re-stamp an already-resolved image as used in the current registration
// sequence, so VK_FreeUnusedImages spares it. Needed by callers that cache an
// image_t* across registrations and have a cache-hit path that skips the normal
// VK_FindImage re-resolve (model skins, sprite frames, sky). Without it those
// cached images get evicted while still referenced by cached descriptors.
void VK_Image_Touch(image_t* img) { if (img) img->reg_seq = s_image_reg_seq; }

// 1x1 fully-white texture so DrawFill / DrawChar can render solid coloured
// quads through the same textured pipeline.
static image_t*  white_texture = NULL;

// ---------------------------------------------------------------------------
// Gamma / brightness / contrast
//
// H2's GL renderer does not use a hardware gamma ramp; it bakes vid_gamma,
// vid_brightness and vid_contrast into texture albedo at load time via a 256
// entry byte LUT (ref_gl1 R_InitGammaTable / R_ApplyGamma32 / GrabPalette),
// applied before lighting and blending. We mirror that exactly so the menu
// sliders behave identically under Vulkan: build the same LUT, run texel RGB
// through it on every disk-loaded texture, and re-bake from disk when a slider
// changes (ref_gl1 RI_BeginFrame). The lightmap atlas is built directly (not
// via LoadM32/M8), so like GL it is correctly left ungamma'd.
// ---------------------------------------------------------------------------

static cvar_t*  vid_gamma      = NULL;
static cvar_t*  vid_brightness = NULL;
static cvar_t*  vid_contrast   = NULL;
static byte     s_gammatable[256];
static qboolean s_gamma_ready  = false;

// Port of ref_gl1 R_InitGammaTable (gl1_Image.c) - identical math. Registers the
// cvars on first call (defaults match GL: 0.5, CVAR_ARCHIVE).
static void VK_InitGammaTable(void)
{
    if (!vid_gamma) {
        vid_gamma      = ri.Cvar_Get("vid_gamma",      "0.5", CVAR_ARCHIVE);
        vid_brightness = ri.Cvar_Get("vid_brightness", "0.5", CVAR_ARCHIVE);
        vid_contrast   = ri.Cvar_Get("vid_contrast",   "0.5", CVAR_ARCHIVE);
    }

    float contrast = 1.0f - vid_contrast->value;
    if (contrast > 0.5f) contrast = powf(contrast + 0.5f, 3.0f);
    else                 contrast = powf(contrast + 0.5f, 0.5f);

    s_gammatable[0] = 0;
    for (int i = 1; i < 256; i++) {
        float inf = 255.0f * powf(((float)i + 0.5f) / 255.5f, vid_gamma->value) + 0.5f;
        float sign;
        if (inf < 128.0f) { inf = 128.0f - inf; sign = -1.0f; }
        else              { inf -= 128.0f;      sign =  1.0f; }
        inf = (vid_brightness->value * 160.0f - 80.0f)
              + (powf(inf / 128.0f, contrast) * sign + 1.0f) * 128.0f;
        int v = (int)inf;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        s_gammatable[i] = (byte)v;
    }
    s_gamma_ready = true;
}

// Run the RGB of an RGBA pixel run through the gamma LUT (alpha preserved).
static void ApplyGammaRGBA(paletteRGBA_t* px, size_t count)
{
    if (!s_gamma_ready) VK_InitGammaTable();
    for (size_t i = 0; i < count; i++) {
        px[i].r = s_gammatable[px[i].r];
        px[i].g = s_gammatable[px[i].g];
        px[i].b = s_gammatable[px[i].b];
    }
}

// Decode an already-resolved image file (name carries its .m32/.m8 extension)
// into a freshly malloc'd, gamma-applied RGBA buffer. Caller frees. Used by both
// the initial load paths and the gamma-refresh path so the two never diverge.
static paletteRGBA_t* DecodeImageRGBA(const char* name, int* out_w, int* out_h)
{
    const size_t len = strlen(name);
    const qboolean is_m8 = (len >= 3 && strcasecmp(name + len - 3, ".m8") == 0);

    if (is_m8) {
        miptex_t* mt = NULL;
        ri.FS_LoadFile(name, (void**)&mt);
        if (mt == NULL) return NULL;
        if (mt->version != MIP_VERSION) { ri.FS_FreeFile(mt); return NULL; }
        const int w = (int)mt->width[0];
        const int h = (int)mt->height[0];
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) { ri.FS_FreeFile(mt); return NULL; }
        const byte* indices = (byte*)mt + mt->offsets[0];
        paletteRGBA_t* rgba = malloc((size_t)w * (size_t)h * 4);
        if (!rgba) { ri.FS_FreeFile(mt); return NULL; }
        for (int i = 0; i < w * h; i++) {
            const byte idx = indices[i];
            rgba[i].r = mt->palette[idx].r;
            rgba[i].g = mt->palette[idx].g;
            rgba[i].b = mt->palette[idx].b;
            rgba[i].a = (idx == 255) ? 0 : 255;
        }
        ri.FS_FreeFile(mt);
        ApplyGammaRGBA(rgba, (size_t)w * (size_t)h);
        *out_w = w; *out_h = h;
        return rgba;
    } else {
        miptex32_t* mt = NULL;
        ri.FS_LoadFile(name, (void**)&mt);
        if (mt == NULL) return NULL;
        if (mt->version != MIP32_VERSION) { ri.FS_FreeFile(mt); return NULL; }
        const int w = (int)mt->width[0];
        const int h = (int)mt->height[0];
        const byte* pixels = (byte*)mt + mt->offsets[0];
        paletteRGBA_t* rgba = malloc((size_t)w * (size_t)h * 4);
        if (!rgba) { ri.FS_FreeFile(mt); return NULL; }
        memcpy(rgba, pixels, (size_t)w * (size_t)h * 4);
        ri.FS_FreeFile(mt);
        ApplyGammaRGBA(rgba, (size_t)w * (size_t)h);
        *out_w = w; *out_h = h;
        return rgba;
    }
}

// Called once per frame from VK_BeginFrame_impl. If a slider moved (cvar
// ->modified) or a full refresh was requested (vid_textures_refresh_required,
// set by the video menu on close), rebuild the LUT and re-bake every
// disk-loaded texture in place. Mirrors ref_gl1 RI_BeginFrame + R_GammaAffect.
// The in-place update keeps each image's view and descriptor, so this is
// eviction-safe. A device-wait up front guards against frames still in flight
// (this only runs on an actual change, so the stall is a one-off per notch).
void VK_GammaRefreshIfNeeded(void)
{
    if (!vid_gamma) VK_InitGammaTable();   // registers cvars + builds initial LUT

    cvar_t* refresh_req = ri.Cvar_Get("vid_textures_refresh_required", "0", 0);
    const qboolean changed = vid_gamma->modified || vid_brightness->modified ||
                             vid_contrast->modified;
    const qboolean full = (refresh_req->value == 1.0f);
    if (!changed && !full) return;

    VK_InitGammaTable();
    vkDeviceWaitIdle(vk_state.device);

    for (int i = 0; i < texture_count; i++) {
        image_t* img = &textures[i];
        if (!img->used || img->name[0] == '*') continue;   // skip the white utility tex
        int w = 0, h = 0;
        paletteRGBA_t* rgba = DecodeImageRGBA(img->name, &w, &h);
        if (!rgba) continue;
        if (w == img->width && h == img->height)
            VK_UpdateTextureRGBA(&img->tex, 0, 0, (uint32_t)w, (uint32_t)h,
                                 rgba, (uint32_t)w * 4);
        free(rgba);
    }

    vid_gamma->modified      = false;
    vid_brightness->modified = false;
    vid_contrast->modified   = false;
    if (full) ri.Cvar_SetValue("vid_textures_refresh_required", 0.0f);
}

// ---------------------------------------------------------------------------
// Texture slot allocation
// ---------------------------------------------------------------------------

static image_t* AllocImageSlot(void)
{
    // Reuse any slot freed by VK_FreeUnusedImages before growing the array.
    for (int i = 0; i < texture_count; i++) {
        if (!textures[i].used) {
            image_t* slot = &textures[i];
            memset(slot, 0, sizeof(*slot));
            slot->used = true;
            slot->reg_seq = s_image_reg_seq;
            return slot;
        }
    }
    if (texture_count >= VK_MAX_TEXTURES) {
        ri.Con_Printf(PRINT_ALL, "vk: texture pool exhausted\n");
        return NULL;
    }
    image_t* slot = &textures[texture_count++];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->reg_seq = s_image_reg_seq;   // freshly loaded this sequence
    return slot;
}

static image_t* FindByName(const char* name)
{
    for (int i = 0; i < texture_count; i++) {
        if (textures[i].used && strcasecmp(textures[i].name, name) == 0) {
            textures[i].reg_seq = s_image_reg_seq;   // touched this sequence -> keep
            return &textures[i];
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// 1x1 white texture used as the "no actual texture" fallback
// ---------------------------------------------------------------------------

static qboolean CreateWhiteTexture(void)
{
    if (white_texture) return true;

    static const uint8_t px[4] = { 255, 255, 255, 255 };
    image_t* slot = AllocImageSlot();
    if (!slot) return false;

    strcpy(slot->name, "*white");
    if (!VK_CreateTextureRGBA(1, 1, px, &slot->tex)) return false;
    slot->descriptor = VK_AllocDescriptorSetFor(slot->tex.view);
    if (slot->descriptor == VK_NULL_HANDLE) {
        VK_DestroyTexture(&slot->tex);
        slot->used = false;
        return false;
    }
    slot->width  = 1;
    slot->height = 1;
    slot->permanent = true;    // never evict the white fallback
    white_texture = slot;
    return true;
}

image_t* VK_GetWhiteTexture(void) { return white_texture; }

// ---------------------------------------------------------------------------
// .m32 - 32-bit RGBA mip-chain images. Used for menu pics, conchars, fonts.
// ---------------------------------------------------------------------------

static image_t* LoadM32(const char* name)
{
    miptex32_t* mt = NULL;
    ri.FS_LoadFile(name, (void**)&mt);
    if (mt == NULL) return NULL;

    if (mt->version != MIP32_VERSION) {
        ri.Con_Printf(PRINT_ALL, "vk: %s: invalid .m32 version (%d)\n", name, mt->version);
        ri.FS_FreeFile(mt);
        return NULL;
    }

    image_t* slot = AllocImageSlot();
    if (!slot) { ri.FS_FreeFile(mt); return NULL; }

    strcpy_s(slot->name, sizeof(slot->name), name);

    const int w = (int)mt->width[0];
    const int h = (int)mt->height[0];
    const byte* pixels = (byte*)mt + mt->offsets[0];

    // Bake gamma/brightness/contrast into the albedo via a writable copy, exactly
    // as ref_gl1 R_ApplyGamma32 does (gamma applied before lighting/blending).
    paletteRGBA_t* gpix = malloc((size_t)w * (size_t)h * 4);
    if (!gpix) { slot->used = false; ri.FS_FreeFile(mt); return NULL; }
    memcpy(gpix, pixels, (size_t)w * (size_t)h * 4);
    ApplyGammaRGBA(gpix, (size_t)w * (size_t)h);

    if (!VK_CreateTextureRGBA((uint32_t)w, (uint32_t)h, gpix, &slot->tex)) {
        free(gpix);
        slot->used = false;
        ri.FS_FreeFile(mt);
        return NULL;
    }
    free(gpix);
    slot->descriptor = VK_AllocDescriptorSetFor(slot->tex.view);
    slot->width  = w;
    slot->height = h;
    slot->has_alpha = true;   // GL marks every .m32 image has_alpha = 1

    ri.FS_FreeFile(mt);
    return slot;
}

// ---------------------------------------------------------------------------
// .m8 - 8-bit indexed with embedded 256-entry palette. Used for some assets.
// ---------------------------------------------------------------------------

static image_t* LoadM8(const char* name)
{
    miptex_t* mt = NULL;
    ri.FS_LoadFile(name, (void**)&mt);
    if (mt == NULL) return NULL;

    if (mt->version != MIP_VERSION) {
        ri.Con_Printf(PRINT_ALL, "vk: %s: invalid .m8 version (%d)\n", name, mt->version);
        ri.FS_FreeFile(mt);
        return NULL;
    }

    const int w = (int)mt->width[0];
    const int h = (int)mt->height[0];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        ri.FS_FreeFile(mt);
        return NULL;
    }

    const byte* indices  = (byte*)mt + mt->offsets[0];
    paletteRGBA_t* rgba  = malloc((size_t)w * (size_t)h * 4);
    if (!rgba) { ri.FS_FreeFile(mt); return NULL; }

    for (int i = 0; i < w * h; i++) {
        const byte idx = indices[i];
        rgba[i].r = mt->palette[idx].r;
        rgba[i].g = mt->palette[idx].g;
        rgba[i].b = mt->palette[idx].b;
        // Quake/H2 paletted convention: index 255 is the transparent color
        // (used by alpha-textured sprites/decals like vines, plant cards).
        // Mark it fully transparent so the alpha-blend/alpha-test path can cut
        // it out instead of drawing an opaque black/colored quad.
        rgba[i].a = (idx == 255) ? 0 : 255;
    }

    // Bake gamma/brightness/contrast into the albedo, matching ref_gl1
    // (GrabPalette runs the palette through the same LUT at load).
    ApplyGammaRGBA(rgba, (size_t)w * (size_t)h);

    image_t* slot = AllocImageSlot();
    if (!slot) { free(rgba); ri.FS_FreeFile(mt); return NULL; }

    strcpy_s(slot->name, sizeof(slot->name), name);
    if (!VK_CreateTextureRGBA((uint32_t)w, (uint32_t)h, rgba, &slot->tex)) {
        slot->used = false;
        free(rgba);
        ri.FS_FreeFile(mt);
        return NULL;
    }
    slot->descriptor = VK_AllocDescriptorSetFor(slot->tex.view);
    slot->width  = w;
    slot->height = h;
    // GL leaves .m8 images has_alpha = false (only .m32 are marked has_alpha).
    // We still bake index-255 -> alpha 0 above so that sprites/surfaces which DO
    // opt into alpha testing (via their own flags) can cut it out; but flex
    // models keyed off has_alpha will treat an .m8 skin as opaque, like GL.
    slot->has_alpha = false;

    free(rgba);
    ri.FS_FreeFile(mt);
    return slot;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

image_t* VK_FindImage(const char* name)
{
    if (!name || !*name) return NULL;
    image_t* hit = FindByName(name);
    if (hit) return hit;

    // Pick loader by extension.
    const size_t len = strlen(name);
    if (len >= 4 && strcasecmp(name + len - 4, ".m32") == 0)
        return LoadM32(name);
    if (len >= 3 && strcasecmp(name + len - 3, ".m8") == 0)
        return LoadM8(name);

    // Try .m32 first, then .m8.
    char with_ext[512];
    snprintf(with_ext, sizeof(with_ext), "%s.m32", name);
    image_t* r = LoadM32(with_ext);
    if (r) return r;
    snprintf(with_ext, sizeof(with_ext), "%s.m8", name);
    return LoadM8(with_ext);
}

image_t* VK_FindPic(const char* name)
{
    // H2 names like "menu/banner.m32" with no path prefix are interpreted
    // relative to "pics/". A leading slash strips the implicit prefix.
    if (!name || !*name) return NULL;

    if (name[0] == '/' || name[0] == '\\')
        return VK_FindImage(name + 1);

    char full[512];
    snprintf(full, sizeof(full), "pics/%s", name);
    return VK_FindImage(full);
}

qboolean VK_InitImages(void)
{
    VK_InitGammaTable();   // register vid_gamma/brightness/contrast + build the LUT
                           // so the very first textures are loaded already gamma'd
    if (!CreateWhiteTexture()) return false;
    return true;
}

void VK_ShutdownImages(void)
{
    for (int i = 0; i < texture_count; i++) {
        if (!textures[i].used) continue;
        VK_DestroyTexture(&textures[i].tex);
        textures[i].used = false;
    }
    texture_count = 0;
    white_texture = NULL;
}

// Mark an image permanent so VK_FreeUnusedImages never evicts it. Used for
// assets loaded once and needed across all maps: conchars, big-font atlases,
// the reflection sphere-map, particle textures, the white fallback.
void VK_Image_MarkPermanent(image_t* img)
{
    if (img) img->permanent = true;
}

// Evict images not touched during the current registration sequence (i.e.
// loaded for a level we've since left). Mirrors ref_gl1 R_FreeUnusedImages.
// MUST be called only when the GPU is idle (we call it at level load behind
// vkDeviceWaitIdle), since it frees textures + descriptor sets that prior
// frames referenced. Permanent images are always kept.
void VK_FreeUnusedImages(void)
{
    int freed = 0;
    for (int i = 0; i < texture_count; i++) {
        image_t* img = &textures[i];
        if (!img->used) continue;
        if (img->permanent) continue;
        if (img->reg_seq == s_image_reg_seq) continue;   // used this level: keep
        if (img == white_texture) continue;              // belt-and-braces

        if (img->descriptor != VK_NULL_HANDLE) {
            VK_FreeDescriptorSet(img->descriptor);
            img->descriptor = VK_NULL_HANDLE;
        }
        if (img->world_descriptor != VK_NULL_HANDLE) {
            VK_FreeWorldDescriptor(img->world_descriptor);
            img->world_descriptor = VK_NULL_HANDLE;
        }
        VK_DestroyTexture(&img->tex);
        img->used = false;
        freed++;
    }
    // Compact the high-water mark so AllocImageSlot can reuse freed tail slots.
    while (texture_count > 0 && !textures[texture_count - 1].used)
        texture_count--;
    if (freed)
        ri.Con_Printf(PRINT_ALL, "vk: freed %d unused textures (level change)\n", freed);
}

int             VK_ImageWidth     (const image_t* img) { return img ? img->width      : 0; }
int             VK_ImageHeight    (const image_t* img) { return img ? img->height     : 0; }
VkDescriptorSet VK_ImageDescriptor(const image_t* img) { return img ? img->descriptor : VK_NULL_HANDLE; }

// World (3D) descriptor for an image, used by model skins, sprite frames, and
// the sky. Allocated lazily on first use and stored ON THE IMAGE so it shares
// the image's lifetime: VK_FreeUnusedImages frees it alongside the texture, and
// AllocImageSlot zeroes it on slot reuse. This makes 3D texture binding
// eviction-safe the same way the 2D descriptor is - a freed image returns
// VK_NULL_HANDLE (caller skips the draw, as ref_gl1 effectively does when its
// texnum is gone) rather than leaving a cached descriptor pointing at a
// destroyed image view (imageView 0x0 -> device-lost). Returns NULL for a freed
// or not-yet-uploaded image.
VkDescriptorSet VK_ImageWorldDescriptor(image_t* img)
{
    if (!img || !img->used || img->tex.view == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;
    if (img->world_descriptor == VK_NULL_HANDLE)
        img->world_descriptor = VK_AllocWorldDescriptor(img->tex.view);
    return img->world_descriptor;
}
qboolean VK_ImageHasAlpha(const image_t* img) { return img ? img->has_alpha : false; }
VkImageView     VK_ImageView      (const image_t* img) { return img ? img->tex.view   : VK_NULL_HANDLE; }
