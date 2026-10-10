#pragma once
#include <stdint.h>
#include <mod/iaml.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MENUKIT_API_VERSION 11

typedef void (*MenuKit_OnReleaseCallback)(void* userdata);
typedef void (*MenuKit_TickCallback)(void* userdata);

/* The API is append-only. Check `version` before using members added after the
   version your client was built against. */
typedef struct MenuKitAPI
{
    uint32_t version;

    /* Register a widget in a Menu Kit group. Group 0 is the root; positive IDs
       are opened with OpenMenu. x/y and scale use the game's virtual 640x448
       space; scale is the square widget's half-size. `texture` names a game
       texture. `icon`, when non-NULL, is an absolute path to an RGBA PNG. The
       widget is built on a later game frame. Returns NULL on failure. */
    void* (*AddButton)(int menu, const char* texture, float x, float y, float scale,
                       MenuKit_OnReleaseCallback onRelease, void* userdata,
                       const char* icon);

    /* Remove a previously added widget (handle from AddButton). No-op if NULL. */
    void (*RemoveWidget)(void* handle);

    /* Return true once when a touch is released over this widget. */
    bool (*IsReleased)(void* handle);

    /* Switch Menu Kit widget groups. OpenMenu pushes a positive group ID and
       CloseMenu restores the previous group. The root group (0) is implicit.
       Return 0 on success, -1 if the operation fails. Only the active group's
       widgets occupy game pool slots. */
    int (*OpenMenu)(int menu);
    int (*CloseMenu)(void);

    /* Set an ASCII label using the embedded 5x7 font. The label replaces the
       default button texture or is composited over a custom icon. Pass NULL or
       an empty string to clear it. Changes are applied by the next game-frame
       update; this can be called before the widget is built. */
    void (*SetText)(void* handle, const char* text);

    /* Register the single shared per-frame callback. It runs on the game's
       processing thread before the widget pool is updated. Registering another
       callback replaces the previous one; keep the callback short. */
    void (*SetTick)(MenuKit_TickCallback onTick, void* userdata);

    /* Set widget alpha from 0 (transparent) to 255 (opaque). The desired value
       is reapplied after the game's widget update each frame. Alpha does not
       disable touch input; use SetVisible for that. */
    void (*SetAlpha)(void* handle, unsigned char alpha);

    /* Show or park a widget. A parked widget is not drawn and does not receive
       touch input. Use this to toggle visibility without destroying and
       rebuilding the widget. */
    void (*SetVisible)(void* handle, int visible);

    /* Set width and height in real pixels, preserving the center computed by
       the game. AddButton positions widgets in virtual 640x448 coordinates and
       creates square widgets; this method changes only their size. The values
       are reapplied after the game's widget update each frame. */
    void (*SetSize)(void* handle, float w, float h);

    /* Return 1 on the frame a new touch begins and write its position in real
       pixels. Return 0 when no new touch is available. */
    int (*GetTap)(float* x, float* y);

    /* Write the live widget rectangle in real pixels. Return 0 for an unknown,
       unbuilt, or not-yet-laid-out widget; otherwise return 1. */
    int (*GetRect)(void* handle, float* l, float* t, float* r, float* b);

    /* Immediate-mode canvas drawing in real pixels. Call these from SetTick;
       coordinates use the same space as GetRect and GetTap. */
    void (*DrawRect)(float x, float y, float w, float h, uint32_t rgba, int filled);
    void (*DrawQuad)(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, uint32_t rgba, int filled);
    void (*DrawTriangle)(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t rgba, int filled);
    void (*DrawLine)(float x1, float y1, float x2, float y2, uint32_t rgba, float width);
    void (*DrawPoly)(const float* xy, int count, uint32_t rgba, int filled, int closed);
    void (*DrawCircle)(float cx, float cy, float r, uint32_t rgba, int filled, int segments);
    void (*SetDrawBlend)(int src, int dst); /* rwBLEND* or -1 to reset */
    void (*ResetDrawState)(void);

    /* Read the current pointer position and down state in real pixels. Returns
       1 when the pointer symbols are available and 0 otherwise. On failure,
       output values are left unchanged. */
    int (*GetPointer)(float* x, float* y, int* down);

    /* Draw 5x7 bitmap text without creating a widget. x/y is the top-left in
       real pixels. Scale is clamped to 1..16; character advance is 6*scale.
       Characters outside ASCII 0x20..0x7E render as '?'; strings over 200
       characters are ignored. Colors use 0xRRGGBBAA.

       Text is emitted as untextured quads in the canvas batch. Draw order follows
       call order and the background is transparent. */
    void (*DrawText)(float x, float y, const char* text, int scale, uint32_t rgba);

    /* Return 1 if a GTA menu is open, 0 if not, or -1 if its game symbol is
       unavailable. This does not report Menu Kit groups. */
    int (*GetMenuUp)(void);
} MenuKitAPI;

/* Exported entrypoint resolved by client mods through AML. */
__attribute__((visibility("default")))
const MenuKitAPI* GetMenuAPI(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
/* Resolve the API from a loaded Menu Kit library. Returns NULL if the library
   or exported entrypoint is unavailable. */
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