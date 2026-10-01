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
  del juego y expone `GetMenuAPI()` (API v7).
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

## API v8 (`mod/menu-api.h`)

```c
const MenuKitAPI* api = MenuKit_GetAPI(aml);   // NULL si el framework no está
if(!api || api->version < 8) return;            // el cliente aborta limpio

void* h = api->AddButton(0, "shoot", 320.0f, 380.0f, 60.0f, onRelease, ud, NULL);
api->SetText(h, "RADIO");
api->SetSize(h, 220.0f, 90.0f);                 // px reales: rompe el cuadrado
api->SetVisible(h, 0);                           // sin coste de rebuild

/* v8: lienzo 2D inmediato. Se dibuja cada frame desde el tick.
   El reloj es del cliente: el framework no da tiempo porque cada cliente ya
   necesita el suyo para sus propios timers. */
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
}
api->SetTick(Tick, NULL);
```

| Miembro | Qué hace |
|---|---|
| `AddButton(menu, texture, x, y, scale, onRelease, userdata, icon)` | Registra un `CWidgetButton` en el pool del juego. `menu=0` es el grupo ROOT, visible mientras no haya un menú abierto; `menu>=1` solo se ve mientras ese menú esté activo. `texture` es el nombre en la base de texturas del juego. `icon` es una ruta **absoluta** a un PNG RGBA, o `NULL` para la textura propia. Devuelve `NULL` si se agotó el pool. |
| `RemoveWidget(handle)` | Libera el slot del pool. No-op con `NULL`. |
| `IsReleased(handle)` | `true` una sola vez por release del dedo sobre el widget. |
| `OpenMenu(menu)` / `CloseMenu()` | Apilan y desapilan menús. Devuelven `0` o `-1`; si el grupo no cabe en los slots libres, el menú **no** se abre. |
| `SetText(handle, text)` | Pinta una etiqueta ASCII con la fuente 5x7 embebida. Siempre gana sobre el icono. `NULL` o `""` la quita. |
| `SetTick(onTick, ud)` | Callback por frame, invocado desde el pump del framework. Es el **único** tick que AML expone (solo da PRELOAD/LOAD/UNLOAD/CRASH); lo que hace posibles los timers de auto-ocultado. |
| `SetAlpha(handle, 0..255)` | Alpha en widgets **vivos**. El framework re-afirma el valor cada frame desde el pump: una escritura única no basta, porque el update del motor corre dentro de `CGame_Process` y restauraba el byte. |
| `SetVisible(handle, 0/1)` | Oculto = no dibujado **y no táctil** (limpia el flag touch-immune). `SetAlpha(0)` solo para el `Draw`: el rect táctil sigue vivo y un botón invisible que se come taps es peor que no tenerlo. Preferido a Remove/Add para show-hide: ni uno ni otro liberan el objeto del widget (512 B por build), así que alternar por esa vía fuga en cada ciclo. |
| `SetSize(handle, w, h)` | Redimensiona en **píxeles reales** manteniendo el centro que calculó el motor. `AddButton` solo puede construir cuadrados porque su `scale` es un único `float` uniforme; `SetSize` es la vía para romper ese límite. Ojo: habla px reales mientras `AddButton` habla virtuales, así que **no** le pases coordenadas del layout. |
| `GetTap(x, y)` | `1` en el frame que **empieza** un tap (flanco de subida) y escribe la posición; `0` si no. Mantener el dedo es **un** tap, no uno por frame. Es la primitiva de "toca fuera para cerrar": AML no expone API de touch, así que un cliente solo podía reaccionar a taps sobre sus propios widgets. Devuelve `0` si no se resolvieron los globales. |
| `GetRect(handle, l, t, r, b)` | Rect vivo en pantalla, en **píxeles reales**. Empareja con `GetTap`: el hit-test son cuatro comparaciones, sin arithmetic de la proyección. `0` = handle desconocido o widget aún sin construir (preguntar el próximo frame). |

### v8: lienzo 2D inmediato

Retained (widgets) es solo la mitad de lo que necesitas. El lienzo es la otra:
formas y alpha arbitrarias, sin gastar slots del pool.

| Miembro | Qué hace |
|---|---|
| `DrawRect(x,y,w,h,rgba,filled)` | Rectángulo. `filled=1` relleno, `0` solo borde de 1 px. |
| `DrawQuad(x1..y4,rgba,filled)` | Cuadrilátero libre (deformado o en perspectiva). |
| `DrawTriangle(x1..y3,rgba,filled)` | Triángulo libre. |
| `DrawLine(x1,y1,x2,y2,rgba,width)` | Línea de grosor **real**: por debajo de 1 px va al raster directo; por encima se construye con dos triángulos, así que engordar una línea no cuesta un estado nuevo. |
| `DrawPoly(xy,count,rgba,filled,closed)` | Polígono desde un array de pares `x,y`. `filled` usa abanico de triángulos: válido para convexos y formas estrelladas, **no** para contornos que se cruzan. `closed=0` deja la última arista abierta. |
| `DrawCircle(cx,cy,r,rgba,filled,segments)` | Círculo, o anillo con `filled=0`. `segments` se limita a 3..128. |
| `SetDrawBlend(src,dst)` | Modo de blend (`rwBLEND*`). `-1` restaura el valor por defecto. |
| `ResetDrawState()` | Vuelve al blend por defecto (src-alpha / inv-src-alpha). |

**El contrato, que es lo importante:**

- **Inmediato, no retained.** No hay escena que mantener: lo que dibujas este frame
  *es* lo que existe. Para animar, cambias los números y **redibujas**. Por eso
  el lote se limpia al principio de cada frame, antes del tick.
- **Solo desde `SetTick`.** Las primitivas no dibujan: apilan geometría. El
  framework la vuelca entera en **una** llamada
  `RwIm2DRenderIndexedPrimitive` desde el hook de `Render2dStuff`, que es donde el
  motor ya tiene el raster 2D montado. Dibujar desde el pump de `CGame_Process`
  no funciona: corre antes de que exista ese contexto.
- **Píxeles reales**, el mismo espacio que `GetRect`/`GetTap`. Sin proyecciones.
- **`rgba` va empaquetado `0xRRGGBBAA`**, para animar un canal con un `<<`.
- **Techo del lote: 2048 vértices / 3072 índices por frame.** Al llenarse se
  descartan primitivas y se avisa por log una vez, en vez de romper el cliente.
- **Z-order: el lienzo se dibuja ENCIMA de los widgets de AML.** Se midió en
  dispositivo, no se supone: con el flush al final de la pasada 2D el arte tapaba
  los paneles de AML, y moverlo antes —al principio de la pasada, o al punto
  donde el motor dibuja sus propios widgets— dio exactamente lo mismo. AML
  renderiza su UI en otro punto del frame, fuera de la pasada 2D del juego, y
  anterior a los dos. Los dos intentos anteriores de cambiarlo **crashearon** el
  proceso: antes de que la pasada se ejecute no hay raster válido, así que no hay
  dónde dibujar.

  En la práctica: **pinta el panel entero con el lienzo** en vez de mezclar arte
  del lienzo con widgets de AML en la misma zona; si se solapan, gana el lienzo.

Es un canvas, no un DOM: no hay selectores ni layout. Las animaciones son
interpolar valores en el tick, que es justo lo que hace el ejemplo de
`examples/internet-radio` (panel con alpha que late, círculo que recorre,
triángulo que gira, línea que engorda).

`x` e `y` de `AddButton` son coordenadas de **origen** en el espacio virtual
640x448 del juego, no normalizadas 0-1. El botón de ataque nativo está en
`Origen(560,380) Escala(50,30)`, y el centro de la pantalla es `(320,224)`.

**Proyección (medida, no re-derives)** — posición y tamaño siguen reglas
**distintas**, y esa es la cosa más importante de esta API:

```
posición x : realRenderWidth  / 640 = 2.500
posición y : realRenderHeight / 448 = 1.607
tamaño     : realRenderWidth  / 640  (UNIFORME en AMBOS ejes)
```

Un widget sale **cuadrado** en píxeles reales (100x100 medido) aunque el espacio
virtual sea 640x448, porque el tamaño usa el factor de ancho para los dos ejes
mientras la posición usa un factor por eje. Como `scale` es un único `float`,
`AddButton` solo puede construir cuadrados.

Dos trampas que salen de esto:

1. **Los huecos verticales salen ~36% cortos.** La altura real de un widget
   (100 px) es mayor que su huella vertical virtual (40 unidades × 1.607 =
   64 px). Calcular separaciones verticales en unidades virtuales hace que las
   formas choquen "donde debería haber margen". Deriva los huecos de `GetRect`,
   no de las unidades virtuales.
2. **La resolución no se puede consultar.** `OS_ScreenGetWidth()` miente
   (reporta 1024x600 contra un render real de 1600x720). `GetRect` sobre un
   widget ya construido es la única fuente fiable.

Por eso `SetSize` es size-only en píxeles reales en vez de dar coordenadas
absolutas: no hay forma fiable de saber la escala para convertirlas.

El registro siempre es diferido: el `CWidgetButton` real lo construye el pump del
motor en el primer `CGame::Process` posterior. Por eso llamar desde
`ON_MOD_LOAD` es seguro, aunque el sistema de texturas no exista todavía, y un
menú que no se ha abierto nunca se dibuja.

### Patrón de adopción

Cuatro reglas que hacen que un cliente funcione en lugar de romperse:

1. **Gate por versión, no por offset.** `if(!api || api->version < 7) return;`.
   Cada miembro nuevo se añade **al final** del struct, así que un cliente viejo
   compilado contra v4 sigue leyendo por offset y funciona.
2. **No re-construir para show-hide ni para resize.** Usa `SetVisible`/`SetSize`;
   ambos se re-afirman cada frame y no gastan slots del pool.
3. **Hit-test en px reales con `GetTap` + `GetRect`**, nunca comparando con las
   coordenadas virtuales de `AddButton`: los dos espacios no se mezclan.
4. **El pool es de 190 slots y el juego ya ocupa ~177.** Reutiliza widgets; no
   los reconstruyas al cambiar de estado, o agotarás el pool.

## Estado

- v8 (actual): lienzo 2D inmediato — `DrawRect/Quad/Triangle/Line/Poly/Circle` con
  alpha y blend por vértice, loteados a una sola llamada RW por frame y volcados
  desde el hook de `Render2dStuff`; `GetTap` + `GetRect` en píxeles reales (v7)
  habilitan "toca fuera para cerrar"; `SetSize` rompe el límite de widget
  cuadrado; `SetAlpha`/`SetVisible` sobre widgets vivos; `SetTick` (v5) como
  único callback por frame; `SetText` con fuente 5x7 embebida (v4), iconos
  propios (v3), grupos de menú y `OpenMenu`/`CloseMenu` (v2), inyección por
  símbolo, dispatch fiel. v8 verificado en dispositivo arm64-v8a (las cuatro
  primitivas renderizan y animan).
- Pendiente: `SetText` con fuente externa, limpieza de widgets huérfanos cuando
  el juego reconstruye el pool a mitad de una pulsación, y paquetes `.amlp` de
  ejemplo.
- Techo conocido: el pool nativo es de 190 slots y el juego ocupa ~177, así que
  quedan ~13 libres. El lote del lienzo va aparte (2048 vértices por frame) y no
  consume pool: para formas y animation el pool dejó de ser el límite.