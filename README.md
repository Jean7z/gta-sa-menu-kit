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
  del juego y expone `GetMenuAPI()` (API v1).
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

## API v1 (`mod/menu-api.h`)

```c
const MenuKitAPI* api = MenuKit_GetAPI(aml);   // NULL si el framework no está

void* h = api->AddButton(texture, x, y, scale, onRelease, userdata);
bool  hit = api->IsReleased(h);
api->RemoveWidget(h);
```

- `AddButton` — crea un `CWidgetButton` en el pool del juego. `texture` es el
  sprite (ej. `"item_vehicle"`), `(x, y)` origen normalizado, `scale` factor.
  Devuelve un handle (NULL si el pool está lleno o se excede el límite de 16).
- `onRelease` — callback `void(*)(void* userdata)` invocado una vez por release
  del dedo sobre el widget (dispatched cada frame desde el hook de `CGame::Process`).
- `IsReleased` — consulta el estado de release sin callback.
- `RemoveWidget` — libera el slot en el pool (el objeto se deja al pump).

## Estado / roadmap

- v0.1 (actual): skeleton compilable, API v1, inyección por símbolo, dispatch fiel.
- v0.2: validar en dispositivo real (texturas de widget, límite real de slots,
  limpieza de objetos huérfanos al rebuild del pool), API multi-botón + texto,
  paquetes `.amlp` de ejemplo.