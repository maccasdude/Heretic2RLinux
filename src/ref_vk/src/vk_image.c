//
// vk_image.c - Texture cache + .m32/.m8 loaders.
//

#include "vk_local.h"
#include "vk_buffer.h"
#include "vk_pipeline.h"
#include "vk_image.h"

#include "qcommon/qfiles.h"   // miptex_t, miptex32_t

#include <string.h>
#include <stdlib.h>

// We define our own image_s. The engine treats it as opaque - it just gets a
// pointer back from RegisterPic / RegisterSkin and hands it to draw calls.
// vk_image_t* is what the engine's `struct image_s*` actually points to.
typedef struct image_s {
    char            name[256];
    vk_texture_t    tex;
    VkDescriptorSet descriptor;
    int             width, height;
    qboolean        used;
} image_t;

// Strictly an array of slots. RegisterPic returns &textures[i].
static image_t   textures[VK_MAX_TEXTURES];
static int       texture_count = 0;

// 1x1 fully-white texture so DrawFill / DrawChar can render solid coloured
// quads through the same textured pipeline.
static image_t*  white_texture = NULL;

// ---------------------------------------------------------------------------
// Texture slot allocation
// ---------------------------------------------------------------------------

static image_t* AllocImageSlot(void)
{
    if (texture_count >= VK_MAX_TEXTURES) {
        ri.Con_Printf(PRINT_ALL, "vk: texture pool exhausted\n");
        return NULL;
    }
    image_t* slot = &textures[texture_count++];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    return slot;
}

static image_t* FindByName(const char* name)
{
    for (int i = 0; i < texture_count; i++) {
        if (textures[i].used && strcasecmp(textures[i].name, name) == 0)
            return &textures[i];
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

    if (!VK_CreateTextureRGBA((uint32_t)w, (uint32_t)h, pixels, &slot->tex)) {
        slot->used = false;
        ri.FS_FreeFile(mt);
        return NULL;
    }
    slot->descriptor = VK_AllocDescriptorSetFor(slot->tex.view);
    slot->width  = w;
    slot->height = h;

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

int             VK_ImageWidth     (const image_t* img) { return img ? img->width      : 0; }
int             VK_ImageHeight    (const image_t* img) { return img ? img->height     : 0; }
VkDescriptorSet VK_ImageDescriptor(const image_t* img) { return img ? img->descriptor : VK_NULL_HANDLE; }
VkImageView     VK_ImageView      (const image_t* img) { return img ? img->tex.view   : VK_NULL_HANDLE; }
