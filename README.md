# SA Menu Kit

Framework de UI para mods de GTA SA en Android (AML / psdk), usando los widgets
nativos del juego. Los mods clientes dibujan botones que el propio motor
procesa (update/draw/touch) — el framework solo los inyecta en el pool del juego.

**Cero parches de bytes, cero offsets hardcodeados**: cada dirección se
resuelve por símbolo vía IAML (`GetSym`). Compatible SA Android 2.10 arm64-v8a
y 2.00 armeabi-v7a (verificado contra `libGTASA.so` reales vía `nm -D`).

## Arquitectura

- `AML_PSDK_MenuKit64` / `AML_PSDK_MenuKit` — cargador del framework.
  Resuelve `CTouchInterface::m_pWidgets`, `CWidgetButton::CWidgetButton(...)`
  y `CTouchInterface::IsReleased(...)` por símbolo, inyecta el widget en el pool
  del juego y expone `GetMenuAPI()` (API v4).
- `AML_PSDK_CarSpawner64` / `AML_PSDK_CarSpawner` — ejemplo de cliente: un botón
  que spawnea una Sanchez (modelo 411) vía `CStreaming` por símbolo.

El framework usa el pool nativo del juego (190 slots). El widget inyectado
participa de los loops nativos: el juego lo pinta, lo toca, y reporta el release
por ID. El framework re-inyecta el widget si el juego reconstruye el pool (por
ejemplo al abrir/cerrar menús), migrando de slot si el juego lo ocupó.

## Compilar

```sh
~/android-ndk-r29/ndk-build
# salida: libs/arm64-v8a/*.so y libs/armeabi-v7a/*.so
```

## Instalar

1. Copia `libAML_PSDK_MenuKit64.so` a `Android/data/com.rockstargames.gtasa/files/psdk/` (arm64).
2. Copia `libAML_PSDK_CarSpawner64.so` a la misma carpeta.
3. Lanza el juego (AML carga los mods en orden de letras: `AML_PSDK_*`).

> El framework debe cargarse ANTES que los clientes (orden alfabético de la
> carpeta psdk: `MenuKit` < `CarSpawner`). Los clientes usan `MenuKit_GetAPI()`
> que resuelve el framework en runtime y aborta limpio si no está.

## API v4 (`mod/menu-api.h`)

```c
const MenuKitAPI* api = MenuKit_GetAPI(aml);   // NULL si el framework no está
if(!api || api->version < 4) return;            // el cliente aborta limpio

void* h = api->AddButton(0, "shoot", 320.0f, 380.0f, 60.0f, onRelease, ud, NULL);
api->SetText(h, "RADIO");
if(api->OpenMenu(1) == 0) { /* el menu 1 pasa a ser el visible */ }
api->CloseMenu();
bool hit = api->IsReleased(h);
api->RemoveWidget(h);
```

| Miembro | Qué hace |
|---|---|
| `AddButton(menu, texture, x, y, scale, onRelease, userdata, icon)` | Registra un `CWidgetButton` en el pool del juego. `menu=0` es el grupo ROOT, visible mientras no haya un menú abierto; `menu>=1` solo se ve mientras ese menú esté activo. `texture` es el nombre en la base de texturas del juego. `icon` es una ruta **absoluta** a un PNG RGBA, o `NULL` para la textura propia. Devuelve `NULL` si se agotó el pool. |
| `SetText(handle, text)` | Pinta una etiqueta ASCII con la fuente 5x7 embebida. Siempre gana sobre el icono. `NULL` o `""` la quita. |
| `OpenMenu(menu)` / `CloseMenu()` | Apilan y desapilan menús. Devuelven `0` o `-1`; si el grupo no cabe en los slots libres, el menú **no** se abre. |
| `IsReleased(handle)` | `true` una sola vez por release del dedo sobre el widget. |
| `RemoveWidget(handle)` | Libera el slot del pool. No-op con `NULL`. |

`x` e `y` son coordenadas de **origen** en el espacio virtual 640x448 del juego,
no normalizadas 0-1. El juego las escala a la pantalla real como
`(Origen ± Escala) * screen/640|448`. El botón de ataque nativo está en
`Origen(560,380) Escala(50,30)`, y el centro de la pantalla es `(320,224)`.

El registro siempre es diferido: el `CWidgetButton` real lo construye el pump del
motor en el primer `CGame::Process` posterior. Por eso llamar desde
`ON_MOD_LOAD` es seguro, aunque el sistema de texturas no exista todavía, y un
menú que no se ha abierto nunca se dibuja.

## Estado

- v4 (actual): `SetText` con fuente 5x7 embebida, iconos propios (v3), grupos de
  menú y `OpenMenu`/`CloseMenu` (v2), inyección por símbolo, dispatch fiel.
  Verificado en dispositivo arm64-v8a.
- Pendiente: `SetText` con fuente externa, limpieza de widgets huérfanos cuando
  el juego reconstruye el pool a mitad de una pulsación, paquetes `.amlp` de
  ejemplo.