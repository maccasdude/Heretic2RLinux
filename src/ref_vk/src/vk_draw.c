//
// vk_draw.c - 2D drawing primitives + batching.
//
// We accumulate textured quads into a single vertex buffer per frame. When
// the texture (descriptor set) changes between consecutive quads, we flush
// the current batch and start a new one. Since menu UI mostly hits one big
// texture per item, this gives us roughly 1 draw call per menu item.
//

#include "vk_local.h"
#include "vk_buffer.h"
#include "vk_pipeline.h"
#include "vk_image.h"
#include "vk_draw.h"

#include <string.h>
#include <stdlib.h>

// Pre-allocated streaming vertex buffer. Each renderer frame uses one slice;
// the slices are sized to handle a worst-case menu+console+HUD frame. The H2
// loading screen (full book/scroll background + lots of text at high res) can
// exceed 8192 quads, so size generously - at 24 B/vertex this is ~2.4 MB per
// in-flight frame slice.
#define VK_QUADS_PER_FRAME   16384
#define VK_VERTS_PER_QUAD    6                    // two triangles per quad
#define VK_VERTS_PER_FRAME   (VK_QUADS_PER_FRAME * VK_VERTS_PER_QUAD)

typedef struct {
    vk_buffer_t   buf;          // persistently mapped, HOST_VISIBLE | HOST_COHERENT
    vk_vertex2d_t* mapped;      // raw pointer to mapped buffer
    uint32_t      cursor;       // next vertex slot to write
} frame_vbo_t;

// One vertex buffer per in-flight frame. Indexed by vk_state.current_frame.
static frame_vbo_t   s_frame_vbos[MAX_FRAMES_IN_FLIGHT] = {0};

// Current batch state. A batch is a run of quads with the same descriptor.
static VkDescriptorSet s_batch_descriptor = VK_NULL_HANDLE;
static uint32_t        s_batch_start      = 0;     // first vertex in the batch

// Forward decl: implementation is down with VK_Draw_BeginFrame.
static void EnsureBound2D(void);

// References used so often the engine keeps them as cvars - we hold our own
// pointers so we don't have to look up by name on every draw.
static image_t*  draw_chars  = NULL;   // "misc/conchars.m32" - 16x16 grid of 8x8 glyphs

// ---------------------------------------------------------------------------
// Batching
// ---------------------------------------------------------------------------

static void FlushBatch(void)
{
    const uint32_t frame  = vk_state.current_frame;
    frame_vbo_t* fv       = &s_frame_vbos[frame];
    VkCommandBuffer cb    = vk_state.command_buffers[frame];

    const uint32_t count = fv->cursor - s_batch_start;
    if (count == 0 || s_batch_descriptor == VK_NULL_HANDLE) {
        s_batch_start      = fv->cursor;
        return;
    }

    // Bind the descriptor and draw.
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk_pipeline_2d.layout, 0, 1, &s_batch_descriptor,
                            0, NULL);
    vkCmdDraw(cb, count, 1, s_batch_start, 0);

    s_batch_start = fv->cursor;
}

static qboolean BeginQuad(VkDescriptorSet desc)
{
    if (desc == VK_NULL_HANDLE) return false;
    if (!vk_state.frame_started) return false;     // outside the renderpass

    EnsureBound2D();

    const uint32_t frame = vk_state.current_frame;
    frame_vbo_t* fv      = &s_frame_vbos[frame];

    if (fv->cursor + VK_VERTS_PER_QUAD > VK_VERTS_PER_FRAME) {
        // Out of vertex space for this frame. Drop further quads silently.
        return false;
    }
    if (desc != s_batch_descriptor) {
        FlushBatch();
        s_batch_descriptor = desc;
    }
    return true;
}

static inline void WriteVert(uint32_t* cursor, vk_vertex2d_t* base,
                             float x, float y, float u, float v,
                             paletteRGBA_t c)
{
    vk_vertex2d_t* dst = &base[(*cursor)++];
    dst->x = x; dst->y = y;
    dst->u = u; dst->v = v;
    dst->r = c.r; dst->g = c.g; dst->b = c.b; dst->a = c.a;
}

// Emit two triangles (= 6 verts) for a textured quad.
static void EmitQuad(float x, float y, float w, float h,
                     float u0, float v0, float u1, float v1,
                     paletteRGBA_t color)
{
    const uint32_t frame = vk_state.current_frame;
    frame_vbo_t* fv      = &s_frame_vbos[frame];
    vk_vertex2d_t* base  = fv->mapped;

    // Defensive: refuse to write if the VBO isn't mapped or we'd overflow.
    if (!base) {
        static int warned = 0;
        if (!warned++)
            ri.Con_Printf(PRINT_ALL, "vk: EmitQuad NULL mapped (frame %u, cursor %u)\n", frame, fv->cursor);
        return;
    }
    if (fv->cursor + VK_VERTS_PER_QUAD > VK_VERTS_PER_FRAME) {
        static int warned = 0;
        if (!warned++)
            ri.Con_Printf(PRINT_ALL, "vk: EmitQuad cursor overflow (cursor %u max %u)\n",
                          fv->cursor, (unsigned)VK_VERTS_PER_FRAME);
        return;
    }

    WriteVert(&fv->cursor, base, x,     y,     u0, v0, color);
    WriteVert(&fv->cursor, base, x + w, y,     u1, v0, color);
    WriteVert(&fv->cursor, base, x + w, y + h, u1, v1, color);

    WriteVert(&fv->cursor, base, x,     y,     u0, v0, color);
    WriteVert(&fv->cursor, base, x + w, y + h, u1, v1, color);
    WriteVert(&fv->cursor, base, x,     y + h, u0, v1, color);
}

// ---------------------------------------------------------------------------
// Public draw primitives
// ---------------------------------------------------------------------------

void VK_Draw_GetPicSize(int* w, int* h, const char* name)
{
    const image_t* img = VK_FindPic(name);
    if (w) *w = img ? VK_ImageWidth (img) : 0;
    if (h) *h = img ? VK_ImageHeight(img) : 0;
}

void VK_Draw_Pic(int x, int y, int scale, const char* name, float alpha)
{
    image_t* img = VK_FindPic(name);
    if (!img) return;

    paletteRGBA_t color = { 255, 255, 255, (byte)(alpha * 255.0f) };
    const int w = VK_ImageWidth(img)  * (scale ? scale : 1);
    const int h = VK_ImageHeight(img) * (scale ? scale : 1);

    if (!BeginQuad(VK_ImageDescriptor(img))) return;
    EmitQuad((float)x, (float)y, (float)w, (float)h, 0.0f, 0.0f, 1.0f, 1.0f, color);
}

void VK_Draw_StretchPic(int x, int y, int w, int h, const char* name, float alpha,
                        DrawStretchPicScaleMode_t mode)
{
    image_t* img = VK_FindPic(name);
    if (!img) return;

    // Apply H2's virtual-coordinate scaling. Source coords are in a 640x480
    // virtual space (DEF_WIDTH x DEF_HEIGHT). Matches gl1_Draw.c Draw_StretchPic.
    const int vw = (int)vk_state.swapchain_extent.width;
    const int vh = (int)vk_state.swapchain_extent.height;
    const int DEF_W = 640, DEF_H = 480;

    if (mode == DSP_SCALE_SCREEN) {
        const int xr = x + w;
        const int yb = y + h;
        x = vw * x / DEF_W;
        y = vh * y / DEF_H;
        w = vw * xr / DEF_W - x;
        h = vh * yb / DEF_H - y;
    } else if (mode == DSP_SCALE_4x3) {
        const int xr = x + w;
        const int yb = y + h;
        const int screen_width = vh * 4 / 3;
        const int screen_offset_x = (vw - screen_width) / 2;
        x = (screen_width * x / DEF_W) + screen_offset_x;
        y = vh * y / DEF_H;
        w = (screen_width * xr / DEF_W - x) + screen_offset_x;
        h = vh * yb / DEF_H - y;
    }
    // DSP_NONE: use coords as-is.

    paletteRGBA_t color = { 255, 255, 255, (byte)(alpha * 255.0f) };
    if (!BeginQuad(VK_ImageDescriptor(img))) return;
    EmitQuad((float)x, (float)y, (float)w, (float)h, 0.0f, 0.0f, 1.0f, 1.0f, color);
}

void VK_Draw_TileClear(int x, int y, int w, int h, const char* name)
{
    image_t* img = VK_FindPic(name);
    if (!img) return;
    // Tile the texture: UVs come from pixel coords / 64 (matches the GL1
    // implementation's choice of repeat unit).
    const float u0 = (float)x / 128.0f, v0 = (float)y / 128.0f;
    const float u1 = (float)(x + w) / 128.0f, v1 = (float)(y + h) / 128.0f;
    paletteRGBA_t color = { 255, 255, 255, 255 };
    if (!BeginQuad(VK_ImageDescriptor(img))) return;
    EmitQuad((float)x, (float)y, (float)w, (float)h, u0, v0, u1, v1, color);
}

void VK_Draw_Fill(int x, int y, int w, int h, paletteRGBA_t color)
{
    image_t* white = VK_GetWhiteTexture();
    if (!white) return;
    if (!BeginQuad(VK_ImageDescriptor(white))) return;
    EmitQuad((float)x, (float)y, (float)w, (float)h, 0.0f, 0.0f, 1.0f, 1.0f, color);
}

void VK_Draw_FadeScreen(paletteRGBA_t color)
{
    // Full-screen dim/fade: a single quad covering the whole viewport.
    VK_Draw_Fill(0, 0, (int)vk_state.swapchain_extent.width,
                       (int)vk_state.swapchain_extent.height, color);
}

void VK_Draw_Char(int x, int y, int scale, int c, paletteRGBA_t color, qboolean shadow)
{
    if (!draw_chars) {
        draw_chars = VK_FindPic("misc/conchars.m32");
        if (!draw_chars) draw_chars = VK_FindPic("misc/conchars");
        if (!draw_chars) return;
    }

    c &= 255;
    if ((c & 127) == 32) return;     // space: no glyph

    const int char_size = 8 * (scale ? scale : 1);

    // The conchars texture is a 16x16 grid; each cell is 1/16 in UV space.
    const float cell = 1.0f / 16.0f;
    const float u0 = (float)(c & 15) * cell;
    const float v0 = (float)(c >> 4) * cell;
    const float u1 = u0 + cell;
    const float v1 = v0 + cell;

    if (!BeginQuad(VK_ImageDescriptor(draw_chars))) return;

    if (shadow) {
        const paletteRGBA_t shade = { 32, 32, 32, (byte)((color.a * 192) / 255) };
        EmitQuad((float)(x + scale), (float)(y + scale),
                 (float)char_size, (float)char_size,
                 u0, v0, u1, v1, shade);
        // Re-bind not necessary since same descriptor.
    }

    EmitQuad((float)x, (float)y, (float)char_size, (float)char_size,
             u0, v0, u1, v1, color);
}

// ---------------------------------------------------------------------------
// Big-font (banner/menu) rendering
// ---------------------------------------------------------------------------
//
// H2 uses two atlas fonts for menu banners and item labels:
//   font1.m32 / font1.fnt  - regular alphabet  (escape code 0x02 selects)
//   font2.m32 / font2.fnt  - decorative subset (escape code 0x03 selects)
//
// .fnt is a packed array of 224 glyph definitions matching glxy_t below
// (each 28 bytes). Index 0 is ASCII 32 (space), entry 95 is ASCII 127, and
// so on through 255.
//
// IMPORTANT: all assets are loaded at VK_DrawInit time, NOT lazily on first
// use. Earlier attempts to lazy-load triggered Vulkan texture-creation calls
// during BF_Strlen, which the menu code calls during layout. That ran a
// one-shot command buffer + queueWaitIdle in the middle of an active frame
// command buffer recording, which is technically legal but broke menu text
// somehow (possibly a driver bug, possibly a hidden invariant the engine
// expects from BF_Strlen). Eager loading sidesteps the whole class of bug.

typedef struct {
    float xl, yt, xr, yb;   // UV bounds in the matching atlas
    int   w, h;             // glyph pixel size at 640x480
    int   baseline;         // y offset to apply when drawing
} bf_glyph_t;

#define BF_NUM_GLYPHS 224   // ASCII 32..255

static bf_glyph_t* bf_font1 = NULL;     // file buffer; freed via FS_FreeFile in shutdown
static bf_glyph_t* bf_font2 = NULL;
static image_t*    bf_atlas1 = NULL;
static image_t*    bf_atlas2 = NULL;
static qboolean    bf_ready  = false;

static qboolean BF_Init(void)
{
    if (bf_ready) return true;

    int len1 = ri.FS_LoadFile("pics/misc/font1.fnt", (void**)&bf_font1);
    int len2 = ri.FS_LoadFile("pics/misc/font2.fnt", (void**)&bf_font2);

    bf_atlas1 = VK_FindPic("misc/font1.m32");
    bf_atlas2 = VK_FindPic("misc/font2.m32");

    if (!bf_font1 || !bf_font2 || !bf_atlas1 || !bf_atlas2) {
        ri.Con_Printf(PRINT_ALL,
            "vk: big-font init failed (font1=%p[%d] font2=%p[%d] atlas1=%p atlas2=%p)\n",
            bf_font1, len1, bf_font2, len2, bf_atlas1, bf_atlas2);
        // Don't error - the placeholder code path will still render via conchars.
        return false;
    }

    ri.Con_Printf(PRINT_ALL, "vk: big-font ready (font1=%dB font2=%dB)\n", len1, len2);
    bf_ready = true;
    return true;
}

static const bf_glyph_t* BF_GetGlyph(byte c, const bf_glyph_t* font)
{
    if (!font || c < 32) return NULL;
    const bf_glyph_t* g = &font[c - 32];
    if (g->w == 0) g = &font[14];     // GL1 falls back to '.' (= '32+14' = ASCII 46)
    return g;
}

void VK_Draw_BigFont(int x, int y, const char* text, float alpha)
{
    if (!text) return;

    // Placeholder fallback if the real font assets failed to load.
    if (!bf_ready) {
        paletteRGBA_t color = { 255, 255, 255, (byte)(alpha * 255.0f) };
        int cx = x;
        for (const char* p = text; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == 0x02 || c == 0x03) continue;
            VK_Draw_Char(cx, y, 2, c, color, false);
            cx += 16;
        }
        return;
    }

    paletteRGBA_t color = { 255, 255, 255, (byte)(alpha * 255.0f) };

    int vid_w = (int)vk_state.swapchain_extent.width;
    int vid_h = (int)vk_state.swapchain_extent.height;
    int offset_x = 0;

    if ((float)vid_w * 0.75f > (float)vid_h) {
        const int new_w = vid_h * 4 / 3;
        offset_x = (vid_w - new_w) / 2;
        vid_w = new_w;
    }

    const bf_glyph_t* cur_font  = bf_font1;
    image_t*          cur_atlas = bf_atlas1;
    int ox = x;
    int oy = y;

    while (*text) {
        const byte c = (byte)*text++;
        switch (c) {
            case 0: return;
            case 1:
                // Centring escape (rarely used; just keep position).
                break;
            case 2: cur_font = bf_font1; cur_atlas = bf_atlas1; break;
            case 3: cur_font = bf_font2; cur_atlas = bf_atlas2; break;
            case '\t': ox += 63; break;
            case '\n': oy += 18; ox = x; break;
            case '\r': break;
            case 32:   ox += 8;  break;
            default:
                if (c > 32) {
                    const bf_glyph_t* g = BF_GetGlyph(c, cur_font);
                    if (!g) break;
                    const int gy = oy - g->baseline;
                    const int xl = (vid_w *  ox          ) / 640 + offset_x;
                    const int xr = (vid_w * (ox  + g->w) ) / 640 + offset_x;
                    const int yt = (vid_h *  gy          ) / 480;
                    const int yb = (vid_h * (gy  + g->h) ) / 480;

                    if (BeginQuad(VK_ImageDescriptor(cur_atlas))) {
                        EmitQuad((float)xl, (float)yt,
                                 (float)(xr - xl), (float)(yb - yt),
                                 g->xl, g->yt, g->xr, g->yb, color);
                    }
                    ox += g->w;
                }
                break;
        }
    }
}

int VK_BF_Strlen(const char* text)
{
    if (!text) return 0;

    // Placeholder fallback matches the placeholder DrawBigFont above.
    if (!bf_ready) {
        int n = 0;
        for (const char* p = text; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == 0x02 || c == 0x03) continue;
            n += 16;
        }
        return n;
    }

    int width = 0;
    const bf_glyph_t* cur_font = bf_font1;

    while (*text) {
        const byte c = (byte)*text++;
        switch (c) {
            case 0:
            case 1:
            case '\t':
            case '\n':
                return width;
            case 2: cur_font = bf_font1; break;
            case 3: cur_font = bf_font2; break;
            case '\r': break;
            case 32:   width += 8; break;
            default:
                if (c > 32) {
                    const bf_glyph_t* g = BF_GetGlyph(c, cur_font);
                    if (g) width += g->w;
                }
                break;
        }
    }
    return width;
}

// ---------------------------------------------------------------------------
// Books (composite menu backgrounds)
// ---------------------------------------------------------------------------
//
// H2 menus use .bk files - composite images split into segments to fit
// within texture-size limits. A .bk is a binary header + array of segments,
// each segment naming its texture and its position/size in the book's
// virtual canvas. We load+cache them by path, then render as multiple
// textured quads scaled to the screen with 4:3 letterboxing.

#include "qcommon/qfiles.h"     // book_t, bookheader_t, bookframe_t, BOOK_VERSION, IDBOOKHEADER

#define VK_MAX_CACHED_BOOKS 16

typedef struct {
    char     name[128];
    int      total_w, total_h;
    int      num_segments;
    bookframe_t segments[MAX_FRAMES];   // x, y, w, h, name
    image_t* skins[MAX_FRAMES];         // resolved textures per segment
    qboolean used;
} vk_book_t;

static vk_book_t s_book_cache[VK_MAX_CACHED_BOOKS] = {0};

static vk_book_t* Book_FindCached(const char* name)
{
    for (int i = 0; i < VK_MAX_CACHED_BOOKS; i++)
        if (s_book_cache[i].used && strcasecmp(s_book_cache[i].name, name) == 0)
            return &s_book_cache[i];
    return NULL;
}

static vk_book_t* Book_AllocSlot(void)
{
    for (int i = 0; i < VK_MAX_CACHED_BOOKS; i++)
        if (!s_book_cache[i].used) return &s_book_cache[i];
    return NULL;
}

static vk_book_t* Book_Load(const char* path)
{
    vk_book_t* hit = Book_FindCached(path);
    if (hit) return hit;

    book_t* book_in = NULL;
    const int len = ri.FS_LoadFile(path, (void**)&book_in);
    if (book_in == NULL || len <= 0) {
        ri.Con_Printf(PRINT_ALL, "vk: book '%s' not found\n", path);
        return NULL;
    }
    if (book_in->bheader.ident != IDBOOKHEADER) {
        ri.Con_Printf(PRINT_ALL, "vk: book '%s' wrong magic\n", path);
        ri.FS_FreeFile(book_in);
        return NULL;
    }
    if (book_in->bheader.version != BOOK_VERSION) {
        ri.Con_Printf(PRINT_ALL, "vk: book '%s' wrong version (%d)\n", path, book_in->bheader.version);
        ri.FS_FreeFile(book_in);
        return NULL;
    }
    if (book_in->bheader.num_segments <= 0 || book_in->bheader.num_segments > MAX_FRAMES) {
        ri.Con_Printf(PRINT_ALL, "vk: book '%s' bad segment count (%d)\n", path, book_in->bheader.num_segments);
        ri.FS_FreeFile(book_in);
        return NULL;
    }

    vk_book_t* b = Book_AllocSlot();
    if (!b) { ri.FS_FreeFile(book_in); return NULL; }

    strcpy_s(b->name, sizeof(b->name), path);
    b->total_w     = book_in->bheader.total_w;
    b->total_h     = book_in->bheader.total_h;
    b->num_segments = book_in->bheader.num_segments;

    for (int i = 0; i < b->num_segments; i++) {
        b->segments[i] = book_in->bframes[i];

        // Segment textures live under "Book/<name>". The H2R conventions put
        // the extension inside frame->name; if not, VK_FindImage will try
        // .m32 then .m8.
        char full[256];
        snprintf(full, sizeof(full), "Book/%s", b->segments[i].name);
        b->skins[i] = VK_FindImage(full);
    }
    b->used = true;
    ri.FS_FreeFile(book_in);
    return b;
}

void VK_BookDrawPic(const char* name, float scale, float alpha)
{
    if (!name || scale < 0.001f) return;
    vk_book_t* book = Book_Load(name);
    if (!book) return;

    const float header_w = (float)book->total_w;
    const float header_h = (float)book->total_h;
    if (header_w < 1.0f || header_h < 1.0f) return;

    float vid_w = (float)vk_state.swapchain_extent.width;
    float vid_h = (float)vk_state.swapchain_extent.height;

    int offset_x = 0;
    int offset_y = 0;

    // 4:3 letterboxing on widescreen.
    if (vid_w * 0.75f > vid_h) {
        const float new_vid_w = vid_h * 4.0f / 3.0f;
        offset_x = (int)((vid_w - new_vid_w) / 2.0f);
        vid_w   = new_vid_w;
    }

    // Centre the scaled book in its target rect (matches GL1).
    offset_x += (int)((header_w - header_w * scale) * 0.5f * (vid_w / header_w));
    offset_y += (int)((header_h - header_h * scale) * 0.5f * (vid_h / header_h));

    paletteRGBA_t color = { 255, 255, 255, (byte)(alpha * 255.0f) };

    for (int i = 0; i < book->num_segments; i++) {
        const bookframe_t* bf = &book->segments[i];
        image_t* img = book->skins[i];
        if (!img) continue;

        const int pic_x = (int)((float)bf->x * vid_w / header_w * scale);
        const int pic_y = (int)((float)bf->y * vid_h / header_h * scale);
        const int pic_w = (int)((float)bf->w * vid_w / header_w * scale + 0.999f);
        const int pic_h = (int)((float)bf->h * vid_h / header_h * scale + 0.999f);

        if (!BeginQuad(VK_ImageDescriptor(img))) continue;
        EmitQuad((float)(offset_x + pic_x), (float)(offset_y + pic_y),
                 (float)pic_w, (float)pic_h,
                 0.0f, 0.0f, 1.0f, 1.0f, color);
    }
}

// Used by cinematic blitting where there's no image_t* by name.
void VK_DrawDirectQuad(VkDescriptorSet desc, float x, float y, float w, float h,
                       float u0, float v0, float u1, float v1,
                       paletteRGBA_t color)
{
    if (!BeginQuad(desc)) return;
    EmitQuad(x, y, w, h, u0, v0, u1, v1, color);
}

// ---------------------------------------------------------------------------
// Per-frame lifecycle
// ---------------------------------------------------------------------------

qboolean VK_DrawInit(void)
{
    // Allocate per-frame vertex buffers and map them persistently.
    const VkDeviceSize sz = sizeof(vk_vertex2d_t) * VK_VERTS_PER_FRAME;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (!VK_CreateBuffer(sz,
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &s_frame_vbos[i].buf))
            return false;
        if (!VK_MapBuffer(&s_frame_vbos[i].buf))
            return false;
        s_frame_vbos[i].mapped = (vk_vertex2d_t*)s_frame_vbos[i].buf.mapped;
        s_frame_vbos[i].cursor = 0;
    }

    // Preload menu/HUD assets so they don't get created mid-frame.
    draw_chars = VK_FindPic("misc/conchars.m32");
    if (!draw_chars)
        ri.Con_Printf(PRINT_ALL, "vk: WARN couldn't preload conchars\n");
    BF_Init();

    // Preload the common menu background book. Loading textures mid-frame
    // (via the lazy path in BookDrawPic) issues one-shot command buffers and
    // a queueWaitIdle, which is unsafe once the 3D world pass has recorded
    // draws into the active frame command buffer. Doing it here keeps all
    // texture creation outside of any frame.
    Book_Load("book/back/b_conback8.bk");

    return true;
}

void VK_DrawShutdown(void)
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VK_DestroyBuffer(&s_frame_vbos[i].buf);
        memset(&s_frame_vbos[i], 0, sizeof(s_frame_vbos[i]));
    }
    if (bf_font1) { ri.FS_FreeFile(bf_font1); bf_font1 = NULL; }
    if (bf_font2) { ri.FS_FreeFile(bf_font2); bf_font2 = NULL; }
    bf_atlas1 = NULL;
    bf_atlas2 = NULL;
    bf_ready  = false;
    draw_chars = NULL;
    // Clear book cache; the textures themselves get freed via VK_ShutdownImages.
    memset(s_book_cache, 0, sizeof(s_book_cache));
}

void VK_Draw_BeginFrame(void)
{
    const uint32_t frame = vk_state.current_frame;
    s_frame_vbos[frame].cursor = 0;
    s_batch_descriptor         = VK_NULL_HANDLE;
    s_batch_start              = 0;

    VkCommandBuffer cb = vk_state.command_buffers[frame];

    // Bind pipeline and per-frame vertex buffer once.
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_2d.pipeline);

    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &s_frame_vbos[frame].buf.buffer, &zero);

    // Dynamic viewport + scissor every frame so we adapt to window resizes.
    VkViewport vp = {0};
    vp.x        = 0.0f;
    vp.y        = 0.0f;
    vp.width    = (float)vk_state.swapchain_extent.width;
    vp.height   = (float)vk_state.swapchain_extent.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    VkRect2D sc = {0};
    sc.extent = vk_state.swapchain_extent;
    vkCmdSetScissor(cb, 0, 1, &sc);

    // Push viewport size for the vertex shader's pixel->NDC conversion.
    float pc[2] = {
        (float)vk_state.swapchain_extent.width,
        (float)vk_state.swapchain_extent.height
    };
    vkCmdPushConstants(cb, vk_pipeline_2d.layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(pc), pc);

    vk_state.pipeline_2d_bound = true;
}

// Called by 2D draw paths after the 3D pass has clobbered the bound pipeline.
// Returns the (still active) frame command buffer so callers don't have to
// fetch it again.
static void EnsureBound2D(void)
{
    if (vk_state.pipeline_2d_bound) return;

    const uint32_t frame = vk_state.current_frame;
    VkCommandBuffer cb = vk_state.command_buffers[frame];

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_2d.pipeline);

    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &s_frame_vbos[frame].buf.buffer, &zero);

    VkViewport vp = {0};
    vp.width    = (float)vk_state.swapchain_extent.width;
    vp.height   = (float)vk_state.swapchain_extent.height;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    VkRect2D sc = {0};
    sc.extent = vk_state.swapchain_extent;
    vkCmdSetScissor(cb, 0, 1, &sc);

    float pc[2] = {
        (float)vk_state.swapchain_extent.width,
        (float)vk_state.swapchain_extent.height
    };
    vkCmdPushConstants(cb, vk_pipeline_2d.layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(pc), pc);

    vk_state.pipeline_2d_bound = true;
    // The 3D pass would have left a stale batch start; reset.
    s_batch_descriptor = VK_NULL_HANDLE;
    s_batch_start      = s_frame_vbos[frame].cursor;
}

void VK_Draw_EndFrame(void)
{
    FlushBatch();
}
