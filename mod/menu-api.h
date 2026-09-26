#pragma once
#include <stdint.h>
#include <mod/iaml.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MENUKIT_API_VERSION 3

typedef void (*MenuKit_OnReleaseCallback)(void* userdata);

/* API v3 - resolved by symbol from AML_PSDK_MenuKit64.so / AML_PSDK_MenuKit.so
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
       space (NOT normalized 0-1); the game scales to the real screen as
       (Origin +/- Scale) * screen/640|448. scale: uniform half-extent in the
       same space (native attack button ~ Origin(560,380) Scale(50,30)).
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