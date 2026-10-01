#pragma once
#include <stdint.h>
#include <mod/iaml.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MENUKIT_API_VERSION 8

typedef void (*MenuKit_OnReleaseCallback)(void* userdata);
typedef void (*MenuKit_TickCallback)(void* userdata);

/* API v7 - resolved by symbol from AML_PSDK_MenuKit64.so / AML_PSDK_MenuKit.so
   v7 changes: GetTap + GetRect appended at the END of the struct. Global
   pointer access, so "tap outside to dismiss" is possible at all: AML exposes
   no touch/pad API, and a client could only react to touches on its OWN
   widgets.
   v6 changes: SetSize appended at the END of the struct (it was SetRect in an
   earlier draft, but an absolute rect needed the true screen size, and
   OS_ScreenGetWidth() lies: 1024x600 against a real 1600x720 render).
   SetAlpha/SetVisible semantics changed: they now set DESIRED state that the
   framework re-asserts on every frame, so they work on LIVE widgets, not just
   pending ones.
   v5 changes: SetTick + SetAlpha appended at the END of the struct.
   v4 changes: SetText(handle, text) — render a label onto an existing button
   with the engine's embedded 5x7 bitmap font (Plan B; NO CFont, which does not
   composite in this fork). The label texture replaces the button's sprite and
   is either composited OVER the custom icon's pixels when the widget has one,
   or drawn centered on a generated dark panel sized to the widget's live
   on-screen rect. New member is appended at the END of the struct so old
   clients compiled against v3 keep working (they only read fields by offset;
   version check is best-effort).
   v3 changes: AddButton takes an optional custom icon (absolute PNG path; NULL
   = the game's default button texture).
   v2 changes: AddButton takes a menu id; OpenMenu/CloseMenu manage the menu
   stack. Registration is ALWAYS deferred — the real CWidgetButton is built by
   the engine pump on the first CGame::Process frame after its menu becomes
   active, so calling from ON_MOD_LOAD is safe (the texture system doesn't
   exist until runtime) and an unopened menu is never drawn. */
typedef struct MenuKitAPI
{
    uint32_t version;

    /* Register a native game button widget (CWidgetButton in the game's widget
       pool) belonging to a menu group. menu=0 is the ROOT group: shown while
       no menu is open. menu>=1 belongs to that menu id: shown only while the
       menu is active via OpenMenu, and hidden (pool slots freed) on close.
       texture: name of the button texture from the game's texture DB (e.g.
       "shoot"). x, y: ORIGIN coordinates in the game's VIRTUAL 640x448 screen
       space (NOT normalized 0-1). scale: UNIFORM half-extent in the same space
       (native attack button ~ Origin(560,380) Scale(20,20)).
       PROJECTION (measured, do not re-derive). POSITION and SIZE follow
       DIFFERENT rules — this is the single most important thing to know here:

           position x : realRenderWidth  / 640 = 2.500   (test device)
           position y : realRenderHeight / 448 = 1.607   (same device)
           size       : realRenderWidth  / 640           (UNIFORM on BOTH axes)

       A widget therefore lands SQUARE in real pixels (100x100 measured) even
       though the virtual space is 640x448, because SIZE uses the width factor
       for both axes while POSITION uses one factor per axis. `scale` is a
       single float, so AddButton can only ever build squares.

       Consequence that silently breaks layout code: a widget's on-screen
       HEIGHT (100 px) exceeds its own vertical virtual footprint
       (40 units x 1.607 = 64 px). Vertical gaps computed in virtual units come
       out roughly 36% too small, so shapes collide that "should" have margin.
       Derive gaps from GetRect, not from virtual units.

       MenuKit's own resolution log reports 1024x600 when the real render is
       1600x720, so the screen size cannot be queried: OS_ScreenGetWidth and
       OS_ScreenGetHeight lie. GetRect on an already-constructed widget is the
       only reliable source of truth.
       icon: optional ABSOLUTE path to a custom button PNG (RGBA), or NULL to
       keep the game's DB texture. Decoded by the engine via stb_image and
       turned into an RwTexture mirroring the game's own pipeline; loaded once
       and cached by path (shared by any button using the same file).
       Returns an opaque handle, or NULL on failure (widget limit reached). */
    void* (*AddButton)(int menu, const char* texture, float x, float y, float scale,
                       MenuKit_OnReleaseCallback onRelease, void* userdata,
                       const char* icon);

    /* Remove a previously added widget (handle from AddButton). No-op if NULL. */
    void (*RemoveWidget)(void* handle);

    /* Get current release state of a widget since last frame. Returns true once per press. */
    bool (*IsReleased)(void* handle);

    /* Menu stack (push/pop). OpenMenu(menu) makes menu's buttons visible:
       it frees the current group's pool widgets for the game to reuse, pushes
       the menu, and the pump builds the new group at topmost free slots on the
       next frame. CloseMenu() reverses (destroys the active group; the
       previous menu's buttons are rebuilt automatically). Only the ACTIVE
       group's widgets ever occupy pool slots. Returns 0 on success, -1 on
       error (invalid id, already open, stack full, engine not ready, or not
       enough free pool slots for the whole group — the menu is NOT opened). */
    int (*OpenMenu)(int menu);
    int (*CloseMenu)(void);

    /* v4: Set a textual label on an existing button (handle from AddButton).
       text: NUL-terminated ASCII string (chars 0x20..0x7E; others render as
       '?'), encoded with the engine's embedded 5x7 bitmap font. When the
       widget has a custom icon, the label is baked ON TOP of the icon's pixel
       data (the same texture that Draw renders — guaranteed composition, on
       top of the sprite). Without an icon, the engine generates a dark
       translucent panel sized to the widget's live on-screen rect and draws
       the text centered on it, dropping the game's default texture. The
       label replaces the button's sprite (it always wins over any icon).
       Safe to call before the widget is built (registered but deferred) and
       at runtime; the engine applies it on the next game frame. Pass NULL or
       "" to clear the label (restores the underlying icon / default texture
       on the next frame). */
    void (*SetText)(void* handle, const char* text);

    /* v5: per-frame client callback, invoked from MenuKit's own pump — the same
       one that builds pending widgets and polls touch state. AML exposes only
       PRELOAD/LOAD/UNLOAD/CRASH, so this is the ONLY per-frame a client gets;
       it is what makes auto-hide timers possible. Runs on the pump's thread:
       keep it cheap, and do not call Remove/Add from inside it. */
    void (*SetTick)(MenuKit_TickCallback onTick, void* userdata);

    /* v6: alpha 0..255. Applies to LIVE widgets, not just pending ones: the
       framework re-asserts the desired value on every frame from the
       CGame_Process pump. That is what a one-shot write could not do - the
       engine's own widget update runs inside CGame_Process and restored the
       byte, so SetAlpha(128) read back as 255 within a frame. Now the write
       lands after the engine, so a dim control stays dim. */
    void (*SetAlpha)(void* handle, unsigned char alpha);

    /* v5: show/hide. Hidden means NOT DRAWN and NOT TOUCHABLE: the touch-immune
       flag is cleared, handing the widget back to CTouchInterface's per-frame
       SetEnabled(w,0) — exactly how the game parks its own idle buttons. alpha 0
       ALONE is not enough: it stops CWidgetButton::Draw but leaves the touch
       rect live, and an invisible button that still eats taps is worse than no
       button at all. Preferred over Remove/Add for a show-hide toggle, because
       neither RemoveWidget nor a group rebuild frees the widget object (512 B
       per build), so toggling that way leaks on every cycle. */
    void (*SetVisible)(void* handle, int visible);

    /* v6: resize a widget in REAL pixels (w, h), keeping the centre the engine
       computed. This is what breaks the "every widget is a square" limit: the
       engine drives both axes from ONE uniform `scale`, so AddButton can only
       ever build a square, while the four floats the engine already keeps in
       the object can be written independently.

       SIZE-ONLY on purpose. An absolute x/y/w/h version needed the true screen
       size, and OS_ScreenGetWidth() is not it - it reported 1024x600 against a
       real 1600x720 render, which put widgets in the middle of the screen.
       Keeping the engine's centre preserves its virtual-unit anchoring, which
       is correct and device-independent, and changes only the shape.

       Re-asserted every frame, so the engine cannot take the size back. */
    void (*SetSize)(void* handle, float w, float h);

    /* v7: did the player tap the screen this frame, and where?
       Returns 1 on the frame a NEW tap begins (rising edge) and writes the
       position; 0 otherwise. Holding a finger is ONE tap, not one per frame.

       This is the "tap outside to dismiss" primitive. AML exposes no touch/pad
       API, so a client could only react to touches landing on one of its OWN
       widgets - and the usual workaround, a transparent full-screen catcher
       widget, swallows the game's joystick and fire button, which is far worse
       than not having the feature.

       Same real-pixel space as GetRect, so a hit test is four comparisons with
       no knowledge of the engine's virtual 640x448 projection. Returns 0 if
       the pointer globals were not resolved, so a client can degrade instead
       of acting on garbage. */
    int (*GetTap)(float* x, float* y);

    /* v7: live on-screen rect of a widget in real pixels (l, t, r, b). Pairs
       with GetTap: both in the same space, so "did the tap land on this widget"
       needs no arithmetic beyond four comparisons.

       Returns 0 for an unknown or not-yet-built handle, and 0 while the engine
       has not laid the widget out yet (its rect is a 1e6 sentinel until then) -
       so a 0 means "ask again next frame", never a garbage rect. */
    int (*GetRect)(void* handle, float* l, float* t, float* r, float* b);

    /* v8: immediate-mode 2D draw (real pixels). Draw calls are issued per frame
       (typically from SetTick). Coordinates match GetRect/GetTap space. */
    void (*DrawRect)(float x, float y, float w, float h, uint32_t rgba, int filled);
    void (*DrawQuad)(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, uint32_t rgba, int filled);
    void (*DrawTriangle)(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t rgba, int filled);
    void (*DrawLine)(float x1, float y1, float x2, float y2, uint32_t rgba, float width);
    void (*DrawPoly)(const float* xy, int count, uint32_t rgba, int filled, int closed);
    void (*DrawCircle)(float cx, float cy, float r, uint32_t rgba, int filled, int segments);
    void (*SetDrawBlend)(int src, int dst); /* rwBLEND* or -1 to reset */
    void (*ResetDrawState)(void);
} MenuKitAPI;

/* Framework entrypoint. The framework .so exports this; client mods resolve it. */
__attribute__((visibility("default")))
const MenuKitAPI* GetMenuAPI(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
/* Client helper: resolve the API from a loaded AML_PSDK_MenuKit* library. Returns NULL if the
   framework isn't loaded or the symbol can't be found. Safe to call from ON_MOD_LOAD. */
static inline const MenuKitAPI* MenuKit_GetAPI(IAML* aml)
{
    if(!aml) return NULL;
    uintptr_t lib = aml->GetLib("AML_PSDK_MenuKit64");
    if(!lib) lib = aml->GetLib("AML_PSDK_MenuKit");
    if(!lib) return NULL;
    typedef const MenuKitAPI* (*GetMenuAPIFn)(void);
    GetMenuAPIFn fn = (GetMenuAPIFn)aml->GetSym(lib, "GetMenuAPI");
    return fn ? fn() : NULL;
}
#endif