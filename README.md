# SA Menu Kit

UI framework for GTA SA mods on Android (AML). It gives you two things: buttons
registered in the game's native widget pool, and an immediate-mode 2D canvas for
drawing shapes, alpha and text without spending slots. Everything is resolved by
symbol via IAML (`GetSym`), with no binary byte patching.

[![Version: 11.1](https://img.shields.io/badge/version-11.1-green.svg)](https://github.com/Jean7z/gta-sa-menu-kit/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](./LICENSE)
[![Game: GTA SA 2.10 Android](https://img.shields.io/badge/game-GTA%20SA%202.10%20Android-blueviolet.svg)]()
[![Platform: Android](https://img.shields.io/badge/platform-Android-lightgrey.svg)]()
[![Loader: AML](https://img.shields.io/badge/loader-Android%20Mod%20Loader-orange.svg)](https://github.com/AndroidModLoader/AndroidModLoader)

A plugin for [Android Mod Loader (AML)](https://github.com/AndroidModLoader/AndroidModLoader)
targeting [GTA: San Andreas 2.10](https://github.com/gta-reversed/gta-reversed-android).
Client mods use this UI layer: they download the `.so` from
[Releases](https://github.com/Jean7z/gta-sa-menu-kit/releases), drop it into their
mods folder, and call `MenuKit_GetAPI()`.

**arm64-v8a only.** The hooks are verified against SA 2.10's `libGTASA.so`, which
is pure arm64.

## Building

The build needs the `psdk` submodule (Android Mod Loader PSDK).

```sh
git clone --recursive https://github.com/Jean7z/gta-sa-menu-kit
cd gta-sa-menu-kit
~/android-ndk-r29/ndk-build
# output: libs/arm64-v8a/libAML_PSDK_MenuKit64.so
```

If you cloned without `--recursive`: `git submodule update --init --recursive`.
The ABI is pinned to arm64-v8a in `Application.mk`.

## Installing

Download `libAML_PSDK_MenuKit64.so` from
[Releases](https://github.com/Jean7z/gta-sa-menu-kit/releases) (arm64-v8a) and
put it in AML's mods folder, which on the device is
`Android/data/com.rockstargames.gtasa/mods/`. AML does not load from
`files/psdk/`.

On Android 11+ that folder cannot be written with `adb push` or from a file
manager, so the normal route is Shizuku (in Termux, `rish`):

```sh
~/shizuku/rish -c "cp libs/arm64-v8a/libAML_PSDK_MenuKit64.so \
  /storage/emulated/0/Android/data/com.rockstargames.gtasa/mods/"
```

With root, or on Android 10 and earlier, `adb push` to
`/sdcard/Android/data/com.rockstargames.gtasa/mods/` works directly.

AML loads mods in alphabetical order, so `AML_PSDK_MenuKit64` loads before any
client. Clients call `MenuKit_GetAPI()`, which resolves the framework at runtime
and returns `NULL` if it is not there yet.

## Minimal API

```c
const MenuKitAPI* api = MenuKit_GetAPI(aml);   // NULL if the framework isn't loaded
if(!api || api->version < MENUKIT_API_VERSION) return;   // the client bails out cleanly

void* h = api->AddButton(0, "shoot", 320.0f, 380.0f, 60.0f, onRelease, ud, NULL);
api->SetText(h, "RADIO");
api->SetSize(h, 220.0f, 90.0f);                 // real px: breaks the square
api->SetVisible(h, 0);                           // no rebuild cost

/* Immediate 2D canvas. The clock is the client's: the framework does not hand
   out time because every client already needs its own for its own timers. */
static unsigned NowMs(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void Tick(void*)
{
    static unsigned t0 = 0;
    if(!t0) t0 = NowMs();
    const unsigned t = NowMs() - t0;
    const unsigned char a = (unsigned char)(127 + 127 * sinf(t * 0.004f));

    api->DrawRect(60, 60, 300, 100, 0x101820u | (a << 24), 1);   /* panel */
    api->DrawCircle(90 + (t % 200), 110, 12, 0xF97316FFu, 1, 24);
    api->DrawText(60, 170, "MENUKIT V11", 2, 0x38BDF8FFu);       /* label */
}
api->SetTick(Tick, NULL);
```

## Reference

### Widgets

| Member | What it does |
|---|---|
| `AddButton(menu, texture, x, y, scale, onRelease, userdata, icon)` | Registers a `CWidgetButton` in the game's pool. `menu=0` is the ROOT group, visible while no menu is open; `menu>=1` is only visible while that menu is active. `texture` is a name in the game's texture DB. `icon` is an **absolute** path to an RGBA PNG, or `NULL` for the game's own texture. Returns `NULL` if the pool ran out. |
| `RemoveWidget(handle)` | Frees the pool slot. No-op on `NULL`. |
| `IsReleased(handle)` | `true` exactly once per finger release over the widget. |
| `OpenMenu(menu)` / `CloseMenu()` | Push and pop menus. Return `0` or `-1`; if the group does not fit in the free slots, the menu does not open. |
| `SetText(handle, text)` | Draws an ASCII label with the embedded 5x7 font. Always wins over the icon. `NULL` or `""` removes it. |
| `SetTick(onTick, ud)` | Per-frame callback, invoked from the framework's pump. It is the only tick AML exposes (AML only gives `PRELOAD`/`LOAD`/`UNLOAD`/`CRASH`), and it is what makes auto-hide timers possible. |
| `SetAlpha(handle, 0..255)` | Alpha on live widgets. The framework re-asserts the value every frame from the pump: a one-shot write is not enough, because the engine's update runs inside `CGame_Process` and restored the byte. |
| `SetVisible(handle, 0/1)` | Hidden = not drawn and not touchable (clears the touch-immune flag). `SetAlpha(0)` only kills the `Draw`: the touch rect stays live, and an invisible button that still eats taps is worse than no button at all. Preferred over Remove/Add for show-hide, because neither Remove nor Add frees and rebuilds the widget object (512 B per build), so toggling that way leaks on every cycle. |
| `SetSize(handle, w, h)` | Resizes in **real pixels**, keeping the centre the engine computed. This is the way to break `AddButton`'s square-widget limit. It speaks real px while `AddButton` speaks virtual units, so do not feed it layout coordinates. |
| `GetTap(x, y)` | `1` on the frame a tap **begins** (rising edge) and writes the position; `0` otherwise. Holding a finger down is ONE tap, not one per frame. This is the primitive behind "tap outside to dismiss": AML exposes no touch API, so a client could only react to taps on its OWN widgets. Returns `0` if the globals were not resolved. |
| `GetRect(handle, l, t, r, b)` | Live on-screen rect, in real pixels. Pairs with `GetTap`: the hit test is four comparisons, with none of the projection arithmetic. `0` = unknown handle, or widget not built yet (ask again next frame). |
| `GetPointer(x, y, down)` | Live pointer state, pollable every frame, in the same real-pixel space as `GetTap` and `GetRect`. `GetTap` is a rising edge that vanishes on the next frame: fine for a button, useless for a drag. A slider, a swipe or a long-press all need to know whether the finger is still down and where it is now. `down=1` while a finger is on the screen. Returns `0` if the globals were not resolved. |
| `GetMenuUp()` | `1` if the game has any menu open, `0` if not, `-1` if the symbol was not found. It exists because while a menu is open the finger still belongs to the game, but the client's canvas sees that same raw touch, so a UI drawn on top swallows the menu's taps. A client with an overlay should ignore input while this returns `1`. Resolved by symbol, no fixed offsets. |

### Immediate 2D canvas

The canvas primitives do not create widgets: they stack geometry and the
framework draws it in one batch per frame.

| Member | What it does |
|---|---|
| `DrawRect(x,y,w,h,rgba,filled)` | Rectangle. `filled=1` fills, `0` draws a 1 px outline only. |
| `DrawQuad(x1..y4,rgba,filled)` | Free quadrilateral (sheared or in perspective). |
| `DrawTriangle(x1..y3,rgba,filled)` | Free triangle. |
| `DrawLine(x1,y1,x2,y2,rgba,width)` | Line with real thickness: under 1 px it goes straight to the raster, above that it is built from two triangles. |
| `DrawPoly(xy,count,rgba,filled,closed)` | Polygon from an array of `x,y` pairs. `filled` uses a triangle fan: valid for convex shapes and star shapes, not for self-intersecting outlines. `closed=0` leaves the last edge open. |
| `DrawCircle(cx,cy,r,rgba,filled,segments)` | Circle, or ring with `filled=0`. `segments` is clamped to 3..128. |
| `DrawText(x, y, text, scale, rgba)` | Canvas text, no widget. `x,y` is the top-left corner in real px; `scale` is the glyph multiplier (1 = raw 5x7, 1..16); the advance is `6*scale` per character, so the width is `len*6*scale` with no extra calls. Characters outside `0x20..0x7E` come out as `'?'`. |
| `SetDrawBlend(src,dst)` | Blend mode (`rwBLEND*`). `-1` restores the default. |
| `ResetDrawState()` | Back to the default blend (src-alpha / inv-src-alpha). |

The canvas contract:

- It is immediate, not retained. There is no scene to maintain: what you draw
  this frame is what exists. To animate, change the numbers and draw again. The
  batch is cleared at the start of every frame, before the tick.
- Only from `SetTick`. The framework flushes the whole geometry in a single
  `RwIm2DRenderIndexedPrimitive` call from the `Render2dStuff` hook, which is
  where the engine already has the 2D raster set up. Drawing from the
  `CGame_Process` pump does not work: it runs before that context exists.
- Coordinates are real pixels, the same space as `GetRect` and `GetTap`.
- `rgba` is packed `0xRRGGBBAA`, so animating one channel is a `<<`.
- Batch ceiling: 4096 vertices and 6144 indices per frame. Once full, primitives
  are dropped and it is logged once, rather than breaking the client.
- `DrawText` goes into the same batch as the shapes, one quad per horizontal run
  of lit pixels (about 14 quads per 5x7 glyph). Glyphs sit on a transparent
  background, so whatever the client painted behind shows through. Draw order is
  call order, including with respect to the shapes: a rectangle drawn before a
  label stays underneath, one drawn after covers it.

  Do not implement this with a per-glyph label texture: that route was tried
  first and on the device the sampler binds the raster but never gets to sample
  the uploaded pixels, so the text came out empty. The version that works is the
  untextured-quad one.
- The canvas draws on top of AML's widgets. This is measured on device: AML
  renders its UI at a different point in the frame, outside the game's 2D pass
  and before it. Moving the flush earlier than that pass gave the same result
  and both attempts crashed, because before it runs there is no valid raster.

  In practice, paint a whole panel with the canvas instead of mixing canvas art
  with AML widgets in the same area. If they overlap, the canvas wins.

## Coordinate space

`x` and `y` in `AddButton` are origin coordinates in the game's virtual 640x448
space, not normalized 0-1. The native attack button sits at
`Origin(560,380) Scale(20,20)`, and the centre of the screen is `(320,224)`.

Position and size follow different rules, and this is the thing that surprises
people most when using the API:

```text
position x : realRenderWidth  / 640 = 2.500
position y : realRenderHeight / 448 = 1.607
size       : realRenderWidth  / 640  (uniform on BOTH axes)
```

A widget therefore lands square in real pixels (100x100 measured) even though
the virtual space is 640x448, because size uses the width factor for both axes
while position uses one factor per axis. Since `scale` is a single float,
`AddButton` can only ever build squares.

Two traps fall out of this:

1. Vertical gaps come out ~36% too small. A widget's real height (100 px) is
   larger than its virtual vertical footprint (40 units x 1.607 = 64 px).
   Computing vertical spacing in virtual units makes shapes collide where there
   should be margin. Derive gaps from `GetRect`.
2. The resolution cannot be queried. `OS_ScreenGetWidth()` lies: it reports
   1024x600 against a real 1600x720 render. `GetRect` on an already-built widget
   is the only reliable source of truth.

That is why `SetSize` is size-only in real pixels instead of taking absolute
coordinates: there is no reliable way to know the scale to convert them with.

Registration is always deferred: the real `CWidgetButton` is built by the engine's
pump on the first `CGame::Process` afterwards. That is why calling from
`ON_MOD_LOAD` is safe even though the texture system does not exist yet, and why
a menu that was never opened is never drawn.

## Rules for clients

1. Gate on version, not on offsets: `if(!api || api->version < MENUKIT_API_VERSION) return;`.
   Every new member is appended to the end of the struct, so an old client
   compiled against v4 keeps reading by offset and keeps working.
2. Do not rebuild for show-hide, and do not rebuild for resize. Use `SetVisible`
   and `SetSize`; both are re-asserted every frame and spend no pool slots.
3. Hit-test in real px with `GetTap` and `GetRect`, never by comparing against
   `AddButton`'s virtual coordinates: the two spaces do not mix.

## Limits

- The game's native pool has 190 slots and the game itself occupies about 177, so
  roughly 13 are free. Reuse widgets instead of rebuilding when state changes.
  The canvas batch is separate and consumes no pool.
- arm64-v8a only. SA 2.00 ships arm7, but those offsets were never verified
  against its binary: a v7a build crashes at runtime, so it is not produced.
- v11 verified on an arm64-v8a device: legible text, a seek bar you can drag with
  the label following your finger, shapes and alpha animating, and `GetMenuUp`
  making the launcher stop swallowing the tap that closes the map.
- Still open: `SetText` with an external font, and cleaning up orphaned widgets
  when the game rebuilds the pool halfway through a press.