# SA Menu Kit

Framework de UI para mods de GTA SA en Android (AML). Da dos cosas: botones
registrados en el pool nativo de widgets del juego, y un lienzo 2D inmediato para
dibujar formas, alpha y texto sin gastar slots. Todo se resuelve por símbolo vía
IAML (`GetSym`), sin parchear bytes del binario.

**Solo arm64-v8a.** Los hooks están verificados contra el `libGTASA.so` de SA
2.10, que es arm64 puro.

## Compilar

El build necesita el submódulo `psdk` (Android Mod Loader PSDK).

```sh
git clone --recursive https://github.com/Jean7z/gta-sa-menu-kit
cd gta-sa-menu-kit
~/android-ndk-r29/ndk-build
# salida: libs/arm64-v8a/libAML_PSDK_MenuKit64.so
```

Si clonaste sin `--recursive`: `git submodule update --init --recursive`.
La ABI está fijada a arm64-v8a en `Application.mk`.

## Instalar

Los `.so` van en la carpeta de mods de AML, que en el dispositivo es
`Android/data/com.rockstargames.gtasa/mods/`. AML no carga de `files/psdk/`.

En Android 11+ esa carpeta no se puede escribir con `adb push` ni desde un gestor
de archivos, así que el paso normal es Shizuku (en Termux, `rish`):

```sh
~/shizuku/rish -c "cp libs/arm64-v8a/libAML_PSDK_MenuKit64.so \
  /storage/emulated/0/Android/data/com.rockstargames.gtasa/mods/"
```

Con root o en Android 10 o anterior, `adb push` a `/sdcard/Android/data/com.rockstargames.gtasa/mods/`
funciona directamente.

AML carga los mods en orden alfabético, así que `AML_PSDK_MenuKit64` carga antes
que cualquier cliente. Los clientes llaman a `MenuKit_GetAPI()`, que resuelve el
framework en runtime y devuelve `NULL` si todavía no está.

## API mínima

```c
const MenuKitAPI* api = MenuKit_GetAPI(aml);   // NULL si el framework no está
if(!api || api->version < 10) return;           // el cliente aborta limpio

void* h = api->AddButton(0, "shoot", 320.0f, 380.0f, 60.0f, onRelease, ud, NULL);
api->SetText(h, "RADIO");
api->SetSize(h, 220.0f, 90.0f);                 // px reales: rompe el cuadrado
api->SetVisible(h, 0);                           // sin coste de rebuild

/* Lienzo 2D inmediato. El reloj es del cliente: el framework no da tiempo
   porque cada cliente ya necesita el suyo para sus propios timers. */
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
    api->DrawText(60, 170, "MENUKIT V11", 2, 0x38BDF8FFu);       /* etiqueta */
}
api->SetTick(Tick, NULL);
```

## Referencia

### Widgets

| Miembro | Qué hace |
|---|---|
| `AddButton(menu, texture, x, y, scale, onRelease, userdata, icon)` | Registra un `CWidgetButton` en el pool del juego. `menu=0` es el grupo ROOT, visible mientras no haya un menú abierto; `menu>=1` solo se ve mientras ese menú esté activo. `texture` es el nombre en la base de texturas del juego. `icon` es una ruta **absoluta** a un PNG RGBA, o `NULL` para la textura propia. Devuelve `NULL` si se agotó el pool. |
| `RemoveWidget(handle)` | Libera el slot del pool. No-op con `NULL`. |
| `IsReleased(handle)` | `true` una sola vez por release del dedo sobre el widget. |
| `OpenMenu(menu)` / `CloseMenu()` | Apilan y desapilan menús. Devuelven `0` o `-1`; si el grupo no cabe en los slots libres, el menú no se abre. |
| `SetText(handle, text)` | Pinta una etiqueta ASCII con la fuente 5x7 embebida. Siempre gana sobre el icono. `NULL` o `""` la quita. |
| `SetTick(onTick, ud)` | Callback por frame, invocado desde el pump del framework. Es el único tick que AML expone (solo da `PRELOAD`/`LOAD`/`UNLOAD`/`CRASH`), y lo que hace posibles los timers de auto-ocultado. |
| `SetAlpha(handle, 0..255)` | Alpha en widgets vivos. El framework re-afirma el valor cada frame desde el pump: una escritura única no basta, porque el update del motor corre dentro de `CGame_Process` y restauraba el byte. |
| `SetVisible(handle, 0/1)` | Oculto = no dibujado y no táctil (limpia el flag touch-immune). `SetAlpha(0)` solo para el `Draw`: el rect táctil sigue vivo, y un botón invisible que se come taps es peor que no tenerlo. Preferido a Remove/Add para show-hide, porque Remove y Add liberan y reconstruyen el objeto del widget (512 B por build) y alternar por esa vía fuga en cada ciclo. |
| `SetSize(handle, w, h)` | Redimensiona en píxeles reales manteniendo el centro que calculó el motor. Es la vía para romper el límite de widget cuadrado de `AddButton`. Habla px reales mientras `AddButton` habla virtuales, así que no le pases coordenadas del layout. |
| `GetTap(x, y)` | `1` en el frame que **empieza** un tap (flanco de subida) y escribe la posición; `0` si no. Mantener el dedo es un tap, no uno por frame. Es la primitiva de "toca fuera para cerrar": AML no expone API de touch, así que un cliente solo podía reaccionar a taps sobre sus propios widgets. Devuelve `0` si no se resolvieron los globales. |
| `GetRect(handle, l, t, r, b)` | Rect vivo en pantalla, en píxeles reales. Empareja con `GetTap`: el hit-test son cuatro comparaciones, sin arithmetic de la proyección. `0` = handle desconocido o widget aún sin construir (preguntar el próximo frame). |
| `GetPointer(x, y, down)` | Estado vivo del puntero, consultable cada frame, en el mismo espacio de px reales que `GetTap` y `GetRect`. `GetTap` es un flanco de subida que desaparece al frame siguiente: sirve para un botón, no para un arrastre. Un slider, un swipe o un long-press necesitan saber si el dedo sigue abajo y dónde está ahora. `down=1` mientras hay dedo en la pantalla. `0` si no se resolvieron los globales. |
| `GetMenuUp()` | `1` si el juego tiene algún menú abierto, `0` si no, `-1` si no se encontró el símbolo. Existe porque con un menú abierto el dedo sigue siendo del juego, pero el lienzo del cliente ve ese mismo toque crudo, así que una UI dibujada encima se come los taps del menú. Un cliente con overlay debería ignorar entrada mientras devuelva `1`. Se resuelve por símbolo, sin offsets fijos. |

### Lienzo 2D inmediato

Las primitivas del lienzo no crean widgets: apilan geometría y la dibuja el
framework en un lote por frame.

| Miembro | Qué hace |
|---|---|
| `DrawRect(x,y,w,h,rgba,filled)` | Rectángulo. `filled=1` relleno, `0` solo borde de 1 px. |
| `DrawQuad(x1..y4,rgba,filled)` | Cuadrilátero libre (deformado o en perspectiva). |
| `DrawTriangle(x1..y3,rgba,filled)` | Triángulo libre. |
| `DrawLine(x1,y1,x2,y2,rgba,width)` | Línea de grosor real: por debajo de 1 px va al raster directo, por encima se construye con dos triángulos. |
| `DrawPoly(xy,count,rgba,filled,closed)` | Polígono desde un array de pares `x,y`. `filled` usa abanico de triángulos: válido para convexos y formas estrelladas, no para contornos que se cruzan. `closed=0` deja la última arista abierta. |
| `DrawCircle(cx,cy,r,rgba,filled,segments)` | Círculo, o anillo con `filled=0`. `segments` se limita a 3..128. |
| `DrawText(x, y, text, scale, rgba)` | Texto en el lienzo, sin widget. `x,y` es la esquina superior izquierda en px reales; `scale` es el multiplicador del glifo (1 = 5x7 crudo, 1..16); el avance es `6*scale` por carácter, así que el ancho es `len*6*scale` sin llamadas extra. Los caracteres fuera de `0x20..0x7E` salen como `'?'`. |
| `SetDrawBlend(src,dst)` | Modo de blend (`rwBLEND*`). `-1` restaura el valor por defecto. |
| `ResetDrawState()` | Vuelve al blend por defecto (src-alpha / inv-src-alpha). |

El contrato del lienzo:

- Es inmediato, no retained. No hay escena que mantener: lo que dibujas este frame
  es lo que existe. Para animar, cambias los números y redibujas. El lote se
  limpia al principio de cada frame, antes del tick.
- Solo desde `SetTick`. El framework vuelca la geometría entera en una llamada
  `RwIm2DRenderIndexedPrimitive` desde el hook de `Render2dStuff`, que es donde
  el motor ya tiene el raster 2D montado. Dibujar desde el pump de
  `CGame_Process` no funciona: corre antes de que exista ese contexto.
- Las coordenadas son píxeles reales, el mismo espacio que `GetRect` y `GetTap`.
- `rgba` va empaquetado `0xRRGGBBAA`, para animar un canal con un `<<`.
- Techo del lote: 4096 vértices y 6144 índices por frame. Al llenarse se
  descartan primitivas y se avisa por log una vez, en vez de romper el cliente.
- `DrawText` va al mismo lote que las formas, un quad por run horizontal de
  píxeles encendidos (unos 14 quads por glifo 5x7). Los glifos van sobre fondo
  transparente, así que se ve lo que el cliente pintó detrás. El orden de dibujo
  es el orden de llamada, también respecto a las formas: un rectángulo dibujado
  antes de una etiqueta queda debajo, uno dibujado después la tapa.

  No lo implementes con una textura de etiqueta por glifo: esa ruta se probó
  primero y en el dispositivo el sampler enlaza el raster pero no llega a
  muestrear los píxeles subidos, así que el texto salía vacío. La versión que
  funciona es la de quads sin texturizar.
- El lienzo se dibuja encima de los widgets de AML. Está medido en dispositivo:
  AML renderiza su UI en otro punto del frame, fuera de la pasada 2D del juego
  y anterior a los dos. Mover el flush antes de esa pasada dio lo mismo y los dos
  intentos crashearon, porque antes de que se ejecute no hay raster válido.

  En la práctica, pinta el panel entero con el lienzo en vez de mezclar arte del
  lienzo con widgets de AML en la misma zona. Si se solapan, gana el lienzo.

## Espacio de coordenadas

`x` e `y` de `AddButton` son coordenadas de origen en el espacio virtual 640x448
del juego, no normalizadas 0-1. El botón de ataque nativo está en
`Origen(560,380) Escala(20,20)`, y el centro de la pantalla es `(320,224)`.

Posición y tamaño siguen reglas distintas, y esto es lo que más sorprende al
usar la API:

```text
posición x : realRenderWidth  / 640 = 2.500
posición y : realRenderHeight / 448 = 1.607
tamaño     : realRenderWidth  / 640  (uniforme en ambos ejes)
```

Un widget sale cuadrado en píxeles reales (100x100 medido) aunque el espacio
virtual sea 640x448, porque el tamaño usa el factor de ancho para los dos ejes
mientras la posición usa un factor por eje. Como `scale` es un único `float`,
`AddButton` solo puede construir cuadrados.

Dos trampas que salen de esto:

1. Los huecos verticales salen ~36% cortos. La altura real de un widget (100 px)
   es mayor que su huella vertical virtual (40 unidades x 1.607 = 64 px).
   Calcular separaciones verticales en unidades virtuales hace que las formas
   choquen donde debería haber margen. Deriva los huecos de `GetRect`.
2. La resolución no se puede consultar. `OS_ScreenGetWidth()` miente: reporta
   1024x600 contra un render real de 1600x720. `GetRect` sobre un widget ya
   construido es la única fuente fiable.

Por eso `SetSize` es size-only en píxeles reales en vez de dar coordenadas
absolutas: no hay forma fiable de saber la escala para convertirlas.

El registro siempre es diferido: el `CWidgetButton` real lo construye el pump
del motor en el primer `CGame::Process` posterior. Por eso llamar desde
`ON_MOD_LOAD` es seguro, aunque el sistema de texturas no exista todavía, y un
menú que no se ha abierto nunca se dibuja.

## Reglas para clientes

1. Gate por versión, no por offset: `if(!api || api->version < 10) return;`.
   Cada miembro nuevo se añade al final del struct, así que un cliente viejo
   compilado contra v4 sigue leyendo por offset y funciona.
2. No reconstruyas para show-hide ni para resize. Usa `SetVisible` y `SetSize`;
   ambos se re-afirman cada frame y no gastan slots del pool.
3. Hit-test en px reales con `GetTap` y `GetRect`, nunca comparando con las
   coordenadas virtuales de `AddButton`: los dos espacios no se mezclan.

## Límites

- El pool nativo del juego tiene 190 slots y el juego ya ocupa unos 177, así que
  quedan unos 13 libres. Reutiliza widgets en vez de reconstruir al cambiar de
  estado. El lote del lienzo va aparte y no consume pool.
- Solo arm64-v8a. SA 2.00 trae arm7, pero esos offsets nunca se comprobaron
  contra su binario: un build v7a crashea en runtime, así que no se genera.
- v11 verificado en dispositivo arm64-v8a: texto legible, seek arrastrable con la
  etiqueta siguiendo el dedo, formas y alpha animando, y `GetMenuUp` haciendo que el
  launcher no se coma el toque con el que se cierra el mapa.
- Pendiente: `SetText` con fuente externa y limpieza de widgets huérfanos cuando
  el juego reconstruye el pool a mitad de una pulsación.
