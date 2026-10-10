# SA Menu Kit

[![Release](https://img.shields.io/github/v/release/Jean7z/gta-sa-menu-kit)](https://github.com/Jean7z/gta-sa-menu-kit/releases)
[![Build](https://github.com/Jean7z/gta-sa-menu-kit/actions/workflows/android-build.yml/badge.svg)](https://github.com/Jean7z/gta-sa-menu-kit/actions/workflows/android-build.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

SA Menu Kit is a shared UI library for Android mods that use Android Mod Loader
(AML). It provides native GTA: San Andreas touch widgets and an immediate-mode
2D canvas. The current target is GTA: San Andreas 2.10 on `arm64-v8a`.

Game functions and globals are resolved through AML symbols. Widget access uses
the layout verified for this game build; other game versions and ABIs have not
been verified.

## Build

The build requires Android NDK r29 and the `psdk` submodule.

```sh
git clone --recurse-submodules https://github.com/Jean7z/gta-sa-menu-kit
cd gta-sa-menu-kit
ndk-build
```

If `ndk-build` is not on `PATH`, invoke it from the NDK directory, for example:

```sh
~/android-ndk-r29/ndk-build
```

The output is `libs/arm64-v8a/libAML_PSDK_MenuKit64.so`. `Application.mk` pins
the build to `arm64-v8a`; GitHub Actions builds the same target with NDK
`29.0.14206865`.

## Install

Copy `libAML_PSDK_MenuKit64.so` from a [release](https://github.com/Jean7z/gta-sa-menu-kit/releases)
to AML's mods directory:

```text
Android/data/com.rockstargames.gtasa/mods/
```

If Android restricts access to that directory, copy the file with Shizuku. For
example, from Termux after downloading the release asset to `Download`:

```sh
~/shizuku/rish -c "cp /sdcard/Download/libAML_PSDK_MenuKit64.so /sdcard/Android/data/com.rockstargames.gtasa/mods/"
```

AML loads mods by filename. Load the framework before client mods so
`MenuKit_GetAPI()` can resolve it during the client's `ON_MOD_LOAD` callback.
The helper returns `NULL` if the library or exported API is not available.

## Use the API

Include the AML and Menu Kit headers in a C++ AML mod. This example registers a
native button and draws a translucent panel each frame:

```cpp
#include <math.h>
#include <time.h>

#include <mod/amlmod.h>
#include <mod/menu-api.h>

MYMOD(example.sa.menukit, MenuKit Example, 1.0, Example)

static const MenuKitAPI* g_menuKit;

static unsigned MonotonicMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void DrawFrame(void*)
{
    static unsigned start = 0;
    const unsigned now = MonotonicMs();
    if(!start) start = now;

    const unsigned char alpha = (unsigned char)(127 + 127 * sinf((now - start) * 0.004f));
    g_menuKit->DrawRect(60, 60, 300, 100, 0x10182000u | alpha, 1);
    g_menuKit->DrawText(72, 72, "RADIO", 2, 0x38BDF8FFu);
}

ON_MOD_LOAD()
{
    g_menuKit = MenuKit_GetAPI(aml);
    if(!g_menuKit || g_menuKit->version < MENUKIT_API_VERSION) return;

    void* button = g_menuKit->AddButton(0, "_shoot", 320, 380, 20, NULL, NULL, NULL);
    if(button)
    {
        g_menuKit->SetText(button, "RADIO");
        g_menuKit->SetSize(button, 220, 90);
    }

    g_menuKit->SetTick(DrawFrame, NULL);
}
```

`SetTick` is one shared callback slot. Calling it replaces the previous
callback, so client mods that need a frame callback must coordinate ownership.
The callback runs on the game's processing thread; keep it short and issue
canvas draw calls there.

## API reference

### Widgets

| Member | Behavior |
|---|---|
| `AddButton(menu, texture, x, y, scale, onRelease, userdata, icon)` | Registers a widget and returns an opaque handle, or `NULL` if registration fails. `texture` must name a texture in the game's database. `icon` may be an absolute path to an RGBA PNG; `NULL` keeps the database texture. The widget is built on a later game frame. |
| `RemoveWidget(handle)` | Destroys the widget and releases its pool slot. No-op for `NULL`. |
| `IsReleased(handle)` | Returns true once when the player releases a touch over the widget. |
| `OpenMenu(menu)` / `CloseMenu()` | Switches between Menu Kit widget groups. Group `0` is the root; positive IDs are opened as a stack. Returns `0` on success and `-1` if the operation cannot be completed. |
| `SetText(handle, text)` | Sets an ASCII label using the embedded 5x7 font. The label replaces the button texture, or is composited over a custom icon. Pass `NULL` or an empty string to clear it. |
| `SetVisible(handle, visible)` | Shows or parks a widget. A parked widget is neither drawn nor touchable. Use this for repeated visibility changes. |
| `SetAlpha(handle, alpha)` | Sets widget alpha from 0 to 255. Alpha alone does not disable touch input; use `SetVisible` to park a widget. |
| `SetSize(handle, width, height)` | Sets widget size in real pixels while preserving the engine-computed center. |
| `GetRect(handle, &left, &top, &right, &bottom)` | Writes the live widget rectangle in real pixels. Returns `0` for an unknown or not-yet-laid-out widget, otherwise `1`. |
| `GetTap(&x, &y)` | Returns `1` on the frame a new touch begins and writes its position in real pixels. Returns `0` otherwise. A held touch produces one tap. |
| `GetPointer(&x, &y, &down)` | Writes the current pointer position and down state in real pixels. Returns `0` if the pointer symbols are unavailable; output values are then unchanged. |
| `GetMenuUp()` | Returns `1` when a GTA menu is open, `0` when none is open, and `-1` if the game menu symbol is unavailable. This reports GTA menus, not Menu Kit groups. |
| `SetTick(callback, userdata)` | Registers the shared per-frame callback described above. Pass `NULL` to clear it. |

Custom icon images larger than 512×512 are resized before upload; if resizing
allocation fails, the icon is skipped and the game texture is used. The icon
cache holds up to 48 entries. Unused entries are released when a widget is removed;
when the cache is full, additional icons fall back to the game's texture. Label
textures are cached separately and unused entries are also released on widget
removal.

### Canvas

Canvas drawing is immediate: redraw each frame from `SetTick`. Coordinates are
real pixels, in the same space as `GetRect`, `GetTap`, and `GetPointer`. Canvas
geometry does not consume widget pool slots.

| Member | Behavior |
|---|---|
| `DrawRect(x, y, width, height, rgba, filled)` | Rectangle; `filled=0` draws a 1 px outline. |
| `DrawQuad(x1, y1, ... x4, y4, rgba, filled)` | Four-point polygon. |
| `DrawTriangle(x1, y1, ... x3, y3, rgba, filled)` | Triangle. |
| `DrawLine(x1, y1, x2, y2, rgba, width)` | Thick line built from two triangles. Widths below 1 px are raised to 1 px. |
| `DrawPoly(xy, count, rgba, filled, closed)` | Polygon from an array of `x, y` pairs. Filled polygons use a triangle fan and should be convex. `closed=0` leaves the outline open. |
| `DrawCircle(cx, cy, radius, rgba, filled, segments)` | Circle or ring; segment count is clamped to 3–128. |
| `DrawText(x, y, text, scale, rgba)` | 5×7 bitmap text. Scale is clamped to 1–16; characters outside ASCII `0x20`–`0x7E` render as `?`. Text longer than 200 characters is ignored. |
| `SetDrawBlend(src, dst)` | Sets RenderWare blend factors for subsequent canvas rendering. Pass `-1` for either factor to use its default. |
| `ResetDrawState()` | Restores the default source-alpha blend. |

Colors use `0xRRGGBBAA`: red is the high byte and alpha is the low byte. For
example, `0xF97316FF` is opaque orange. Blend state persists until reset.

Shapes and canvas text share a batch capped at 4096 vertices and 6144 indices
per frame. When the batch fills, further primitives are dropped and the event
is logged. Draw order follows call order. The canvas is rendered after AML's
widgets, so canvas content covers overlapping AML controls.

Canvas text uses untextured geometry in this batch. It does not allocate a
texture per string.

## Coordinates and sizing

`AddButton` uses the game's virtual 640×448 coordinates. `x` and `y` specify the
widget origin; `scale` is its half-size in virtual units. `SetSize` instead uses
real pixels. `GetRect`, `GetTap`, `GetPointer`, and canvas drawing also use real
pixels.

On the test device, the game rendered at 1600×720. Widget position used factors
of 1600/640 horizontally and 720/448 vertically, while the native widget size
used the horizontal factor on both axes. A widget created with equal horizontal
and vertical scale therefore appeared square. These measurements describe that
test setup; use `GetRect` for the live rectangle on the current device.

The engine's `OS_ScreenGetWidth()` and `OS_ScreenGetHeight()` reported 1024×600
on that setup, so those functions are not used to infer widget bounds. `SetSize`
changes the dimensions around the center chosen by the game.

Widget registration is deferred until the next `CGame::Process` frame. This lets
clients register widgets from `ON_MOD_LOAD`, before the game's texture database
is ready. Only the active Menu Kit group occupies game widget slots; other
groups are built when opened.

## Compatibility and limits

- The public API is version 11. New API members are appended to `MenuKitAPI`.
  Check `api->version` before using members introduced after the version your
  client was built against.
- The framework tracks at most 16 widget handles. The game has 190 pool slots,
  but the number available to mods varies with game state.
- Only `arm64-v8a` on GTA: San Andreas 2.10 has been verified. Other game
  versions and ABIs are unsupported.
- The API v11 widget, touch, and canvas paths have been exercised on device,
  including dragging a seek bar and checking `GetMenuUp` around GTA menus.
- `SetText` uses the embedded bitmap font; external fonts are not supported.

See [releases](https://github.com/Jean7z/gta-sa-menu-kit/releases) for packaged
builds and [the MIT license](LICENSE) for usage terms.
