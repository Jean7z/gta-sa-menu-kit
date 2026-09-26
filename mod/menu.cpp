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
    uint32_t pad14;    /* +0x14 */
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
    WidgetPosition pos;
};
static WidgetEntry s_widgets[OUR_WIDGET_LIMIT];

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

    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load(path, &w, &h, &ch, 4);   /* RGBA8 */
    if(!px) { logger->Error("MenuKit: icon '%s' not decodable", path); return NULL; }

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
       +0x58=0xFF alpha instantly visible. */
    *(uint32_t*)((uintptr_t)mem + 0x8c) |= 0x04;
    *(uint8_t*)((uintptr_t)mem + 0x59) = 0x01;
    *(uint8_t*)((uintptr_t)mem + 0x58) = 0xFF;
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

/* Free a menu's pool widgets so the game can reuse the slots (pool is
   ~172/190 used with menus open). The CWidgetButton objects themselves are NOT
   freed (double-free risk — the game has no per-widget free we can trust);
   the leak is bounded by OUR_WIDGET_LIMIT. Entries keep their definition;
   the pump rebuilds the group at topmost free when it becomes active again. */
static void MenuKit_DestroyGroup(int menu)
{
    for(int i = 0; i < OUR_WIDGET_LIMIT; ++i)
    {
        WidgetEntry& e = s_widgets[i];
        if(!e.active || e.menu != menu) continue;
        if(e.widget && e.slot >= 0 && e.slot < MAX_POOL_SLOTS && s_pWidgetsPool[e.slot] == e.widget)
            s_pWidgetsPool[e.slot] = NULL;
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

DECL_HOOKv(CGame_Process, void)
{
    CGame_Process(); /* orig */

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
    }

    /* DEBUG: full-pool dump. Every ~300 ticks (5s) AND on structural change
       (pool content hash), capped at 150 dumps (~12min window). Catches the
       vehicle-mode layout: entering a car rebuilds the pool around the game's
       vehicle buttons (the native attack button sits at slot 187, so vehicle
       widgets are NOT all low). Verifies our widget stays in the topmost free
       slot and reveals which slots the game's vehicle buttons use. */
    static int s_dbgTick = 0;
    static uint64_t s_dbgHash = 0;
    static int s_dbgDumps = 0;
    ++s_dbgTick;
    uint64_t hash = 0;
    for(int s = 0; s < MAX_POOL_SLOTS; ++s)
        hash ^= (uint64_t)(uintptr_t)s_pWidgetsPool[s] * (uint64_t)(s + 1);
    bool changed = s_dbgTick > 90 && hash != s_dbgHash;
    if(((s_dbgTick % 300) == 0 || changed) && s_dbgDumps < 150)
    {
        s_dbgHash = hash;
        ++s_dbgDumps;
        int used = 0;
        /* Raster-header diff: ours (MenuKitIcon) vs the FIRST non-ours textured
           widget (a game-native texture like shoot, rendered by the SAME
           CWidgetButton::Draw). Both rasters flow through CSprite2d::Draw's
           texture bind — any flag/format/type difference is the corruption
           suspect. RwTexture.raster is its FIRST member (*spr); this fork's
           raster header is GL-backed: dims u32 @+0x18/+0x1c, flags/type packed
           in the low header dwords (our creation diag showed of=0x604: low
           nibble 4 = rwRASTERTYPECAMERATEXTURE vs Txd textures' TEXTURE type). */
        int firstForeign = -1;
        void* ourSprSlot = NULL;
        for(int s = 0; s < MAX_POOL_SLOTS; ++s)
        {
            void* w = s_pWidgetsPool[s];
            if(!w) continue;
            ++used;
            uintptr_t p = (uintptr_t)w;
            uint32_t flags = *(uint32_t*)(p + 0x8c);
            uint32_t hid   = *(uint32_t*)(p + 0x08);
            uint8_t  alpha = *(uint8_t*)(p + 0x58);
            uint8_t  fade  = *(uint8_t*)(p + 0x59);
            void*    spr   = *(void**)(p + 0x10);
            const char* tnm = spr ? (const char*)((uintptr_t)spr + 0x20) : ""; /* RwTexture name inline @+0x20 */
            float l = *(float*)(p + 0x2c), t = *(float*)(p + 0x30);
            float r = *(float*)(p + 0x34), b = *(float*)(p + 0x38);
            bool ours = false;
            for(int i = 0; i < OUR_WIDGET_LIMIT; ++i)
                if(s_widgets[i].active && s_widgets[i].widget == w) { ours = true; break; }
            if(spr && ours && !ourSprSlot) ourSprSlot = spr;
            if(spr && !ours && firstForeign < 0) firstForeign = s;
            logger->Info("MenuKit: DBG slot=%d vtb=%p spr=%p tnm=%.24s hid=%u flg=0x%X al=%u fade=%u rect=(%g,%g,%g,%g)%s",
                s, *(void**)p, spr, tnm, hid, flags, alpha, fade, l, t, r, b, ours ? " <<< OURS" : "");
        }
        logger->Info("MenuKit: DBG pool=%d/%d used%s", used, MAX_POOL_SLOTS, changed ? " (changed)" : "");
        if(ourSprSlot || firstForeign >= 0)
        {
            void* foreignSpr = firstForeign >= 0 ? *(void**)((uintptr_t)s_pWidgetsPool[firstForeign] + 0x10) : NULL;
            for(int pass = 0; pass < 2; ++pass)
            {
                void* spr = pass == 0 ? ourSprSlot : (firstForeign >= 0 && pass == 1 ? foreignSpr : NULL);
                if(!spr) continue;
                void* ras = *(void**)spr;
                const char* tag = pass == 0 ? "OURS" : "GAME";
                char qws[360] = {0}; int qp = 0;
                const unsigned char* rb = (const unsigned char*)ras;
                for(int i = 0; i < 0x38 && qp < 340; i += 8)
                    qp += snprintf(qws + qp, sizeof(qws) - qp, "[%02x]=%p ", i, *(const void**)(rb + i));
                logger->Info("MenuKit: RASTERDIFF %s slot=%d tex=%p ras=%p w=%u h=%u %s",
                             tag, pass == 0 ? -1 : firstForeign, spr, ras,
                             rb ? *(const uint32_t*)(rb + 0x18) : 0,
                             rb ? *(const uint32_t*)(rb + 0x1c) : 0, qws);
            }
        }
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

    /* RenderWare symbol resolution for custom icons (non-fatal if missing). */
    s_pfnRwImageCreate           = (void*(*)(int,int,int))aml->GetSym(pGameHandle, "_Z13RwImageCreateiii");
    s_pfnRwImageDestroy          = (void(*)(void*))aml->GetSym(pGameHandle, "_Z14RwImageDestroyP7RwImage");
    s_pfnRwImageFindRasterFormat = (int(*)(void*,int,int*,int*,int*,int*))aml->GetSym(pGameHandle, "_Z23RwImageFindRasterFormatP7RwImageiPiS1_S1_S1_");
    s_pfnRwRasterCreate          = (void*(*)(int,int,int,int))aml->GetSym(pGameHandle, "_Z14RwRasterCreateiiii");
    s_pfnRwRasterDestroy         = (void(*)(void*))aml->GetSym(pGameHandle, "_Z14RwRasterDestroyP8RwRaster");
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
    if(e->widget && e->slot >= 0 && e->slot < MAX_POOL_SLOTS && s_pWidgetsPool[e->slot] == e->widget)
        s_pWidgetsPool[e->slot] = NULL; /* leave the object; pump may reclaim the slot */
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

static MenuKitAPI g_api = {
    MENUKIT_API_VERSION,
    MenuKit_AddButton,
    MenuKit_RemoveWidget,
    MenuKit_IsReleased,
    MenuKit_OpenMenu,
    MenuKit_CloseMenu
};

const MenuKitAPI* GetMenuAPI(void) { return &g_api; }

/* entry: framework main.cpp calls this from ON_MOD_LOAD */
extern "C" __attribute__((visibility("default")))
bool MenuKit_Load(IAML* aml) { return MenuKit_Init(aml); }