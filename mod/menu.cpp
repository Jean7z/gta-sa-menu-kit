/* SA Menu Kit - engine (Route B: symbol-only widget-pool injection).
   Resolves every game address by name via IAML; zero hardcoded offsets.
   Widget objects live in the game's CTouchInterface::m_pWidgets pool, so the
   game itself updates/draws/touches them like native buttons. */
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/menu-api.h>
#include <stdint.h>
#include <new>
#include <cstring>
#include <map>
#include <string>

/* stb_image: decodifica el PNG del icono custom del boton. La macro
   STB_IMAGE_IMPLEMENTATION debe definirse exactamente aqui (unica TU). */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* Plan B: fuente bitmap 5x7 embebida (dominio publico, layout Adafruit GFX).
   El texto NO usa CFont (experimento Tarea 0: CFont no compone en este fork y
   el flush rompe el HUD). En su lugar se rasteriza aqui en un buffer RGBA y se
   sube por el MISMO pipeline de iconos (RwImage -> raster 0x10 -> clean textura),
   horneado en la textura que Draw pinta -> composicion garantizada. */
#include "font5x7.h"

#define MAX_POOL_SLOTS 190              /* game's widget pool size (MAX_WIDGETS_GAME) */
#define OUR_WIDGET_LIMIT 16
#define MENU_STACK_MAX 8                /* nested OpenMenu depth (root group 0 is implicit at depth 0) */
#define WIDGET_ALLOC_SIZE 0x200         /* ponytail: unknown CWidgetButton size; over-alloc
                                           (ctor inits the real layout; game never reads beyond it) */

/* Mirror of game's WidgetPosition (psdk) - 4 floats, aggregate. */
struct WidgetPosition { float m_fOriginX, m_fOriginY, m_fScaleX, m_fScaleY; };

/* Resolved game symbols */
static void**               s_pWidgetsPool    = NULL;   /* CTouchInterface::m_pWidgets */
static void*                s_pfnCtorButton   = NULL;   /* CWidgetButton::CWidgetButton(...) */
static bool               (*s_pfnIsReleased)(int widgetId, void* unused, int frames) = NULL;
static bool               (*s_pfnIsTouched )(int widgetId, void* unused, int frames) = NULL; /* level while finger down; release = falling edge */

/* RenderWare symbols (exported by libGTASA.so) for custom button icons.
   RwImage layout verified by disasm (RwImageCreate @0x2709d8): flags@+0,
   width@+4, height@+8, depth@+12, free@+16, cpPixels@+24. RwImageDestroy frees
   cpPixels ONLY if flags bit0 is set — we never set it, so the stbi buffer is
   never touched by the engine. */
/* RwImage layout in this fork (disasm: RwImageCreate 0x2709d8,
   _rwRasterSetFromImage8888 0x23b264): flags@0x00, width@0x04, height@0x08,
   depth@0x0c, stride@0x10, cpPixels@0x18. RwImageCreate leaves stride
   UNINITIALIZED — the copy loop advances rows with `add x10, x10, x11`
   (image stride), so callers must set it (width*4 for RGBA8) or the
   set-from-image reads garbage rows (visible as a graphical mess). */
struct RwImageLayout {
    uint32_t flags;    /* +0x00 */
    uint32_t width;    /* +0x04 */
    uint32_t height;   /* +0x08 */
    uint32_t depth;    /* +0x0c */
    uint32_t stride;   /* +0x10 — MUST be set before RwRasterSetFromImage */
    uint32_t pad14;    /* +0x14 — pointer alignment on arm64, not a field */
    uint8_t* cpPixels; /* +0x18 */
    void* rsvd20;      /* +0x20 (cpPixels2/palette) unused */
};

static void* (*s_pfnRwImageCreate)(int, int, int) = NULL;
static void  (*s_pfnRwImageDestroy)(void*) = NULL;
static int   (*s_pfnRwImageFindRasterFormat)(void*, int, int*, int*, int*, int*) = NULL;
static void* (*s_pfnRwRasterCreate)(int, int, int, int) = NULL;
static void  (*s_pfnRwRasterDestroy)(void*) = NULL;
static int   (*s_pfnRwRasterSetFromImage)(void*, void*) = NULL;
static void* (*s_pfnRwTextureCreate)(void*) = NULL;
static void  (*s_pfnRwTextureDestroy)(void*) = NULL;
static void  (*s_pfnRwTextureSetName)(void*, const char*) = NULL;
/* Engine-provided pixel access. This fork's RwRaster is NOT stock RenderWare
   (diag dump: self-pointer @+0x00, width/height as u32 @+0x18/+0x1c, NO pixel
   buffer pointer in the 0x40-byte header - it's GL-backed). RwRasterGetPixels
   is a MACRO in stock RW (reads raster->pixels) so it never resolves as a
   symbol (GetSym -> NULL, expected); the fork's RwRasterLock is a real export
   with a 3-arg fork-specific signature (nm: _Z12RwRasterLockP8RwRasterhi =
   (RwRaster*, unsigned char, int), offset 0x272f18) and is called
   RwRasterLock(raster, 0, 1) to obtain a readable RwRGBA* (see
   CPostEffects::Initialise). Never blind-scan the struct (a previous scan
   dereferenced the packed dims 0x8000000080 as a pointer and SEGV'd). */
static void* (*s_pfnRwRasterGetPixels)(void*) = NULL;
static void* (*s_pfnRwRasterLock)(void*, unsigned char, int) = NULL;
static void  (*s_pfnRwRasterUnlock)(void*) = NULL;

/* ---- v8: immediate-mode 2D canvas ---------------------------------------
   The engine already exposes RenderWare's 2D immediate pipeline, which is all a
   shape/alpha layer needs: per-vertex RGBA plus blend state. No ImGui, no DOM.

   Clients do NOT call RenderWare. They push geometry from SetTick (which runs
   in the CGame_Process pump); we accumulate it and flush the WHOLE frame as a
   single indexed triangle list inside the Render2dStuff hook, where the engine
   has actually set up the 2D raster. Two reasons for that split:
     - CGame_Process is early in the frame; there is no valid 2D raster state
       yet, so drawing from there would target whatever the last raster was.
     - one RW call per frame instead of one per primitive, and a flush point
       isolated in one function if the hook ever needs moving.

   Coordinates are already REAL PIXELS, passed through untouched: the same space
   as GetRect/GetTap, so canvas art and hit-testing never disagree.

   The psdk RenderWare headers are NOT includable standalone (sdk_base.h demands
   plugin.h first), and the framework resolves RenderWare by mangled symbol
   anyway, so the two types and the enum values we need are declared here. They
   mirror gta_base/DrawVertices.h and renderware/RwRender.h - keep in sync if
   those ever change, or assert against them from a plugin.h translation unit. */
struct RwIm2DVertex
{
    float x, y, z, rhw;          /* screen x/y/z, reciprocal homogeneous W */
    unsigned char r, g, b, a;    /* vertex colour, byte order as stored */
    float u, v;                  /* texture coords (unused: no texture) */
};
typedef unsigned short RwImVertexIndex;

/* RenderWare render-state ids (aml-psdk/renderware/RwRender.h enum
   RwRenderState) and the blend ids from the same header. TEXTURERASTER takes a
   pointer to an RwRaster - NOT to the RwTexture that wraps it. We bind it to NULL
   before the untextured shape batch so the engine's last raster is not inherited. */
enum { RW_RS_TEXTURE_RASTER = 1, RW_RS_ZTEST_ENABLE = 6, RW_RS_ZWRITE_ENABLE = 8,
       RW_RS_SRC_BLEND = 10, RW_RS_DST_BLEND = 11, RW_RS_VERTEX_ALPHA_ENABLE = 12 };
enum { RW_BLEND_SRC_ALPHA = 5, RW_BLEND_INV_SRC_ALPHA = 6 };
enum { RW_PRIMTYPE_TRI_LIST = 3 };

#define CANVAS_MAX_VERTS  4096
#define CANVAS_MAX_INDICES 6144

static RwIm2DVertex   s_canvasVerts[CANVAS_MAX_VERTS];
static RwImVertexIndex s_canvasIdx[CANVAS_MAX_INDICES];
static int s_canvasNumVerts = 0;
static int s_canvasNumIdx   = 0;
static int s_canvasSrcBlend = RW_BLEND_SRC_ALPHA;
static int s_canvasDstBlend = RW_BLEND_INV_SRC_ALPHA;
static int s_canvasLostVerts = 0;   /* overflow accounting, logged once */

static int   (*s_pfnRwRenderStateSet)(int, void*) = NULL;
static int   (*s_pfnRwIm2DRenderIndexedPrimitive)(int, RwIm2DVertex*, int, RwImVertexIndex*, int) = NULL;

/* Append one vertex in real pixels. Returns 0 when the batch is full, so the
   draw call can bail instead of writing past the arrays. */
static bool Canvas_Push(float x, float y, uint32_t rgba)
{
    if(s_canvasNumVerts >= CANVAS_MAX_VERTS) { s_canvasLostVerts++; return false; }
    RwIm2DVertex* v = &s_canvasVerts[s_canvasNumVerts++];
    v->x   = x;
    v->y   = y;
    v->z   = 0.0f;
    v->rhw = 1.0f;
    v->r   = (unsigned char)( rgba        & 0xFF);
    v->g   = (unsigned char)((rgba >>  8) & 0xFF);
    v->b   = (unsigned char)((rgba >> 16) & 0xFF);
    v->a   = (unsigned char)((rgba >> 24) & 0xFF);
    v->u = v->v = 0.0f;
    return true;
}

/* Append one triangle. All-or-nothing: on overflow the vertices stay orphaned in
   the batch but no index is emitted, so a dropped triangle can never reference
   vertices belonging to the next shape. */
static void Canvas_Tri(const float* xy, uint32_t rgba)
{
    if(s_canvasNumIdx + 3 > CANVAS_MAX_INDICES) { s_canvasLostVerts++; return; }
    const RwImVertexIndex base = (RwImVertexIndex)s_canvasNumVerts;
    if(!Canvas_Push(xy[0], xy[1], rgba)) return;
    if(!Canvas_Push(xy[2], xy[3], rgba)) return;
    if(!Canvas_Push(xy[4], xy[5], rgba)) return;
    s_canvasIdx[s_canvasNumIdx++] = base;
    s_canvasIdx[s_canvasNumIdx++] = (RwImVertexIndex)(base + 1);
    s_canvasIdx[s_canvasNumIdx++] = (RwImVertexIndex)(base + 2);
}

/* Convex or star-shaped polygon as a triangle fan. Good enough for circles,
   rounded shapes, arrows and rings; NOT for self-intersecting outlines. */
static void Canvas_Fan(const float* xy, int count, uint32_t rgba)
{
    for(int i = 1; i < count - 1; ++i) {
        const float t[6] = { xy[0], xy[1], xy[2*i], xy[2*i+1], xy[2*i+2], xy[2*i+3] };
        Canvas_Tri(t, rgba);
    }
}

/* Stroke a closed/open outline by fanning quads between each segment and the
   next, so the stroke keeps a real thickness instead of relying on the raster's
   fixed line width. */
static void Canvas_Stroke(const float* xy, int count, uint32_t rgba, float width, bool closed)
{
    if(width <= 0.0f) return;
    const int segs = closed ? count : count - 1;
    for(int i = 0; i < segs; ++i) {
        const int a = i * 2, b = ((i + 1) % count) * 2;
        /* Offset perpendicular to the segment, half the width each way. */
        const float dx = xy[b] - xy[a], dy = xy[b+1] - xy[a+1];
        const float len = dx * dx + dy * dy;
        if(len < 1e-6f) continue;
        const float h = width * 0.5f;
        const float nx = -dy * h / (float)sqrt(len), ny = dx * h / (float)sqrt(len);
        const float q0[6] = { xy[a]+nx,   xy[a+1]+ny,   xy[a]-nx,   xy[a+1]-ny,   xy[b]-nx,   xy[b+1]-ny };
        const float q1[6] = { xy[a]+nx,   xy[a+1]+ny,   xy[b]-nx,   xy[b+1]-ny,   xy[b]+nx,   xy[b+1]+ny };
        Canvas_Tri(q0, rgba);
        Canvas_Tri(q1, rgba);
    }
}

/* Reset the batch. Called at the top of the frame, before the client tick, so
   each frame's geometry stands alone and a client that stops drawing simply
   stops drawing (rather than accumulating a permanent polygon pile). */
static void Canvas_Clear(void)
{
    s_canvasNumVerts = 0;
    s_canvasNumIdx   = 0;
    if(s_canvasLostVerts) { logger->Info("MenuKit: canvas batch overflow, %d prim(s) dropped", s_canvasLostVerts); s_canvasLostVerts = 0; }
}

/* The states a 2D overlay wants; the engine restores its own after the frame.
   Shared by both submit paths so shapes and text blend identically. */
static void Canvas_SetBlendStates(void)
{
    s_pfnRwRenderStateSet(RW_RS_ZTEST_ENABLE,           (void*)(intptr_t)0);
    s_pfnRwRenderStateSet(RW_RS_ZWRITE_ENABLE,          (void*)(intptr_t)0);
    s_pfnRwRenderStateSet(RW_RS_VERTEX_ALPHA_ENABLE,     (void*)(intptr_t)1);
    s_pfnRwRenderStateSet(RW_RS_SRC_BLEND,             (void*)(intptr_t)s_canvasSrcBlend);
    s_pfnRwRenderStateSet(RW_RS_DST_BLEND,            (void*)(intptr_t)s_canvasDstBlend);
}

/* Submit the pending untextured shapes and reset the batch. Resetting here is
   what lets Canvas_Flush call this more than once per frame: without it the same
   triangles would be re-submitted around every text op. */
static void Canvas_SubmitShapes(void)
{
    if(s_canvasNumIdx < 3) return;
    s_pfnRwRenderStateSet(RW_RS_TEXTURE_RASTER,         NULL);
    Canvas_SetBlendStates();
    s_pfnRwIm2DRenderIndexedPrimitive(RW_PRIMTYPE_TRI_LIST, s_canvasVerts,
                                      s_canvasNumVerts, s_canvasIdx, s_canvasNumIdx);
    s_canvasNumVerts = 0;
    s_canvasNumIdx   = 0;
}

/* Submit the frame. Called from the Render2dStuff hook, i.e. after the engine has
   set up the 2D raster. Shapes and text share one untextured batch, so call order
   is preserved by construction and there is nothing to interleave here. */
static void Canvas_Flush(void)
{
    if(!s_pfnRwRenderStateSet || !s_pfnRwIm2DRenderIndexedPrimitive) return;
    Canvas_SubmitShapes();
}


/* Real screen size in pixels (OS_ScreenGetWidth/Height, resolved at init like
   MenuVSL does). Used to project the widget's VIRTUAL 640x448 position to
   real pixels for the label panel — the widget's own live rect (+0x2c..+0x38)
   is a 1e6 sentinel until the game activates the widget (observed in the pool
   dump), so reading it produces absurd h and silently fails the raster. */
static int (*s_pfnOS_ScreenGetWidth)(void)  = NULL;
static int (*s_pfnOS_ScreenGetHeight)(void) = NULL;
static float s_screenW = 640.0f;   /* fallbacks until resolved */
static float s_screenH = 448.0f;

/* Global pointer state. AML exposes no touch/pad API, so "was that tap outside
   my panel?" was unanswerable: a client could only react to touches that landed
   on one of ITS OWN widgets, and a full-screen catcher widget would swallow the
   game's joystick and fire button.

   The state lives in static members of CTouchInterface, which is just a global
   with a class name (the mangling ends in `E` for static data members):
     m_bTouchDown  - a finger is on the glass
     m_vecCachedPos - CVector2D, the position the game cached for that touch
   Read directly, no CTouchInterface instance needed (GetTouchPosition(int) is a
   non-static member, so it would need one; the statics need nothing). */
static bool*  s_pTouchDown  = NULL;   /* CTouchInterface::m_bTouchDown */
static float* s_pTouchPos   = NULL;   /* CTouchInterface::m_vecCachedPos, {x, y} */
/* gMobileMenu: the game's own MobileMenu singleton, 208 bytes. Reverse
   engineered in libGTASA_device.so (see mod/menu-api.h for the layout): it
   owns the stack of menu screens, and the engine gates all menu work on
   "+0x24 (stack depth) != 0 || +0x30 (current screen) != NULL". That is the
   same test MobileMenu::Update does at its top, so reusing it is exactly the
   engine's own idea of "a menu is on screen" - not a guess. */
static void*  s_gMobileMenu = NULL;
/* Latched by the pump so GetTap is a pure read; see the CGame_Process hook. */
static bool   s_prevDown    = false;
static int    s_tapThisFrame = 0;
static float  s_tapX = 0.0f, s_tapY = 0.0f;
static int    s_tapCount = 0;   /* DEBUG: counts rising edges to expose a bouncing signal */
/* Bounded proof that m_vecCachedPos tracks a DRAG and not just the touchdown
   point: logs only while a finger is down AND the point moved >4px since the
   last line. This is the evidence GetPointer's drag story depends on - if the
   engine only cached the initial press point, these would never fire. */
static int    s_dragLogCount = 0;
static float  s_dragLastX = 0.0f, s_dragLastY = 0.0f;

/* Icon cache: same path -> same RwTexture (shared across buttons). Created
   once and NEVER destroyed by the mod: we write sprite[0] (+0x10) directly and
   the widget is never SetTexture'd again, so no double-free. ponytail: the
   textures live until the game closes; accepted. */
/* Icon cache. ok = the raster-member fingerprint was verified at creation
   (RwTexture.raster@0x0 readback == the raster we handed RwTextureCreate).
   The game's streaming heap can recycle our block without touching the
   widget's sprite pointer (observed: slot keeps our tex addr but renders a
   game ped texture), so every cache hit re-checks the fingerprint and reloads
   the PNG fresh when the block was stolen. */
struct IconEntry
{
    void*  tex;
    void*  ras;
    bool   ok;       /* tex->raster@0x0 == ras verified at creation */
    bool   okPix;    /* pixel-content fingerprint calibrated (stable engine pixel access) */
    void*  pix;      /* pixel pointer value from RwRasterGetPixels (validated at creation) */
    size_t pixOff;   /* 0 = stable engine pixel access (RwRasterGetPixels), -1 = none */
    int    w, h;     /* raster dims (for the center-sample offset) */
    unsigned char snap[16]; /* 16B at the center sample (red) */
};
static std::map<std::string, IconEntry> s_iconCache;
/* Textures are NEVER freed (s_pfnRwTextureDestroy has no call sites), so this
   cap is a hard ceiling on icon memory, not a cache tuning knob. 48 entries at
   the 512 px cap is ~48 MiB of rasters. */
#define ICON_CACHE_MAX_ENTRIES 48
static bool s_iconCacheFullLogged = false;

/* Plan B: label-texture cache. Same path-key trick as s_iconCache but keyed
   by the label string; rendered once and NEVER destroyed by the mod (same
   ponytail as icons: textures live until the game closes). Same liveness
   machinery: texture block recycling by the game is detected via the raster
   fingerprint re-check so the pump can re-render fresh. */
static std::map<std::string, IconEntry> s_textCache;

/* Widget registry */
struct WidgetEntry
{
    bool        active;
    bool        wasTouched;   /* previous frame's IsTouched, for falling-edge release */
    int         slot;         /* index into m_pWidgets; -1 = not built yet */
    void*       widget;       /* object in the pool at slot; NULL until first built frame */
    int         menu;         /* owning menu id; 0 = ROOT (shown when no menu is open) */
    MenuKit_OnReleaseCallback onRelease;
    void*       userdata;
    char        texture[64];  /* saved so the widget can be (re)built lazily */
    char        icon[256];    /* custom icon PNG path (absolute device path); empty = game default */
    void*       iconTex;      /* cached custom RwTexture from MenuKit_LoadIcon; NULL = none/failed */
    char        text[128];    /* Plan B label: rendered by SetText into a texture ("" = no label) */
    void*       textTex;      /* cached label texture from MenuKit_RenderTextTexture */
    uint8_t     alpha;        /* 0..255 written at build; 0xFF = opaque (SetAlpha) */
    uint8_t     visible;      /* 0 = parked (not drawn, not touchable); SetVisible */
    int         hasSize;      /* 1 = sizeW/sizeH owned by us, not the engine's square */
    float       sizeW, sizeH; /* size in REAL pixels, applied around the engine's centre; SetSize */
    int         sizeCal;      /* 1 = pxPerUnit* already calibrated against the engine */
    float       pxPerUnitW, pxPerUnitH; /* real px per virtual unit, measured once from the engine */
    WidgetPosition pos;
};
static WidgetEntry s_widgets[OUR_WIDGET_LIMIT];

/* Client per-frame callback (SetTick). AML exposes no per-frame hook, so this
   pump is the only place a client can run a timer (auto-hide, toast). */
static MenuKit_TickCallback s_tick = NULL;
static void* s_tickUser = NULL;

/* Menu stack (push/pop). Only the ACTIVE menu's widgets ever occupy pool
   slots; switching menus destroys the outgoing group so the game can reuse
   the slots (pool is ~172/190 used with menus open — every slot counts). */
static int s_menuStack[MENU_STACK_MAX];
static int s_menuDepth = 0;

/* Pixel-content liveness: the game can recycle the raster's PIXEL BUFFER —
   the texture struct still passes (raster member unchanged) but the button
   renders a borrowed/white texture. The center of the raster must still hold
   our icon's red center snapshot, and the raster's dims must match what we
   validated at creation. All reads here are bounded: ras is live
   (tex->raster==ras checked by the caller), the dims reads stay inside
   RwRaster, and the pixel read goes through the ENGINE's own accessor
   (never a struct field treated as a pointer — that SEGV'd at +0x23724).
   Layout verified by diag: this fork stores dims as u32 @+0x18/+0x1c (stock
   RW's +0x0c/+0x0e u16 are 0 here) and has NO pixel pointer in the 0x40-byte
   header (GL-backed). */
static bool MenuKit_IconContentAlive(const IconEntry& c)
{
    if(!c.okPix) return true;
    if(*(const uint32_t*)((uintptr_t)c.ras + 0x18) != (uint32_t)c.w) return false;
    if(*(const uint32_t*)((uintptr_t)c.ras + 0x1c) != (uint32_t)c.h) return false;
    /* okPix=true only when calibration got a stable pointer from
       RwRasterGetPixels. A NULL return does NOT mean dead: GL-backed rasters
       may expose no CPU pixels outside an explicit RwRasterLock, so on NULL we
       fall back to the dims gate (already passed) instead of reloading every
       frame. A non-NULL pointer that MOVED (p != c.pix) is a real recycle
       signal: reload. Otherwise require the red center snapshot to match. */
    if(s_pfnRwRasterGetPixels)
    {
        const unsigned char* p = (const unsigned char*)s_pfnRwRasterGetPixels(c.ras);
        if(p)
        {
            if(p != c.pix) return false;
            size_t mid = ((size_t)c.h / 2 * (size_t)c.w + (size_t)c.w / 2) * 4;
            return memcmp(p + mid, c.snap, sizeof(c.snap)) == 0;
        }
    }
    return true; /* no CPU pixel path outside lock; dims gate only */
}

/* Load a custom icon PNG (absolute device path) into an RwTexture, replicating
   1:1 the game's own pipeline (CSprite2d::SetTexture @0x6ecce8: findRasterFormat
   -> rasterCreate -> rasterSetFromImage -> textureCreate). Zero ownership
   tricks: the stbi buffer lives only during the call (cpPixels is cleared
   before RwImageDestroy; flags bit0 never set, so the engine never frees it).
   Result is cached by path and never destroyed by the mod. */
/* Covers arrive at 1200-1500 px but the widgets that display them are ~300 px,
   and a raster costs w*h*4. Measured: three covers (1200^2 + 1400^2 + 1500^2 =
   5.8+7.8+9 MB) exhaust the RenderWare raster pool, and the NEXT
   RwRasterSetFromImage then writes into a pixel buffer the allocator could not
   commit -> SIGSEGV SEGV_ACCERR, reproducibly on the 4th distinct cover.
   Box-filter down to a cap BEFORE the raster is created: 512 px keeps ~1.7x
   density on a 300 px widget and costs 1 MB instead of 9. */
#define ICON_MAX_DIM 512

static void Icon_DownscaleRGBA(const unsigned char* src, int sw, int sh,
                               unsigned char* dst, int dw, int dh)
{
    for(int y = 0; y < dh; ++y)
    {
        int y0 = (int)((int64_t)y * sh / dh);
        int y1 = (int)((int64_t)(y + 1) * sh / dh);
        if(y1 <= y0) y1 = y0 + 1;
        for(int x = 0; x < dw; ++x)
        {
            int x0 = (int)((int64_t)x * sw / dw);
            int x1 = (int)((int64_t)(x + 1) * sw / dw);
            if(x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for(int sy = y0; sy < y1; ++sy)
            {
                const unsigned char* row = src + (size_t)sy * sw * 4;
                for(int sx = x0; sx < x1; ++sx)
                {
                    r += row[sx * 4 + 0]; g += row[sx * 4 + 1];
                    b += row[sx * 4 + 2]; a += row[sx * 4 + 3];
                    ++n;
                }
            }
            unsigned char* o = dst + ((size_t)y * dw + x) * 4;
            o[0] = (unsigned char)(r / n); o[1] = (unsigned char)(g / n);
            o[2] = (unsigned char)(b / n); o[3] = (unsigned char)(a / n);
        }
    }
}

static void* MenuKit_LoadIcon(const char* path)
{
    if(!path || !*path) return NULL;
    if(!s_pfnRwImageCreate || !s_pfnRwImageDestroy || !s_pfnRwImageFindRasterFormat ||
       !s_pfnRwRasterCreate || !s_pfnRwRasterSetFromImage || !s_pfnRwTextureCreate)
    {
        logger->Error("MenuKit: RenderWare symbols missing, icon '%s' ignored", path);
        return NULL;
    }

    auto it = s_iconCache.find(path);
    if(it != s_iconCache.end())
    {
        const IconEntry& c = it->second;
        if(c.ok)
        {
            /* Liveness check: RwTexture.raster@0x0 must still point at the
               raster we created, AND the raster's pixel center must still hold
               our icon (content fingerprint). The game's streaming can recycle
               the texture block (slot keeps our tex pointer but the content is
               now a game texture) or just the raster's pixel buffer (texture
               struct checks pass but the button shows a borrowed/white texture).
               On mismatch we drop the entry and reload fresh. */
            if(c.tex && *(void**)c.tex == c.ras && MenuKit_IconContentAlive(c))
                return c.tex;
            logger->Info("MenuKit: icon '%s' recycled by game, reloading fresh", path);
            s_iconCache.erase(it);
        }
        else
        {
            return c.tex; /* fingerprint unavailable; trust the cached pointer */
        }
    }

    /* NOTHING in this framework ever frees a texture: s_pfnRwTextureDestroy is
       resolved but has zero call sites. So every texture we build leaks for the
       rest of the process, and s_iconCache is what stops us building the SAME
       one twice. Bound the leak by refusing to build once the cache is at its
       budget, NOT by destroying: calling RwTextureDestroy on a raster the engine
       may already have recycled is use-after-free, and mutating the refcount at
       +0x64 to "make it safe" is a guess about engine internals on a field the
       liveness check only ever READS. Returning NULL here is safe: both callers
       fall back to the widget's default sprite. */
    if(s_iconCache.size() >= ICON_CACHE_MAX_ENTRIES)
    {
        if(!s_iconCacheFullLogged)
        {
            s_iconCacheFullLogged = true;
            logger->Print(LogP_Warn, "MenuKit: icon cache at budget (%d entries, ~%d MiB of rasters at %d px) — "
                         "further icons skipped; textures are never freed, so this is a "
                         "hard memory ceiling, not a soft one",
                         ICON_CACHE_MAX_ENTRIES,
                         (int)((size_t)ICON_CACHE_MAX_ENTRIES * ICON_MAX_DIM * ICON_MAX_DIM * 4 / (1024 * 1024)),
                         ICON_MAX_DIM);
        }
        return NULL;
    }

    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load(path, &w, &h, &ch, 4);   /* RGBA8 */
    if(!px) { logger->Error("MenuKit: icon '%s' not decodable", path); return NULL; }

    /* Never hand the raster a full-resolution cover: see ICON_MAX_DIM. */
    if(w > ICON_MAX_DIM || h > ICON_MAX_DIM)
    {
        int nw, nh;
        if(w >= h) { nw = ICON_MAX_DIM; nh = (int)((int64_t)h * ICON_MAX_DIM / w); }
        else       { nh = ICON_MAX_DIM; nw = (int)((int64_t)w * ICON_MAX_DIM / h); }
        if(nw < 1) nw = 1;
        if(nh < 1) nh = 1;
        stbi_uc* small = (stbi_uc*)malloc((size_t)nw * (size_t)nh * 4);
        if(small)
        {
            Icon_DownscaleRGBA(px, w, h, small, nw, nh);
            stbi_image_free(px);
            px = small;
            logger->Info("MenuKit: icon '%s' %dx%d -> %dx%d (raster pool is finite)", path, w, h, nw, nh);
            w = nw; h = nh;
        }
    }

    void* img = s_pfnRwImageCreate(w, h, 32);
    if(!img) { stbi_image_free(px); return NULL; }
    RwImageLayout* il = (RwImageLayout*)img;
    il->cpPixels = px;           /* backend copies it into the raster */
    il->stride = w * 4;          /* RGBA8: rows advance by w*4 in the copy loop */

    void* tex = NULL;
    int ow = 0, oh = 0, od = 0, of = 0;
    /* rasterType = rwRASTERTYPETEXTURE (0x10), NOT CAMERA (4): the CAMERA type
       makes this GL fork pick rwRASTERFORMAT888 (24-bit RGB, no alpha ->
       corrupted colors when the premultiplied-RGBA sprite pipeline samples it).
       Game Txd textures (texture type) are 32bpp RGBA (stride = w*4). Empirically
       verified: raster diag showed od=32 requested but bpp=24/stride=w*3 header
       and of=0x604 (0x600 = 888 format, low nibble 4 = CAMERA type). */
    if(s_pfnRwImageFindRasterFormat(img, 0x10, &ow, &oh, &od, &of))
    {
        void* ras = s_pfnRwRasterCreate(ow, oh, od, of);
        if(ras)
        {
            if(s_pfnRwRasterSetFromImage(ras, img))
            {
                il->cpPixels = NULL; /* copied; we free px now */
                stbi_image_free(px); px = NULL;
                s_pfnRwImageDestroy(img); img = NULL;
                tex = s_pfnRwTextureCreate(ras);
                if(tex)
                {
                    if(s_pfnRwTextureSetName) s_pfnRwTextureSetName(tex, "MenuKitIcon");
                    /* Keep the texture alive across the game's streaming
                       sweeps: CSprite2d::SetTexture (0x6ece38-0x6ece48) bumps a
                       u32 refcount at texture+0x64 right after
                       sprite->texture = tex. Without it the engine recycles
                       our block ~80ms after creation ("recycled by game,
                       reloading fresh" in the log) and the button renders a
                       borrowed game texture. Mirror the game exactly:
                       ldr; add #1; str on +0x64. */
                    if(*(uint32_t*)((uintptr_t)tex + 0x64) < 0x0fffffff)
                        *(uint32_t*)((uintptr_t)tex + 0x64) += 1;
                    /* Fingerprint at creation: RwTexture.raster is its FIRST
                       member, so reading *tex must give back the very raster we
                       handed to RwTextureCreate. ok=false means the layout
                       assumption failed and cache-hits trust the pointer. */
                    bool fpOk = (*(void**)tex == ras);
                    if(!fpOk)
                        logger->Info("MenuKit: icon '%s' fingerprint mismatch (raster@tex=%p != %p), liveness check off", path, *(void**)tex, ras);
                    /* DIAGNOSTIC: dump the raster header right after creation so
                       we can calibrate the RwRaster offsets for this fork (the
                       stock layout guard below failed: content never calibrated).
                       Read-only on the live struct, no pixel deref. */
                    {
                        char hdr[512] = {0}; int hp = 0;
                        const unsigned char* rb = (const unsigned char*)ras;
                        for(int i = 0; i < 0x40 && hp < 470; i += 4)
                            hp += snprintf(hdr + hp, sizeof(hdr) - hp, "%02x%02x%02x%02x ", rb[i], rb[i+1], rb[i+2], rb[i+3]);
                        char qws[512] = {0}; int qp = 0;
                        for(int i = 0; i < 0x40 && qp < 470; i += 8)
                            qp += snprintf(qws + qp, sizeof(qws) - qp, "[%02x]=%p ", i, *(const void**)(rb + i));
                        logger->Info("MenuKit: [diag] raster@%p ow=%d oh=%d od=%d of=%#x", ras, ow, oh, od, of);
                        logger->Info("MenuKit: [diag] dwords: %s", hdr);
                        logger->Info("MenuKit: [diag] qwords: %s", qws);
                    }
                    /* Content fingerprint via the fork's OWN pixel accessors.
                       Diag proved this runtime is GL-backed: dims are u32 at
                       +0x18/+0x1c (the classic u16 at +0x0c/+0x0e read 0 here)
                       and 0x8000000080 was packed dims, NOT a pointer — reading
                       +0x18 as a pointer is exactly what SEGV'd at 0x23724.
                       So: gate on fork dims, then fetch pixels through the
                       engine's exported RwRasterGetPixels; if the raster has no
                       CPU buffer (GL), one RwRasterLock(READ) forces a readback
                       probe (paired unlock) so we can still fingerprint the
                       content at creation — its result decides hypothesis A
                       (recycle/rendering) vs B (copy broken at creation). Only
                       a STABLE GetPixels buffer becomes the per-frame liveness
                       pointer (pixOff=0, re-fetched each check); a Lock probe
                       pointer is NOT retained (it is transient). */
                    size_t pixOff = (size_t)-1;   /* no stable engine pixel buffer */
                    void* pixPtr = NULL;
                    unsigned char centerSnap[16];
                    bool pixOk = false;
                    const char* via = "none";
                    if(*(const uint32_t*)((uintptr_t)ras + 0x18) == (uint32_t)ow &&
                       *(const uint32_t*)((uintptr_t)ras + 0x1c) == (uint32_t)oh)
                    {
                        const unsigned char* p = NULL;
                        bool locked = false;
                        if(s_pfnRwRasterGetPixels)
                        {
                            p = (const unsigned char*)s_pfnRwRasterGetPixels(ras);
                            if(p) via = "GetPixels";
                        }
                        if(!p && s_pfnRwRasterLock)
                        {
                            /* fork signature (ras, uchar reserved=0, int mode): 1 = read
                               (CPostEffects returns RwRGBA*), paired unlock below */
                            p = (const unsigned char*)s_pfnRwRasterLock(ras, 0, 1);
                            locked = (p != NULL);
                            if(p) via = "Lock";
                        }
                        if(p)
                        {
                            size_t mid = ((size_t)oh / 2 * (size_t)ow + (size_t)ow / 2) * 4;
                            if(mid + 16 <= (size_t)ow * (size_t)oh * 4)  /* bounds sanity */
                            {
                                /* red-ish center pixel: icon is a solid red square. Channel order is
                                   unknown until first read (fork GL backend may
                                   expose RGBA or BGRA), so accept either: one
                                   color channel high, the other two low, alpha
                                   high. The c16 dump identifies the real layout
                                   if neither order matches. */
                                bool redRGBA = p[mid] > 0xA0 && p[mid+1] < 0x60 && p[mid+2] < 0x60;
                                bool redBGRA = p[mid] < 0x60 && p[mid+1] < 0x60 && p[mid+2] > 0xA0;
                                bool alphaHi = p[mid+3] > 0x80;
                                char c16[64];
                                snprintf(c16, sizeof(c16),
                                         "%02x %02x %02x %02x | %02x %02x %02x %02x | %02x %02x %02x %02x | %02x %02x %02x %02x",
                                         p[mid], p[mid+1], p[mid+2], p[mid+3],
                                         p[mid+4], p[mid+5], p[mid+6], p[mid+7],
                                         p[mid+8], p[mid+9], p[mid+10], p[mid+11],
                                         p[mid+12], p[mid+13], p[mid+14], p[mid+15]);
                                if((redRGBA || redBGRA) && alphaHi)
                                {
                                    if(!locked)
                                    {
                                        pixOff = 0;   /* stable engine buffer, re-fetched */
                                        pixPtr = (void*)p;
                                        memcpy(centerSnap, p + mid, 16);
                                        pixOk = true;
                                    }
                                    logger->Info("MenuKit: icon '%s' red center CONFIRMED via %s (locked=%d, order=%s) c16=%s", path, via, locked, redRGBA ? "RGBA" : "BGRA", c16);
                                }
                                else
                                    logger->Info("MenuKit: icon '%s' center NOT red via %s (locked=%d) c16=%s — hypothesis B (creation copy broken)", path, via, locked, c16);
                            }
                            if(locked && s_pfnRwRasterUnlock) s_pfnRwRasterUnlock(ras);
                        }
                    }
                    else
                        logger->Info("MenuKit: icon '%s' fork dims gate failed (hdr %u x %u != %d x %d)", path,
                                     *(const uint32_t*)((uintptr_t)ras + 0x18),
                                     *(const uint32_t*)((uintptr_t)ras + 0x1c), ow, oh);
                    if(!pixOk)
                        logger->Info("MenuKit: icon '%s' content fingerprint not found, liveness = member+dims only", path);
                    s_iconCache[path] = { tex, ras, fpOk, pixOk, pixPtr, pixOff, ow, oh };
                    if(pixOk) memcpy(s_iconCache[path].snap, centerSnap, 16);
                    logger->Info("MenuKit: icon '%s' loaded (%dx%d -> %p, fp=%d, content=%d)", path, w, h, tex, fpOk, pixOk);
                    return tex;
                }
                s_pfnRwRasterDestroy(ras);           /* textureCreate failed */
            }
        }
    }
    if(img) s_pfnRwImageDestroy(img);                /* flags bit0=0 -> never touches px */
    if(px)  stbi_image_free(px);
    logger->Error("MenuKit: icon '%s' pipeline failed", path);
    return NULL;
}

/* ---- Plan B: label textures (NO CFont — CFont no compone en este fork) ----
   Same upload path as MenuKit_LoadIcon (findRasterFormat 0x10 -> rasterCreate
   -> setFromImage -> textureCreate -> refcount +0x64), factored out so the
   icon path and the label path share ONE verified pipeline. The label is
   rasterized with the embedded 5x7 bitmap font and COMPOSITED either over the
   widget's icon PNG pixels (text inside the button image -> guaranteed on
   top) or over a generated dark panel sized to the widget's live on-screen
   rect (floats l/t/r/b at +0x2c..+0x38, real pixels). */

/* Upload an RGBA8 buffer to a fresh RwTexture (game's own pipeline). The
   buffer is consumed (copied by RwRasterSetFromImage, then freed here).
   outRas (optional) receives the raster for liveness fingerprinting.
   Returns the texture, or NULL on failure. */
static void* MenuKit_UploadRGBA(stbi_uc* px, int w, int h, void** outRas)
{
    if(!px || w <= 0 || h <= 0 || !s_pfnRwImageCreate || !s_pfnRwImageDestroy ||
       !s_pfnRwImageFindRasterFormat || !s_pfnRwRasterCreate || !s_pfnRwRasterDestroy ||
       !s_pfnRwRasterSetFromImage || !s_pfnRwTextureCreate)
    {
        if(px) stbi_image_free(px);
        return NULL;
    }
    void* img = s_pfnRwImageCreate(w, h, 32);
    if(!img) { stbi_image_free(px); return NULL; }
    RwImageLayout* il = (RwImageLayout*)img;
    il->cpPixels = px;          /* backend copies it into the raster */
    il->stride = w * 4;         /* RGBA8 rows advance by w*4 */
    void* tex = NULL;
    int ow = 0, oh = 0, od = 0, of = 0;
    if(s_pfnRwImageFindRasterFormat(img, 0x10, &ow, &oh, &od, &of))
    {
        void* ras = s_pfnRwRasterCreate(ow, oh, od, of);
        if(ras)
        {
            if(s_pfnRwRasterSetFromImage(ras, img))
            {
                il->cpPixels = NULL; /* copied; we free px now */
                stbi_image_free(px); px = NULL;
                s_pfnRwImageDestroy(img); img = NULL;
                tex = s_pfnRwTextureCreate(ras);
                if(tex)
                {
                    if(s_pfnRwTextureSetName) s_pfnRwTextureSetName(tex, "MenuKitLabel");
                    /* mirror the game's refcount bump so streaming never
                       recycles our block (see LoadIcon comment) */
                    if(*(uint32_t*)((uintptr_t)tex + 0x64) < 0x0fffffff)
                        *(uint32_t*)((uintptr_t)tex + 0x64) += 1;
                    if(outRas) *outRas = ras;
                    return tex;
                }
            }
            s_pfnRwRasterDestroy(ras);
        }
    }
    if(img) s_pfnRwImageDestroy(img);
    if(px)  stbi_image_free(px);
    return NULL;
}

/* Rasterize a label into a fresh RGBA8 buffer with the embedded 5x7 font.
   scale: glyph multiplier (1 = raw 5x7). Returns the buffer (caller frees
   with stbi_image_free), or NULL. */
static stbi_uc* MenuKit_RasterizeText(const char* text, int scale,
                                      unsigned r, unsigned g, unsigned b, unsigned a)
{
    if(!text || !*text || scale < 1) return NULL;
    if(scale > 16) scale = 16;
    size_t len = strlen(text);
    int w = (int)len * 6 * scale;   /* 5 cols + 1 spacing per glyph */
    int h = 8 * scale;              /* 7 rows + 1 */
    if(w < 1 || h < 1) return NULL;
    stbi_uc* px = (stbi_uc*)calloc((size_t)w * (size_t)h * 4, 1);
    if(!px) return NULL;
    for(size_t i = 0; i < len; ++i)
    {
        unsigned char c = (unsigned char)text[i];
        if(c < 0x20 || c > 0x7e) c = '?';
        const unsigned char* glyph = s_font5x7[c - 0x20];
        for(int col = 0; col < 5; ++col)
        {
            unsigned char bits = glyph[col];
            for(int row = 0; row < 7; ++row)
            {
                if(!((bits >> row) & 1)) continue;
                int x0 = (int)i * 6 * scale + col * scale;
                int y0 = row * scale;
                for(int sy = 0; sy < scale; ++sy)
                    for(int sx = 0; sx < scale; ++sx)
                    {
                        size_t off = ((size_t)(y0 + sy) * w + (size_t)(x0 + sx)) * 4;
                        px[off + 0] = (stbi_uc)r;
                        px[off + 1] = (stbi_uc)g;
                        px[off + 2] = (stbi_uc)b;
                        px[off + 3] = (stbi_uc)a;
                    }
            }
        }
    }
    return px;
}

/* Full label pipeline: build (or fetch cached) the RwTexture that carries the
   label, composited per the rules above. Called from the pump only (RW
   symbols + live screen dims are runtime things). Cached by (icon|text) — the
   friendly reload path mirrors LoadIcon's liveness (member + raster check).
   The widget param is the registry entry (not the game object): Variant B
   projects the widget's VIRTUAL 640x448 position to real pixels instead of
   reading the game widget's live rect (+0x2c..+0x38), which our widgets keep
   at the 1e6 sentinel (the game never updates it for touch-immune buttons:
   the pool dump showed 1e6/-1e6 rects and the resulting absurd h silently
   failed the raster). */
static void* MenuKit_RenderTextTexture(const char* text, const char* icon, const WidgetEntry& e)
{
    if(!text || !*text) return NULL;
    if(!s_pfnRwImageCreate || !s_pfnRwImageFindRasterFormat ||
       !s_pfnRwRasterCreate || !s_pfnRwRasterSetFromImage || !s_pfnRwTextureCreate)
        return NULL;

    char key[512];
    snprintf(key, sizeof(key), "label:%s|%s", icon ? icon : "", text);
    auto it = s_textCache.find(key);
    if(it != s_textCache.end())
    {
        const IconEntry& c = it->second;
        if(c.ok)
        {
            if(c.tex && *(void**)c.tex == c.ras) return c.tex;
            logger->Info("MenuKit: label '%s' recycled by game, re-rendering", text);
            s_textCache.erase(it);
        }
        else
        {
            return c.tex; /* fingerprint unavailable; trust the cached pointer */
        }
    }

    stbi_uc* px = NULL;
    int w = 0, h = 0;
    if(icon && *icon)
    {
        /* Variant A: compose the label OVER the icon's own pixels. The text is
           baked into the button image itself — composition on top of the
           sprite is guaranteed because this texture is what Draw renders. */
        int ch = 0;
        px = stbi_load(icon, &w, &h, &ch, 4);
        if(!px) logger->Error("MenuKit: label '%s': icon '%s' not decodable", text, icon);
    }
    if(!px)
    {
        /* Variant B: generated dark panel sized to the widget's on-screen
           rect, derived from the VIRTUAL 640x448 position the game dials to
           screen space (left/right = (OriginX ± ScaleX)*screenW/640,
           top/bottom = (OriginY ± ScaleY)*screenH/448 — same projection the
           game's CWidget::Update applies to native widgets). */
        float l = (e.pos.m_fOriginX - e.pos.m_fScaleX) * s_screenW / 640.0f;
        float r = (e.pos.m_fOriginX + e.pos.m_fScaleX) * s_screenW / 640.0f;
        float t = (e.pos.m_fOriginY - e.pos.m_fScaleY) * s_screenH / 448.0f;
        float b = (e.pos.m_fOriginY + e.pos.m_fScaleY) * s_screenH / 448.0f;
        w = (int)(r - l); h = (int)(b - t);
        if(w < 8) w = 8;
        if(h < 8) h = 8;
        px = (stbi_uc*)calloc((size_t)w * (size_t)h * 4, 1);
        if(!px)
        {
            logger->Error("MenuKit: label '%s': calloc %dx%d failed", text, w, h);
            return NULL;
        }
        for(int i = 0; i < w * h; ++i) /* dark translucent panel */
        {
            px[i*4 + 0] = 0; px[i*4 + 1] = 0; px[i*4 + 2] = 0; px[i*4 + 3] = 0x8C;
        }
    }

    /* Fit the text to ~70% of the panel width, centered. Zero-padding on the
       glyph buffer becomes transparent when composited (alpha 0 skip). */
    size_t len = strlen(text);
    int scale = 1;
    if(len > 0 && w > 0 && h > 0)
    {
        int fitW = (int)((float)w * 0.7f) / (int)(len * 6);
        int fitH = (int)((float)h * 0.9f) / 8;
        int fit = fitW < fitH ? fitW : fitH;
        if(fit > 1) scale = fit > 16 ? 16 : fit;
    }
    stbi_uc* glyphs = MenuKit_RasterizeText(text, scale, 255, 255, 255, 255);
    if(glyphs)
    {
        int gw = (int)len * 6 * scale;
        int gh = 8 * scale;
        int x0 = (w - gw) / 2; if(x0 < 0) x0 = 0;
        int y0 = (h - gh) / 2; if(y0 < 0) y0 = 0;
        for(int gy = 0; gy < gh && y0 + gy < h; ++gy)
        {
            for(int gx = 0; gx < gw && x0 + gx < w; ++gx)
            {
                size_t so = ((size_t)gy * gw + (size_t)gx) * 4;
                if(glyphs[so + 3] == 0) continue;   /* glyph pixel or padding */
                size_t dst = ((size_t)(y0 + gy) * w + (size_t)(x0 + gx)) * 4;
                px[dst + 0] = glyphs[so + 0];
                px[dst + 1] = glyphs[so + 1];
                px[dst + 2] = glyphs[so + 2];
                px[dst + 3] = glyphs[so + 3];
            }
        }
        stbi_image_free(glyphs);
    }

    void* ras = NULL;
    void* tex = MenuKit_UploadRGBA(px, w, h, &ras);
    if(!tex)
    {
        logger->Error("MenuKit: label '%s': upload failed (%dx%d)", text, w, h);
        return NULL;
    }
    IconEntry c;
    memset(&c, 0, sizeof(c));
    c.tex = tex;
    c.ras = ras;
    c.ok  = (ras && *(void**)tex == ras);
    s_textCache[key] = c;
    logger->Info("MenuKit: label '%s' rendered (%dx%d -> %p, scale %d%s)",
                 text, w, h, tex, scale, icon && *icon ? ", over icon" : ", panel");
    return tex;
}

/* Build the real CWidgetButton into the pool. Safe only once the game is
   running (TextureDatabaseRuntime exists); during ON_MOD_LOAD it would SEGV
   inside GetTexture, so this is called from the CGame_Process pump, never
   from AddButton. */
static void MenuKit_BuildWidget(WidgetEntry& e)
{
    void* mem = ::operator new(WIDGET_ALLOC_SIZE);
    memset(mem, 0, WIDGET_ALLOC_SIZE);
    /* ponytail: an old widget may have been freed by the game; we never free it
       ourselves (double-free risk). Fixed limit keeps the leak bounded.
       Ctor args mirror the game's native attack button (CWidgetButtonAttackC2
       0x47444c): it calls CWidgetButtonC2 with (param4=1, param5=0x100,
       param6=1) -> base flags +0x8c = 0x100|0x3 = 0x103, HIDMapping +0x8 = 1,
       +0xa4 = 1. Our old SDK call (1, 0, 0) produced +0x8c = 0x03 and HID = 0.
       NOTE: e.texture must be a name that exists in the game's texture DB
       (e.g. "_shoot" used by the native attack button); the ctor resolves it
       by name via TextureDatabaseRuntime and a bogus name leaves the sprite
       empty -> invisible button. */
    ((void(*)(void*, const char*, const WidgetPosition*, unsigned, unsigned, int))s_pfnCtorButton)
        (mem, e.texture, &e.pos, 1, 0x100, 1);
    /* Render visibility, reverse-engineered from libGTASA.so:
       - base ctor leaves alpha (+0x58)=0 and fade gate (+0x59)=0;
       - CTouchInterface::Update calls CWidget::SetEnabled(w,0) every frame on
         any widget whose flags lack bit 0x4, pinning +0x59=0 (ManageAlpha fade-out);
       - CWidgetButton::Draw early-returns while +0x58==0.
       So: flag 0x4 = touch-immune (skips the auto-disable), +0x59=1 fade-in,
       +0x58=alpha (SetAlpha; 0xFF = the old hardcoded instant-visible).
       A widget parked with SetVisible(0) is the INVERSE of all three — no 0x4,
       +0x59=0, +0x58=0 — which is the game's own idle-button state: not drawn
       and, because the engine re-applies SetEnabled(w,0) every frame to anything
       without 0x4, not touchable either. */
    *(uint32_t*)((uintptr_t)mem + 0x8c) |= 0x04;
    if(!e.visible) *(uint32_t*)((uintptr_t)mem + 0x8c) &= ~0x04u;
    *(uint8_t*)((uintptr_t)mem + 0x59) = e.visible ? 0x01 : 0x00;
    *(uint8_t*)((uintptr_t)mem + 0x58) = e.visible ? e.alpha : 0x00;
    /* Custom icon: swap sprite[0] (+0x10) post-ctor. The ctor resolved
       e.texture from the game's DB so the sprite exists; Draw reads sprite[0]
       directly (CWidgetButton::Draw @0x373698) — writing the pointer is all it
       needs. The game texture being replaced is SHARED (the same "shoot"
       texture backs the native attack button), so it is never destroyed here;
       the pump re-applies our texture every frame the game overwrites it. */
    if(e.icon[0])
    {
        void* tex = MenuKit_LoadIcon(e.icon);
        if(tex)
        {
            void** field = (void**)((uintptr_t)mem + 0x10);
            *field = tex;
            e.iconTex = tex;
            logger->Info("MenuKit: widget '%s' uses custom icon", e.texture);
        }
    }
    /* Plan B label: like the icon, swap sprite[0] post-ctor. The label
       texture is either the icon pixels + text baked in, or a generated dark
       panel (no icon). It intentionally WINS over the icon (it includes it). */
    if(e.text[0])
    {
        void* tex = MenuKit_RenderTextTexture(e.text, e.icon[0] ? e.icon : NULL, e);
        if(tex)
        {
            void** field = (void**)((uintptr_t)mem + 0x10);
            *field = tex;
            e.textTex = tex;
            logger->Info("MenuKit: widget '%s' label applied", e.texture);
        }
    }
    /* Native recipe sets +0xa4 = param4 = 1 already via the ctor; nothing more
       to patch. Log the widget identity. */
    e.widget = mem;
    s_pWidgetsPool[e.slot] = e.widget;
    logger->Info("MenuKit: widget '%s' built at slot %d", e.texture, e.slot);
}

/* Menu helpers ------------------------------------------------------------- */

static int MenuKit_CurrentMenu(void)
{
    return s_menuDepth > 0 ? s_menuStack[s_menuDepth - 1] : 0;
}

/* Free one of OUR widgets the same way the game does it.

   Native reference (libGTASA.so):
     - CTouchInterface::DeleteAll @0x36ed44 and DeleteWidget @0x371aa0 both do
       the SAME three steps on a pool slot: load m_pWidgets[slot]; if non-NULL,
       call the object's virtual destructor at [vtable+0x8]; store NULL back.
     - CTouchInterface::CreateShopWidget @0x3719fc frees the widget previously
       in its slot exactly that way BEFORE operator new'ing the replacement.

   So the game owns the per-widget free, and the canonical recipe is exactly
   "virtual dtor + NULL". We reproduce it here instead of calling DeleteWidget
   (which is a non-static method needing a CTouchInterface* we don't hold).

   Safe against double-free: callers verify s_pWidgetsPool[slot]==e->widget
   first, so we only destroy the object that is CURRENTLY in that slot; if the
   game already reclaimed it, the pointer differs (or the slot is NULL) and we
   leave it alone. This mirrors the game's own habit of checking the slot. */
static void MenuKit_FreeWidgetSlot(void* widget, int slot)
{
    if(!widget || !s_pWidgetsPool) return;
    if(slot < 0 || slot >= MAX_POOL_SLOTS) return;
    if(s_pWidgetsPool[slot] != widget) return;   /* not ours anymore */
    /* Virtual destructor: first two vtable slots are the deleting/complete
       object dtors; the game calls [vtable+0x8] (the complete-object dtor). */
    void** vtable = *(void***)widget;
    if(vtable && vtable[1])
        ((void(*)(void*))vtable[1])(widget);
    s_pWidgetsPool[slot] = NULL;
}

/* Free a menu's pool widgets so the game can reuse the slots (pool is
   ~172/190 used with menus open). Each widget object is destroyed via its
   virtual destructor, exactly as CTouchInterface::DeleteWidget does, so the
   slots are genuinely released instead of leaked. Entries keep their
   definition; the pump rebuilds the group at topmost free when active again. */
static void MenuKit_DestroyGroup(int menu)
{
    for(int i = 0; i < OUR_WIDGET_LIMIT; ++i)
    {
        WidgetEntry& e = s_widgets[i];
        if(!e.active || e.menu != menu) continue;
        MenuKit_FreeWidgetSlot(e.widget, e.slot);
        e.widget = NULL;
        e.slot = -1;
    }
}

static int MenuKit_CountGroup(int menu)
{
    int n = 0;
    for(int i = 0; i < OUR_WIDGET_LIMIT; ++i)
        if(s_widgets[i].active && s_widgets[i].menu == menu) ++n;
    return n;
}

static int MenuKit_FreeSlots(void)
{
    int n = 0;
    for(int s = 0; s < MAX_POOL_SLOTS; ++s) if(s_pWidgetsPool[s] == NULL) ++n;
    return n;
}

/* Push the DESIRED state of a widget onto the live object: custom rect
   (+0x2c..+0x38), alpha (+0x58), fade (+0x59) and the touch-immune bit in
   flags (+0x8c). Called from the setters for immediate effect AND from the
   pump on EVERY frame.

   Why every frame: the engine's own widget update runs INSIDE CGame_Process
   and restores alpha/fade/flags, so a one-shot write from a setter loses
   within a frame — measured, SetAlpha(128) read back as 255. Because the pump
   runs after orig, re-asserting here wins, and that single fact is what makes
   a dim trigger and a hide that survives playable at all. */
static void MenuKit_ApplyState(WidgetEntry* e)
{
    if(!e->widget) return;
    if(e->hasSize)
    {
        /* The rect at +0x2c is DERIVED state: the engine recomputes it from
           pos.w/pos.h every frame, and it does that AFTER this hook runs, so
           writing the rect here was silently discarded. Measured: SetSize was a
           complete no-op while its own log reported success (scale=20 widgets
           both measured 100x100, with and without SetSize(300,150)). pos is the
           source of truth, so pos is what we write.

           The factor is CALIBRATED once against the engine's own square instead
           of hardcoded, because the two axes do not share a scale: measured,
           pos.x is the centre at realW/640 per virtual unit while pos.w is at
           realW/320. Deriving it from the engine keeps it correct on any
           resolution, and works for non-square sizes that a hardcoded divisor
           would get wrong.

           Anchoring stays correct for free: pos.x is the centre, so widening
           pos.w grows the widget symmetrically - the documented "resize around
           the centre" behaviour, no separate rect maths needed. */
        const float* r = (const float*)((uintptr_t)e->widget + 0x2c);
        float* pos = (float*)((uintptr_t)e->widget + 0x18);   /* x, y, w, h */
        /* 1e6 sentinel: the engine has not laid this widget out yet, so its rect
           is not a measurement and calibrating from it would poison the factor
           for the widget's whole life. Skip ONLY the resize - alpha/fade/flags
           below are independent of the rect and must still land, or a
           not-yet-laid-out widget would keep the visible state the engine gave it. */
        if(r[0] <= 1e5f && r[1] <= 1e5f)
        {
            float rw = r[2] - r[0];       /* engine's real px width */
            float rh = r[1] - r[3];       /* r[1]=BOTTOM, r[3]=TOP */
            if(!e->sizeCal)
            {
                if(rw > 0.5f && pos[2] > 0.0f) e->pxPerUnitW = rw / pos[2];
                if(rh > 0.5f && pos[3] > 0.0f) e->pxPerUnitH = rh / pos[3];
                /* Only latch once BOTH axes have a usable factor, otherwise a
                   half-measured value would silently pin one axis wrong. */
                if(e->pxPerUnitW > 0.0f && e->pxPerUnitH > 0.0f) e->sizeCal = 1;
            }
            if(e->pxPerUnitW > 0.0f) pos[2] = e->sizeW / e->pxPerUnitW;
            if(e->pxPerUnitH > 0.0f) pos[3] = e->sizeH / e->pxPerUnitH;
        }
    }
    uint8_t*  pAlpha = (uint8_t*)((uintptr_t)e->widget + 0x58);
    uint8_t*  pFade  = (uint8_t*)((uintptr_t)e->widget + 0x59);
    uint32_t* pFlags = (uint32_t*)((uintptr_t)e->widget + 0x8c);
    if(e->visible)
    {
        if(*pAlpha != e->alpha) *pAlpha = e->alpha;
        if(*pFade != 1)          *pFade  = 1;
        if((*pFlags & 0x04u) == 0) *pFlags |= 0x04u;
    }
    else
    {
        if(*pAlpha != 0) *pAlpha = 0;
        if(*pFade != 0)  *pFade  = 0;
        /* No 0x04: CTouchInterface re-applies SetEnabled(w,0) and the widget
           stops eating taps as well as stop drawing. */
        if((*pFlags & 0x04u) != 0) *pFlags &= ~0x04u;
    }
}

/* v8 canvas flush.

   One indexed triangle list, submitted from the game's 2D pass. Render2dStuff is
   where the raster, camera and blend states an overlay needs are already set up.

   Z-order, measured on device rather than assumed: flushing after the pass put
   AML widget panels *under* client art, and moving the flush earlier - to the
   head of the pass, or to CTouchInterface::DrawAll inside it - changed nothing.
   Both still landed on top. So AML renders its own UI somewhere earlier in the
   frame, outside the game's 2D pass entirely, and this hook is the cheapest
   point that has a live raster.

   The first two attempts crashed the process and are deliberately not repeated:
   the raster is not mounted before the pass runs, so there is nowhere valid to
   draw from there.

   Consequence for clients: canvas art covers AML widgets in overlapping space.
   Draw a whole panel with the canvas rather than mixing the two. */
DECL_HOOKv(Render2dStuff, void)
{
    Render2dStuff(); /* orig */
    Canvas_Flush();
}

DECL_HOOKv(CGame_Process, void)
{
    CGame_Process(); /* orig */

    /* Latch the pointer BEFORE the client tick, so a client calling GetTap from
       its tick sees the tap that started this frame instead of the one that
       ended it. Rising edge (was up, now down) = one new tap. Falling edge only
       refreshes s_prevDown, so holding a finger is one tap, not 60. */
    if(s_pTouchDown && s_pTouchPos)
    {
        bool down = *s_pTouchDown;
        if(down && !s_prevDown)
        {
            s_tapX = s_pTouchPos[0];
            s_tapY = s_pTouchPos[1];
            s_tapThisFrame = 1;
            /* One-shot proof the pointer globals work and the units are sane.
               Bounded at a few so a real play session cannot spam the log. */
            if(s_tapCount < 3)
            {
                s_tapCount++;
                logger->Info("MenuKit: TAP#%d at (%g,%g) real px", s_tapCount, s_tapX, s_tapY);
            }
        }
        /* Drag proof for v9 GetPointer: while a finger is down and the cached
           point moves >4px from the last logged one, the engine is reporting a
           live position, not a frozen touchdown point. A slider needs exactly
           this. Bounded so holding still cannot spam. */
        if(down && s_dragLogCount < 8)
        {
            float dx = s_pTouchPos[0] - s_dragLastX;
            float dy = s_pTouchPos[1] - s_dragLastY;
            if(dx * dx + dy * dy > 16.0f)
            {
                s_dragLogCount++;
                s_dragLastX = s_pTouchPos[0];
                s_dragLastY = s_pTouchPos[1];
                logger->Info("MenuKit: DRAG#%d at (%g,%g) real px", s_dragLogCount, s_dragLastX, s_dragLastY);
            }
        }
        s_prevDown = down;
    }

    /* Client timer tick. BEFORE the pool check: a client counting down an
       auto-hide should not lose frames just because the pool is late, and
       Add/Remove are deferred so they are safe either way.

       Canvas_Clear runs first so the tick rebuilds this frame's geometry from
       scratch: the batch is per-frame, not a persistent scene graph. */
    Canvas_Clear();
    if(s_tick) s_tick(s_tickUser);

    s_tapThisFrame = 0;   /* one-frame lifetime, consumed by whoever reads it */

    if(!s_pWidgetsPool) return;
    for(int i = 0; i < OUR_WIDGET_LIMIT; ++i)
    {
        WidgetEntry& e = s_widgets[i];
        /* Re-evaluate the active menu on EVERY entry: release callbacks
           (OpenMenu/CloseMenu) mutate the stack mid-loop. A value captured at
           loop entry would rebuild widgets of the just-closed menu in the very
           same frame — leaving a ghost button drawn stacked on top of the root
           (user-visible as a "duplicated shoot button") — and would tie the
           opening group's build to stale slots. */
        if(!e.active || e.menu != MenuKit_CurrentMenu()) continue;

        /* Game rebuilds the pool (menu open/close, entering a vehicle, etc.):
           it deletes pool widgets and re-creates its own. Re-inject ours when
           missing; migrate slot if taken. Also covers widgets registered during
           ON_MOD_LOAD: those must wait for the first frame because at load time
           the game's TextureDatabaseRuntime doesn't exist yet and constructing
           CWidgetButton would crash. */
        if(e.widget == NULL || e.slot < 0 || e.slot >= MAX_POOL_SLOTS || s_pWidgetsPool[e.slot] != e.widget)
        {
            /* Need (re)assignment: a deferred registration waiting for the
               engine (slot=-1), the game rebuilt the pool and freed ours,
               or the game took our slot for its own widget. Reclaim the
               TOPMOST free slot after the game's own build pass. m_pWidgets
               is one contiguous pool; the game pre-creates most of it (observed
               172/190 with menus open, native attack button at 187), so the top
               of the free range is the safe zone — the vehicle-mode dump below
               verifies which slots the game's vehicle buttons actually use. */
            int newSlot = -1;
            for(int s = MAX_POOL_SLOTS - 1; s >= 0; --s)
                if(s_pWidgetsPool[s] == NULL) { newSlot = s; break; }
            if(newSlot < 0) { e.wasTouched = false; continue; } /* pool full */
            e.slot = newSlot;
            if(s_pWidgetsPool[e.slot] == NULL)
            {
                MenuKit_BuildWidget(e);
                e.wasTouched = false; /* freshly built, not touched yet */
            }
        }

        /* Our state wins this frame: rect, alpha, fade, flags. Must come after
           the (re)build above, since a fresh widget needs the rect applied. */
        /* Re-protect the custom icon: two distinct steals — (1) the game
           overwrites sprite[0] when it rebuilds pool widgets (menu open/close,
           entering a vehicle, pausing...); (2) the game's streaming can recycle
           the texture BLOCK behind pointer equality (slot keeps our tex addr,
           renders a game texture). LoadIcon re-checks its liveness fingerprint
           every call and reloads fresh when stolen; if that yielded a new
           pointer, swap it into the widget too. O(1) per frame per icon widget;
           only rewrites when the game actually stole something. */
        if(e.widget && e.icon[0])
        {
            void* want = MenuKit_LoadIcon(e.icon);
            if(want != e.iconTex)
            {
                if(e.iconTex)
                    logger->Info("MenuKit: icon refreshed on '%s' (slot %d): %p -> %p", e.texture, e.slot, e.iconTex, want);
                e.iconTex = want;
            }
            if(e.iconTex)
            {
                void** field = (void**)((uintptr_t)e.widget + 0x10);
                if(*field != e.iconTex)
                {
                    *field = e.iconTex;
                    logger->Info("MenuKit: icon re-protected on '%s' (slot %d)", e.texture, e.slot);
                }
            }
        }

        /* Plan B: re-protect the label exactly like the icon. RenderTextTexture
           is cached (key = texture|text) and liveness-checked (member pointer
           vs stored raster) inside, so this is cheap when nothing was stolen;
           when the game recycled the block it re-renders and we swap the new
           texture in. Also covers SetText() called at runtime: the new text is
           marked textTex=NULL (force) and this block rebuilds it on the next
           frame. The label always wins sprite[0] over the icon (it includes
           the icon pixels when one is set). */
        if(e.widget && e.text[0])
        {
            void* want = MenuKit_RenderTextTexture(e.text, e.icon[0] ? e.icon : NULL, e);
            if(want != e.textTex)
            {
                if(e.textTex)
                    logger->Info("MenuKit: label refreshed on '%s': %p -> %p", e.texture, e.textTex, want);
                e.textTex = want;
            }
            if(e.textTex)
            {
                void** field = (void**)((uintptr_t)e.widget + 0x10);
                if(*field != e.textTex)
                {
                    *field = e.textTex;
                    logger->Info("MenuKit: label re-protected on '%s' (slot %d)", e.texture, e.slot);
                }
            }
        }

        /* Dispatch: release = falling edge of IsTouched. IsTouched is a LEVEL
           signal (true while the finger is down) — the game itself drives our
           press animation off this same per-widget query (vtable+0xa0).
           CWidget::IsReleased can't fire for ctor-built widgets: it needs a CHID
           release edge on hid=1 (consumed by the game's native fire polling) or
           m_pReleasedWidget[+0x84]==widget, and +0x84 is 0 for ctor-built
           widgets so that slot-0 record never matches ours. */
        bool touched = s_pfnIsTouched(e.slot, NULL, 1);
        if(e.wasTouched && !touched && e.onRelease) e.onRelease(e.userdata);
        e.wasTouched = touched;

        /* Our state wins the frame: rect, alpha, fade, flags. LAST on purpose.
           IsTouched above is a real game call that runs the widget's own
           per-frame update, which reset fade to 1 right after an earlier
           placement of this call had set it to 0 - measured. Written after it,
           the desired state survives to the draw. */
        MenuKit_ApplyState(&e);
    }

}

static bool MenuKit_Init(IAML* aml)
{
    if(!aml) return false;
    uintptr_t pGameHandle = aml->GetLib("libGTASA.so");
    if(!pGameHandle) { logger->Error("MenuKit: libGTASA.so not loaded"); return false; }

    s_pWidgetsPool    = (void**)aml->GetSym(pGameHandle, "_ZN15CTouchInterface10m_pWidgetsE");
    s_pfnCtorButton   = (void*)aml->GetSym(pGameHandle, "_ZN13CWidgetButtonC2EPKcRK14WidgetPositionjj10HIDMapping");
    s_pfnIsReleased   = (bool(*)(int,void*,int))aml->GetSym(pGameHandle, "_ZN15CTouchInterface10IsReleasedENS_9WidgetIDsEP9CVector2Di");
    s_pfnIsTouched    = (bool(*)(int,void*,int))aml->GetSym(pGameHandle, "_ZN15CTouchInterface9IsTouchedENS_9WidgetIDsEP9CVector2Di");

    /* Global pointer state. Static data members, so GetSym hands back the
       ADDRESS of the member: bool* and float* {x,y} respectively, not a copy. */
    s_pTouchDown = (bool*)aml->GetSym(pGameHandle, "_ZN15CTouchInterface12m_bTouchDownE");
    s_pTouchPos  = (float*)aml->GetSym(pGameHandle, "_ZN15CTouchInterface14m_vecCachedPosE");
    if(s_pTouchDown && s_pTouchPos)
        logger->Info("MenuKit: global pointer state resolved (m_bTouchDown=%p m_vecCachedPos=%p)",
                     s_pTouchDown, s_pTouchPos);
    else
        logger->Error("MenuKit: global pointer state NOT resolved (down=%p pos=%p) - "
                      "'tap outside' will not work, everything else will", s_pTouchDown, s_pTouchPos);

    /* The game's own menu singleton. GetMenuUp() reads its screen-stack fields
       so a client can tell "the game has a menu up" from "the player is
       driving". Optional: without it the client just never sees menu state. */
    s_gMobileMenu = (void*)aml->GetSym(pGameHandle, "gMobileMenu");
    logger->Info("MenuKit: gMobileMenu = %p%s", s_gMobileMenu,
                 s_gMobileMenu ? " (GetMenuUp available)" : " (GetMenuUp unavailable)");

    /* RenderWare symbol resolution for custom icons (non-fatal if missing). */
    s_pfnRwImageCreate           = (void*(*)(int,int,int))aml->GetSym(pGameHandle, "_Z13RwImageCreateiii");
    s_pfnRwImageDestroy          = (void(*)(void*))aml->GetSym(pGameHandle, "_Z14RwImageDestroyP7RwImage");
    s_pfnRwImageFindRasterFormat = (int(*)(void*,int,int*,int*,int*,int*))aml->GetSym(pGameHandle, "_Z23RwImageFindRasterFormatP7RwImageiPiS1_S1_S1_");
    s_pfnRwRasterCreate          = (void*(*)(int,int,int,int))aml->GetSym(pGameHandle, "_Z14RwRasterCreateiiii");
    s_pfnRwRasterDestroy         = (void(*)(void*))aml->GetSym(pGameHandle, "_Z15RwRasterDestroyP8RwRaster");
    s_pfnRwRasterSetFromImage    = (int(*)(void*,void*))aml->GetSym(pGameHandle, "_Z20RwRasterSetFromImageP8RwRasterP7RwImage");
    s_pfnRwTextureCreate         = (void*(*)(void*))aml->GetSym(pGameHandle, "_Z15RwTextureCreateP8RwRaster");
    s_pfnRwTextureDestroy        = (void(*)(void*))aml->GetSym(pGameHandle, "_Z16RwTextureDestroyP9RwTexture");
    s_pfnRwTextureSetName        = (void(*)(void*,const char*))aml->GetSym(pGameHandle, "_Z16RwTextureSetNameP9RwTexturePKc");
    /* Engine pixel accessors: this fork's RwRaster is GL-backed (diag: dims are
       u32 @+0x18/+0x1c, NO CPU pixel pointer in the 0x40-byte header). Content
       liveness must go through the ENGINE, never a struct offset read as a
       pointer (that SEGV'd at +0x23724). Non-fatal: NULL accessors degrade
       liveness to the dims gate. */
    /* RwRasterGetPixels is a MACRO in stock RW (never exported; GetSym->NULL is
       expected and the dims-gate fallback covers it). RwRasterLock in this fork
       is a 3-arg export _Z12RwRasterLockP8RwRasterhi = (RwRaster*, uchar, int):
       engine calls RwRasterLock(raster, 0, 1) to obtain a readable RwRGBA*
       (CPostEffects) — h is a reserved/dirty arg (always 0), i is the mode
       (1=read). */
    s_pfnRwRasterGetPixels       = (void*(*)(void*))aml->GetSym(pGameHandle, "_Z17RwRasterGetPixelsP8RwRaster");
    s_pfnRwRasterLock            = (void*(*)(void*,unsigned char,int))aml->GetSym(pGameHandle, "_Z12RwRasterLockP8RwRasterhi");
    s_pfnRwRasterUnlock          = (void(*)(void*))aml->GetSym(pGameHandle, "_Z14RwRasterUnlockP8RwRaster");

    /* Real screen size for the label panel projection (MenuVSL proves these
       exports exist: _Z17OS_ScreenGetWidthv / _Z18OS_ScreenGetHeightv). */
    s_pfnOS_ScreenGetWidth       = (int(*)(void))aml->GetSym(pGameHandle, "_Z17OS_ScreenGetWidthv");
    s_pfnOS_ScreenGetHeight      = (int(*)(void))aml->GetSym(pGameHandle, "_Z18OS_ScreenGetHeightv");
    if(s_pfnOS_ScreenGetWidth && s_pfnOS_ScreenGetHeight)
    {
        int sw = s_pfnOS_ScreenGetWidth(), sh = s_pfnOS_ScreenGetHeight();
        if(sw > 0 && sh > 0) { s_screenW = (float)sw; s_screenH = (float)sh; }
        logger->Info("MenuKit: screen %dx%d (real px)", (int)s_screenW, (int)s_screenH);
    }
    else
    {
        logger->Error("MenuKit: OS_ScreenGetWidth/Height not resolved - label panels use 640x448 fallback");
    }
    logger->Info("MenuKit: pixel accessors GetPixels=%p Lock=%p Unlock=%p",
                 s_pfnRwRasterGetPixels, s_pfnRwRasterLock, s_pfnRwRasterUnlock);

    if(!s_pWidgetsPool || !s_pfnCtorButton || !s_pfnIsReleased || !s_pfnIsTouched)
    {
        logger->Error("MenuKit: failed to resolve widget symbols (pool=%p ctor=%p rel=%p touch=%p)",
                      s_pWidgetsPool, s_pfnCtorButton, s_pfnIsReleased, s_pfnIsTouched);
        s_pWidgetsPool = NULL;
        return false;
    }

    if(!s_pfnRwImageCreate || !s_pfnRwImageDestroy || !s_pfnRwImageFindRasterFormat ||
       !s_pfnRwRasterCreate || !s_pfnRwRasterSetFromImage || !s_pfnRwTextureCreate)
    {
        logger->Error("MenuKit: RenderWare symbols missing - custom button icons disabled");
    }
    else if(!s_pfnRwTextureSetName)
    {
        /* Cosmetic: icon textures stay unnamed (games's DB lookups by name
           would miss them). Rendering is unaffected — Draw uses the pointer. */
        logger->Error("MenuKit: RwTextureSetName not resolved - icon textures unnamed");
    }

    aml->Hook((void*)aml->GetSym(pGameHandle, "_ZN5CGame7ProcessEv"),
              (void*)&HookOf_CGame_Process, (void**)&CGame_Process);

    /* v8 canvas: the 2D immediate pipeline. Optional - if it is missing the
       draw calls become no-ops and the retained widget API still works, so a
       mismatch degrades instead of breaking every client. */
    s_pfnRwRenderStateSet = (int(*)(int, void*))aml->GetSym(pGameHandle,
        "_Z16RwRenderStateSet13RwRenderStatePv");
    s_pfnRwIm2DRenderIndexedPrimitive = (int(*)(int, RwIm2DVertex*, int, RwImVertexIndex*, int))
        aml->GetSym(pGameHandle,
        "_Z28RwIm2DRenderIndexedPrimitive15RwPrimitiveTypeP14RwOpenGLVertexiPti");

    if(!s_pfnRwRenderStateSet || !s_pfnRwIm2DRenderIndexedPrimitive)
    {
        s_pfnRwRenderStateSet = NULL;
        s_pfnRwIm2DRenderIndexedPrimitive = NULL;
        logger->Error("MenuKit: Rw 2D symbols missing - canvas disabled (API v8 draw calls will no-op)");
    }
    else
    {
        aml->Hook((void*)aml->GetSym(pGameHandle, "_Z13Render2dStuffv"),
                  (void*)&HookOf_Render2dStuff, (void**)&Render2dStuff);
        logger->Info("MenuKit: canvas ready (API v%d)", MENUKIT_API_VERSION);
    }

    /* self-check: pool pointer sane, slots reservable */
    int freeSlots = 0;
    for(int s = 0; s < MAX_POOL_SLOTS; ++s) if(s_pWidgetsPool[s] == NULL) ++freeSlots;
    if(freeSlots == 0) logger->Error("MenuKit: widget pool is full (%d slots)", MAX_POOL_SLOTS);
    else logger->Info("MenuKit: init ok, %d/%d pool slots free", freeSlots, MAX_POOL_SLOTS);
    return true;
}

/* ---- API impl (exported by main.cpp's GetMenuAPI) ---- */
static void* MenuKit_AddButton(int menu, const char* texture, float x, float y, float scale,
                               MenuKit_OnReleaseCallback onRelease, void* userdata,
                               const char* icon)
{
    WidgetEntry* e = NULL;
    for(int i = 0; i < OUR_WIDGET_LIMIT; ++i)
        if(!s_widgets[i].active) { e = &s_widgets[i]; break; }
    if(!e) { logger->Error("MenuKit: widget limit reached (%d)", OUR_WIDGET_LIMIT); return NULL; }

    /* Deferred-only registration (slot=-1). Two reasons: (1) AML calls
       ON_MOD_LOAD in mod-directory order, NOT dependency order — a client can
       run before our init resolves the pool symbols; (2) a button may belong
       to a menu that is not active — building it now would put it in the pool
       and the game would draw an unopened menu. Registration never touches the
       pool; the CGame_Process pump is the single place that assigns a slot and
       builds, and it only does so for the ACTIVE menu. */
    memset(e->texture, 0, sizeof(e->texture));
    if(texture) strncpy(e->texture, texture, sizeof(e->texture) - 1);
    e->texture[sizeof(e->texture) - 1] = '\0';
    e->pos = WidgetPosition{ x, y, scale, scale };
    e->menu = menu;
    e->active = true;
    e->wasTouched = false;
    e->slot = -1;
    e->widget = NULL;  /* built lazily on the first CGame_Process frame */
    e->alpha = 0xFF;   /* opaque unless SetAlpha says otherwise before the build */
    e->visible = 1;    /* shown unless SetVisible parks it before the build */
    e->hasSize = 0;    /* engine keeps its square size until SetSize is called */
    e->sizeCal = 0;    /* pxPerUnit* measured lazily on the first laid-out frame */
    e->pxPerUnitW = 0.0f;
    e->pxPerUnitH = 0.0f;
    e->onRelease = onRelease;
    e->userdata = userdata;
    memset(e->icon, 0, sizeof(e->icon));
    if(icon) strncpy(e->icon, icon, sizeof(e->icon) - 1);
    e->icon[sizeof(e->icon) - 1] = '\0';
    logger->Info("MenuKit: widget '%s' registered (menu %d, deferred build)%s",
                 e->texture, menu, e->icon[0] ? " [custom icon]" : "");
    return (void*)e;
}

static void MenuKit_RemoveWidget(void* handle)
{
    if(!handle || !s_pWidgetsPool) return;
    WidgetEntry* e = (WidgetEntry*)handle;
    /* Destroy the object like the game would (virtual dtor + NULL slot), then
       clear the entry. Safe: FreeWidgetSlot re-checks the slot holds OUR widget
       before destroying, so a game-reclaimed slot is never double-freed. */
    MenuKit_FreeWidgetSlot(e->widget, e->slot);
    *e = WidgetEntry{};
    logger->Info("MenuKit: widget removed");
}

static bool MenuKit_IsReleased(void* handle)
{
    if(!handle || !s_pWidgetsPool || !s_pfnIsReleased) return false;
    WidgetEntry* e = (WidgetEntry*)handle;
    if(!e->active || !e->widget || e->slot < 0 || e->slot >= MAX_POOL_SLOTS) return false;
    return s_pfnIsReleased(e->slot, NULL, 1);
}

/* Open menu: destroy the current group's pool widgets, push the menu, let the
   pump rebuild the new group at topmost free on the next frame. Refuses to
   open when the group cannot fully fit in the pool (approved fallback: log +
   don't open, instead of a half-drawn menu). */
static int MenuKit_OpenMenu(int menu)
{
    if(menu <= 0) { logger->Error("MenuKit: OpenMenu(%d): invalid id (root is 0, menus are 1+)", menu); return -1; }
    if(s_menuDepth >= MENU_STACK_MAX) { logger->Error("MenuKit: OpenMenu(%d): stack full (%d)", menu, MENU_STACK_MAX); return -1; }
    for(int i = 0; i < s_menuDepth; ++i)
        if(s_menuStack[i] == menu) { logger->Error("MenuKit: OpenMenu(%d): already open", menu); return -1; }
    if(!s_pWidgetsPool || !s_pfnCtorButton) { logger->Error("MenuKit: OpenMenu(%d): engine not ready", menu); return -1; }

    int need = MenuKit_CountGroup(menu);
    int fits = MenuKit_FreeSlots() + MenuKit_CountGroup(MenuKit_CurrentMenu()); /* freed by the switch */
    if(need > fits)
    {
        logger->Error("MenuKit: OpenMenu(%d): needs %d slots, only %d available — menu not opened",
                      menu, need, fits);
        return -1;
    }

    MenuKit_DestroyGroup(MenuKit_CurrentMenu());
    s_menuStack[s_menuDepth++] = menu;
    logger->Info("MenuKit: menu %d open (depth %d, %d widgets)", menu, s_menuDepth, need);
    return 0;
}

static int MenuKit_CloseMenu(void)
{
    if(s_menuDepth == 0) { logger->Info("MenuKit: CloseMenu: nothing to close"); return -1; }
    int closing = s_menuStack[--s_menuDepth];
    MenuKit_DestroyGroup(closing);
    logger->Info("MenuKit: menu %d closed (depth %d, %s rebuilt next frame)",
                 closing, s_menuDepth,
                 s_menuDepth > 0 ? "previous menu" : "root");
    return 0;
}

/* Plan B (v4): SetText. Changes the label of an existing widget (POST-build
   or deferred — both work). Safe before the widget exists: the pump renders
   the label from e.text in the same re-protect pass that builds widgets, so
   this only flips a string buffer and lets the deterministic next-frame pump
   do the RenderWare work. textTex=NULL forces a re-render of the cached
   label (the texture carries the old text, so the cache key must change). */
static void MenuKit_SetText(void* handle, const char* text)
{
    if(!handle) return;
    WidgetEntry* e = (WidgetEntry*)handle;
    memset(e->text, 0, sizeof(e->text));
    if(text) strncpy(e->text, text, sizeof(e->text) - 1);
    e->text[sizeof(e->text) - 1] = '\0';
    e->textTex = NULL;  /* force rebuild; old texture may be game-owned now */
    logger->Info("MenuKit: widget '%s' text set to '%s'", e->texture, e->text[0] ? e->text : "(none)");
}

/* v5: register the client's per-frame callback (NULL clears it). Invoked from
   the CGame_Process pump, so it runs once per rendered frame. */
static void MenuKit_SetTick(MenuKit_TickCallback onTick, void* userdata)
{
    s_tick = onTick;
    s_tickUser = userdata;
    logger->Info("MenuKit: client tick %s", onTick ? "registered" : "cleared");
}

/* v5: alpha 0..255. Applied at build (replacing the old hardcoded 0xFF) AND,
   when the widget is already live, written straight into its alpha byte so a
   dim/bright toggle needs no remove+rebuild — which matters because the client
   never frees widget objects itself, so every rebuild would leak one. */
static void MenuKit_SetAlpha(void* handle, unsigned char alpha)
{
    if(!handle) return;
    WidgetEntry* e = (WidgetEntry*)handle;
    e->alpha = alpha;
    MenuKit_ApplyState(e);   /* pump re-asserts it every frame, see ApplyState */
    logger->Info("MenuKit: widget '%s' alpha set to %u%s", e->texture, (unsigned)alpha,
                 e->widget ? "" : " (pending build)");
}

/* v5: show/hide. Hiding clears the touch-immune flag and zeroes the alpha, so
   CTouchInterface re-applies SetEnabled(w,0) every frame and the widget stops
   eating taps as well as stop drawing. */
static void MenuKit_SetVisible(void* handle, int visible)
{
    if(!handle) return;
    WidgetEntry* e = (WidgetEntry*)handle;
    int on = visible != 0;
    e->visible = (uint8_t)on;
    MenuKit_ApplyState(e);   /* pump re-asserts it every frame, see ApplyState */
    logger->Info("MenuKit: widget '%s' %s", e->texture, on ? "shown" : "parked");
}

/* v6: resize a widget in REAL pixels, keeping the centre the engine computed.

   This is what breaks the "everything is a square" limit. The engine derives
   both axes from ONE uniform `scale` (one number for width and height), so
   every widget AddButton builds is square. The four floats the engine already
   keeps in the object (+0x2c..+0x38) are just as writable, so decoupling them
   lets a client express panels, bars and banners.

   Deliberately SIZE-ONLY, not x/y/w/h. An earlier absolute-rect version needed
   the true screen size, and OS_ScreenGetWidth() is not that: it reported
   1024x600 while the real render is 1600x720, so absolute coordinates landed
   the widget in the middle of the screen. Resizing around the engine's own
   centre sidesteps the question entirely - the virtual-unit placement the
   engine already does is correct and device-independent, so the anchor keeps
   working and only the shape changes.

   Re-asserted every frame by the pump (see MenuKit_ApplyState), so the engine
   cannot take the size back. */
static void MenuKit_SetSize(void* handle, float w, float h)
{
    if(!handle) return;
    WidgetEntry* e = (WidgetEntry*)handle;
    if(w <= 0.0f || h <= 0.0f)
    {
        logger->Info("MenuKit: SetSize('%s') ignored, bad size %gx%g", e->texture, w, h);
        return;
    }
    e->hasSize = 1;
    e->sizeW = w;
    e->sizeH = h;
    MenuKit_ApplyState(e);
    logger->Info("MenuKit: widget '%s' size set to %gx%g real px%s",
                 e->texture, w, h, e->widget ? "" : " (pending build)");
}

/* v7: where the player tapped, anywhere on screen.

   AML has no touch/pad API, so a client could only ever react to touches landing
   on one of its OWN widgets - and the usual workaround, a full-screen catcher
   widget, silently eats the game's joystick and fire button. Reading the game's
   own pointer globals removes the limitation instead of working around it.

   Returns 1 on the frame a NEW tap begins (rising edge) and writes the position
   in the same real-pixel space as GetRect, so a client hit-tests with two
   numbers and no coordinate conversion. Holding a finger is one tap, not one
   per frame. Returns 0 if the pointer globals were not resolved, or if the tap
   was not this frame. */
static int MenuKit_GetTap(float* x, float* y)
{
    if(!s_tapThisFrame) return 0;
    if(x) *x = s_tapX;
    if(y) *y = s_tapY;
    return 1;
}

/* v9: live pointer state, polled every frame. Reads the SAME engine statics
   GetTap latches from, but without the rising-edge/one-frame gate, so a client
   can follow a finger across the screen instead of only seeing where it landed.
   That is the whole difference between a tap and a drag, and a slider, a swipe
   or a scroll list are all drags.

   The outs are left untouched on failure so a caller that forgot to check the
   return value keeps its previous value instead of reading uninitialised
   stack. */
static int MenuKit_GetPointer(float* x, float* y, int* down)
{
    if(!s_pTouchDown || !s_pTouchPos) return 0;
    if(x)    *x    = s_pTouchPos[0];
    if(y)    *y    = s_pTouchPos[1];
    if(down) *down = *s_pTouchDown ? 1 : 0;
    return 1;
}

/* v11: is a game menu on screen right now? 1 = yes, 0 = no, -1 = unknown.

   Reads gMobileMenu's own screen-stack fields with the engine's own gate:
   MobileMenu::Update top-tests `(+0x24 != 0) || (+0x30 != NULL)` and returns
   immediately when that is false, so "no screen on the stack" is exactly what
   the engine itself calls an idle menu. A client that draws on top of gameplay
   needs this to know when its own input must stand down - a menu is open, the
   finger belongs to the game, not to us. */
static int MenuKit_GetMenuUp(void)
{
    if(!s_gMobileMenu) return -1;
    const int  depth   = *(const int*)((const char*)s_gMobileMenu + 0x24);
    const void* current = *(void* const*)((const char*)s_gMobileMenu + 0x30);
    return (depth != 0 || current != NULL) ? 1 : 0;
}

/* v7: live on-screen rect of a widget in real pixels (l, t, r, b). The engine
   already keeps these four floats in every widget object, and a client needs
   them for hit-testing, anchoring and tooltips. Pairs with GetTap: both are in
   the same space, so `tap inside panel` is a four-comparison test with no
   knowledge of the engine's virtual 640x448 projection.

   Returns 0 for an unknown or not-yet-built handle, or if the engine has not
   activated the widget yet (its rect is a 1e6 sentinel until then, observed in
   the pool dump) - so a false 0 means "ask again next frame", never a garbage
   rect. */
static int MenuKit_GetRect(void* handle, float* l, float* t, float* r, float* b)
{
    if(!handle) return 0;
    WidgetEntry* e = (WidgetEntry*)handle;
    if(!e->widget) return 0;
    const float* q = (const float*)((uintptr_t)e->widget + 0x2c);
    /* 1e6 sentinel: the engine has not laid this widget out yet. */
    if(q[0] > 1e5f || q[1] > 1e5f) return 0;
    /* The engine stores the rect bottom-up: q = (left, BOTTOM, right, TOP), so
       q[1] > q[3] for a real on-screen rect (verified in the pool dump, e.g.
       (1461.5, 701.9, 1563.5, 599.9)). Export it TOP-DOWN (top < bottom) so a
       client's `y >= top && y <= bottom` hit-test is satisfiable. Without this
       swap the test is impossible (top > bottom) and every tap reads as
       "outside" -> the panel closed on every touch. */
    float top = q[3], bot = q[1];
    if(top > bot) { float tmp = top; top = bot; bot = tmp; }
    if(l) *l = q[0];
    if(t) *t = top;
    if(r) *r = q[2];
    if(b) *b = bot;
    return 1;
}

/* ---------------------------------------------------------------------------
   v8: immediate-mode 2D canvas. Real pixels, same space as GetRect/GetTap.
   Call these from SetTick; the frame's geometry is flushed as one batch by the
   Render2dStuff hook. Not thread-safe and not persistent - redraw every frame.
   ------------------------------------------------------------------------ */

/* rgba is 0xRRGGBBAA so a client can shift/ease a channel without bit-fiddling.
   These return void: drawing is fire-and-forget, and a full batch degrades to
   dropping primitives (counted and logged) rather than failing the client. */
static void MenuKit_DrawRect(float x, float y, float w, float h, uint32_t rgba, int filled)
{
    if(w <= 0.0f || h <= 0.0f) return;
    float xy[8] = { x, y,  x+w, y,  x+w, y+h,  x, y+h };
    if(filled) { float q[6] = { x, y, x+w, y, x+w, y+h }; Canvas_Tri(q, rgba);
                 float r[6] = { x, y, x+w, y+h, x, y+h }; Canvas_Tri(r, rgba); }
    else       { Canvas_Stroke(xy, 4, rgba, 1.0f, true); }
}

static void MenuKit_DrawQuad(float x1, float y1, float x2, float y2, float x3, float y3,
                             float x4, float y4, uint32_t rgba, int filled)
{
    if(filled) {
        const float a[6] = { x1, y1, x2, y2, x3, y3 };
        const float b[6] = { x1, y1, x3, y3, x4, y4 };
        Canvas_Tri(a, rgba);
        Canvas_Tri(b, rgba);
    } else {
        const float xy[8] = { x1, y1, x2, y2, x3, y3, x4, y4 };
        Canvas_Stroke(xy, 4, rgba, 1.0f, true);
    }
}

static void MenuKit_DrawTriangle(float x1, float y1, float x2, float y2, float x3, float y3,
                                 uint32_t rgba, int filled)
{
    if(filled) { const float t[6] = { x1, y1, x2, y2, x3, y3 }; Canvas_Tri(t, rgba); }
    else {
        const float xy[6] = { x1, y1, x2, y2, x3, y3 };
        Canvas_Stroke(xy, 3, rgba, 1.0f, true);
    }
}

static void MenuKit_DrawLine(float x1, float y1, float x2, float y2, uint32_t rgba, float width)
{
    if(width <= 0.0f) return;
    /* Una linea SIEMPRE se rasteriza como un quad (2 triangulos, 6 indices).
       Existia un atajo de 2 indices sueltos que estaba roto: el lote se dibuja
       como RW_PRIMTYPE_TRI_LIST, que consume de 3 en 3, de modo que un par
       suelto desalineaba todas las primitivas siguientes y una linea al final
       del lote se comia indices de mas. Todo emisor del batch debe emitir
       multiplos de 3. */
    if(width < 1.0f) width = 1.0f;   /* el raster no hace sub-pixel */
    const float dx = x2 - x1, dy = y2 - y1;
    const float len = dx * dx + dy * dy;
    if(len < 1e-6f) return;
    const float nx = -dy * width * 0.5f / (float)sqrt(len);
    const float ny =  dx * width * 0.5f / (float)sqrt(len);
    const float a[6] = { x1+nx, y1+ny, x1-nx, y1-ny, x2-nx, y2-ny };
    const float b[6] = { x1+nx, y1+ny, x2-nx, y2-ny, x2+nx, y2+ny };
    Canvas_Tri(a, rgba);
    Canvas_Tri(b, rgba);
}

static void MenuKit_DrawPoly(const float* xy, int count, uint32_t rgba, int filled, int closed)
{
    if(!xy || count < 3) return;
    if(filled) Canvas_Fan(xy, count, rgba);
    else       Canvas_Stroke(xy, count, rgba, 1.0f, closed != 0);
}

static void MenuKit_DrawCircle(float cx, float cy, float r, uint32_t rgba, int filled, int segments)
{
    if(r <= 0.0f) return;
    if(segments < 3) segments = 3;
    if(segments > 128) segments = 128;   /* matches the canvas batch budget */
    float xy[129 * 2];
    for(int i = 0; i < segments; ++i) {
        const float a = (float)(i * 2.0 * 3.14159265358979323846 / segments);
        xy[i * 2]     = cx + r * (float)cos(a);
        xy[i * 2 + 1] = cy + r * (float)sin(a);
    }
    if(filled) Canvas_Fan(xy, segments, rgba);
    else       Canvas_Stroke(xy, segments, rgba, 1.0f, true);
}

/* Text goes into the shape batch as untextured quads, one per horizontal run of
   lit font pixels. The immediate-mode textured path in this GL fork binds a raster
   we build but never samples our pixels, while the shape batch is proven to
   render on device - so reuse the path that works instead of guessing at
   RenderWare internals. Runs rather than single pixels keep the count low (a 5x7
   glyph is ~14 quads). Ordering comes for free: shapes and text share one batch,
   so a panel drawn after its label still covers it. */
static void MenuKit_DrawText(float x, float y, const char* text, int scale, uint32_t rgba)
{
    if(!text || !*text) return;
    if(scale < 1) scale = 1;
    if(scale > 16) scale = 16;
    if(((rgba >> 24) & 0xFF) == 0) return;   /* fully transparent */

    const size_t len = strlen(text);
    if(len > 200) return;

    for(size_t i = 0; i < len; ++i)
    {
        unsigned char c = (unsigned char)text[i];
        if(c < 0x20 || c > 0x7e) c = '?';
        const unsigned char* glyph = s_font5x7[c - 0x20];
        for(int row = 0; row < 7; ++row)
        {
            int col = 0;
            while(col < 5)
            {
                while(col < 5 && !((glyph[col] >> row) & 1)) ++col;
                if(col >= 5) break;
                const int start = col;
                while(col < 5 && ((glyph[col] >> row) & 1)) ++col;

                const float rx = x + (float)(i * 6 * scale + start * scale);
                const float ry = y + (float)(row * scale);
                const float rw = (float)((col - start) * scale);
                const float rh = (float)scale;
                const float q0[6] = { rx,    ry,    rx+rw, ry,    rx+rw, ry+rh };
                const float q1[6] = { rx,    ry,    rx,    ry+rh, rx+rw, ry+rh };
                Canvas_Tri(q0, rgba);
                Canvas_Tri(q1, rgba);
            }
        }
    }
}

static void MenuKit_SetDrawBlend(int src, int dst)
{
    s_canvasSrcBlend = (src < 0) ? RW_BLEND_SRC_ALPHA : src;
    s_canvasDstBlend = (dst < 0) ? RW_BLEND_INV_SRC_ALPHA : dst;
}

static void MenuKit_ResetDrawState(void)
{
    s_canvasSrcBlend = RW_BLEND_SRC_ALPHA;
    s_canvasDstBlend = RW_BLEND_INV_SRC_ALPHA;
}

static MenuKitAPI g_api = {
    MENUKIT_API_VERSION,
    MenuKit_AddButton,
    MenuKit_RemoveWidget,
    MenuKit_IsReleased,
    MenuKit_OpenMenu,
    MenuKit_CloseMenu,
    MenuKit_SetText,
    MenuKit_SetTick,
    MenuKit_SetAlpha,
    MenuKit_SetVisible,
    MenuKit_SetSize,
    MenuKit_GetTap,
    MenuKit_GetRect,
    MenuKit_DrawRect,
    MenuKit_DrawQuad,
    MenuKit_DrawTriangle,
    MenuKit_DrawLine,
    MenuKit_DrawPoly,
    MenuKit_DrawCircle,
    MenuKit_SetDrawBlend,
    MenuKit_ResetDrawState,
    MenuKit_GetPointer,
    MenuKit_DrawText,
    MenuKit_GetMenuUp
};

const MenuKitAPI* GetMenuAPI(void) { return &g_api; }

/* Dispose every widget this framework still owns, then drop all state. Called
   from the host mod's ON_MOD_UNLOAD via the framework main.cpp. Each live
   object is destroyed through its virtual destructor (the game's own recipe),
   so this is a real free, not a pool-slot NULL: no 0x200-per-widget leak on
   reload. FreeWidgetSlot re-checks the slot still holds OUR widget, so if the
   game already reclaimed one we skip it instead of double-freeing. */
extern "C" __attribute__((visibility("default")))
void MenuKit_Shutdown(void)
{
    int freed = 0;
    if(!s_pWidgetsPool)
    {
        memset(s_widgets, 0, sizeof(s_widgets));
        return;
    }
    for(int i = 0; i < OUR_WIDGET_LIMIT; ++i)
    {
        WidgetEntry& e = s_widgets[i];
        if(!e.active) continue;
        if(e.widget) { MenuKit_FreeWidgetSlot(e.widget, e.slot); ++freed; }
        e = WidgetEntry{};
    }
    s_tick = NULL; s_tickUser = NULL;   /* stop the pump touching freed entries */
    logger->Info("MenuKit: shutdown, %d widget(s) freed", freed);
}

/* entry: framework main.cpp calls this from ON_MOD_LOAD */
extern "C" __attribute__((visibility("default")))
bool MenuKit_Load(IAML* aml) { return MenuKit_Init(aml); }