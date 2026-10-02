/* Internet Radio - reproductor local de musica, cliente de la API v10 de SA Menu Kit.
   El boton recorre los ficheros de una carpeta y los decodifica el propio
   telefono (radio/meddec.c: AMediaExtractor + AMediaCodec), asi que suenan flac,
   m4a, opus, ogg... y no solo mp3. La eleccion de formato la hace el
   decodificador del sistema, no una lista nuestra.
   UI: una tarjeta de reproductor dibujada entera con el lienzo 2D del framework
   (sin widgets nativos y sin texturas), con hit-test propio a traves de
   GetPointer. Ver "UI" mas abajo.
   Todo lo que es CADENCIA y formato vive en radio/ (banco de host
   radio/host_test/test_sink.c); aqui solo se cablean start/stop, el salto de
   pista y los diagnosticos al parar.
   Pure consumer: all menu machinery lives in MenuKit.
   Requires the SA Menu Kit framework (AML_PSDK_MenuKit64) loaded first. */
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <mod/menu-api.h>

#include "radio/local.h"
#include "radio/cover.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>

MYMODCFG(net.psdk.samod.internetradio, Internet Radio, 0.4, Jean7z)

/* Carpeta de musica. Va en el external files dir DEL JUEGO, asi que el proceso
   puede leerla sin pedir ningun permiso: en Android 11+ el acceso al
   external files dir propio no necesita WRITE_EXTERNAL_STORAGE ni
   MANAGE_EXTERNAL_STORAGE. Copia aqui los ficheros y el boton los reproduce. */
#define RADIO_MUSIC_DIR "/sdcard/Android/data/com.rockstargames.gtasa/files/music"

/* Portadas decodificadas. El nombre del fichero lleva el hash de los bytes de la
   imagen (radio/cover.c), y eso NO es cosmetico: el motor cachea las texturas de
   icono POR RUTA, asi que la misma portada debe mapear al mismo fichero (una
   sola textura) y dos portadas distintas a ficheros distintos (si no, la segunda
   dibujaria la primera para siempre). */
#define RADIO_COVER_DIR "/sdcard/Android/data/com.rockstargames.gtasa/files/.covers"

/* --- UI ---------------------------------------------------------------------
   TODO se dibuja con el lienzo 2D, en PIXELES REALES (1600x720 medidos en
   dispositivo). No hay widgets nativos: los controles son hit-test propio
   sobre el puntero, que es lo unico que permite el lienzo inmediato.

   Lo que NO se puede hacer aqui, y por que:

   - Barra de progreso con porcentaje: la capa de radio (radio/local.h) NO
     expone duracion ni posicion. Una barra de progreso dibujada con un contador
     local seria una mentira (el valor no tendria nada que ver con el audio), asi
     que no hay barra. Lo que se dibuja es actividad real: las barras laten solo
     mientras el hilo de reproduccion esta metiendo muestras en el ring PCM.

   - Texto con textura: se probo y en el dispositivo el sampler enlaza el raster
     pero no llega a muestrear los pixeles subidos, asi que salia en blanco. El
     texto va por la ruta de quads sin texturizar (DrawText), que ya funciona.

   - Bordes redondeados con un fan de perimetro: parpadea. Por eso el relleno son
     4 cuartos de disco + 3 rectangulos (piezas convexas, sin strip) y el borde
     son 4 arcos de 90 como polilinea abierta + 4 lados rectos. No vale
     DrawCircle, que solo sabe dibujar el circulo COMPLETO: al trazar las 4
     esquinas aparecia tambien el arco interior de cada una.

   - Tamano de pantalla: OS_ScreenGetWidth() miente (reporto 1024x600 con un
     render real de 1600x720), asi que no se puede usar para colocar la tarjeta.
     Se mide con un widget sonda, ver ProbeScreen(). */
#define CARD_W          620.0f
#define CARD_H          184.0f
#define CARD_R           18.0f   /* radio de esquina */
#define CARD_PAD         24.0f
#define CARD_BOTTOM      28.0f   /* margen del borde inferior */
#define BTN_GAP          78.0f   /* separacion centro a centro */
#define BTN_R            22.0f   /* radio visual de prev/next */
#define BTN_R_PLAY       30.0f   /* el play es el primario, se ve mas grande */
#define BTN_TOUCH_PAD     8.0f   /* margen extra de hit-test (dedo, no raton) */

/* Lanzador replegado. Con la tarjeta cerrada solo se ve este boton-redondo, una
   nota musical que abre el reproductor. Va abajo al centro, justo donde aparece
   la tarjeta, para que al abrir y cerrar no salte de un sitio a otro. El color
   dice si esta sonando algo, para que no haya que abrirlo a mirar. */
#define ICO_R           38.0f   /* radio del boton */
#define ICO_MARGIN      40.0f   /* margen respecto al borde de la pantalla */
/* Fundido por inactividad. Pasa un rato sin tocar nada y el icono se atenua hasta
   un piso: se intuye que sigue ahi sin robar sitio a la partida, pero nunca llega
   a desaparecer (si desaparece, no se sabria donde tocar para despertarlo). Al
   pulsarlo vuelve a su aspecto. */
#define ICO_FADE_DELAY 6000.0f  /* ms sin presion antes de empezar a apagarse */
#define ICO_FADE_TIME   900.0f  /* ms que dura el fundido */
#define ICO_FADE_MIN    0.38f   /* piso de opacidad */

#define R_BG          0xE60B1220u   /* tarjeta, casi opaca */
#define R_EDGE        0xFF1E3A5Fu   /* borde de la tarjeta */
#define R_ACCENT      0xFF38BDF8u   /* activo / en juego */
#define R_TEXT        0xFFE2E8F0u   /* titulo */
#define R_DIM         0xFF64748Bu   /* metadatos e iconos inactivos */
#define R_LIVE        0xFF0EA5E9u   /* barras con audio fluyendo */

/* Longitud maxima del nombre de pista que muestra el log y la tarjeta. */
#define NAME_MAX_CHARS 40

static const MenuKitAPI* s_api = NULL;
static RadioLocal* s_local = NULL;
static bool s_radioOn = false;
static bool s_paused  = false;

static char  s_dispName[NAME_MAX_CHARS + 1];
static pthread_mutex_t s_nameLock = PTHREAD_MUTEX_INITIALIZER;

/* Sonda de tamano de pantalla: un widget invisible y cuadrado, centrado en el
   centro del espacio virtual (320,224), que el motor proyecta al centro EXACTO
   del render. Con su rect en pixeles reales sale el ancho y el alto. */
static void*  s_probe     = NULL;
static float  s_screenW   = 0.0f;
static float  s_screenH   = 0.0f;
static uint64_t s_probeAt  = 0;

/* Geometria de la tarjeta del frame actual (la escribe LayoutCard). */
static float s_cardX, s_cardY, s_bxPrev, s_bxPlay, s_bxNext, s_bY;
/* Geometria del lanzador replegado y del boton que repliega la tarjeta. Se
   calculan siempre, aunque solo se dibujen cuando toca: el hit-test necesita las
   coordenadas igual que el dibujo. */
static float s_icoX, s_icoY, s_closeX, s_closeY;
static bool  s_cardOpen = false;   /* la tarjeta arranca replegada */
static uint64_t s_idleAt = 0;      /* ms de la ultima presion sobre nuestra UI */
static uint64_t s_tickPrev = 0;    /* ms del tick anterior, para detectar parones */
static int   s_press = 0;      /* boton pulsado: ver B_* */
static int   s_wasDown = 0;    /* flanco de bajada del puntero */
/* Nivel del medidor. El delta de muestras se promedia en una ventana de 200 ms en
   lugar de mirar el frame: el decodificador entrega un chunk por cada ~186 ms de
   audio, asi que un valor por frame alternaba "hay chunk" / "no hay chunk" y las
   barras parpadeaban. Con la ventana el nivel cambia como mucho 5 veces por
   segundo y ademas se recorre en rampa, asi que el movimiento es continuo. La
   senal es la misma de antes: el total de muestras solo crece si el hilo de audio
   esta escribiendo. */
static float s_flow = 0.0f;          /* nivel actual, 0..1 */
static float s_flowT = 0.0f;         /* objetivo del ultimo cierre de ventana */
static long long s_winPrev = 0;      /* muestras al abrir la ventana */
static uint64_t s_winAt = 0;         /* ms en que se abrio la ventana */

#define B_BTN_PREV 1
#define B_BTN_PLAY 2
#define B_BTN_NEXT 3
#define B_BTN_CLOSE 4    /* repliega la tarjeta */
#define B_ICO_OPEN  5    /* el icono de nota despliega la tarjeta */

/* Reloj de milisegundos. El unico con el que se puede animar: AML no da hook por
   frame (solo PRELOAD/LOAD/UNLOAD/CRASH), asi que el pump de CGame_Process es
   lo que llama a SetTick, una vez por frame. */
static uint64_t NowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* radio/local.c es C portable y no conoce el logger del mod: se le pasa este
   puente, que es lo unico de este archivo que el hilo de reproduce puede tocar. */
static void RadioLog(void* ctx, const char* fmt, ...)
{
    char buf[288];
    va_list ap;
    (void)ctx;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logger->Info("%s", buf);
}

/* Diagnostico al parar: es el momento en que el usuario acaba de oir (o no) la
   carpeta, y los datos estan disponibles sin hilo ni polling extra. */
static void RadioLogStats(void)
{
    logger->Info("InternetRadio: fin -> %d temas, %lld muestras, %d underruns, %d cambios de formato",
                 radio_local_tracks(s_local), radio_local_samples(s_local),
                 radio_local_underruns(s_local), radio_local_fmt_changes(s_local));
}

/* Copia solo 0x20..0x7E, que es todo lo que la fuente 5x7 del motor sabe pintar.
   Un nombre de fichero con acentos llega aqui crudo desde el sistema de
   ficheros, y el motor los dibujaria como '?'. */
static void CopyAsciiClamped(const char* in, char* out, size_t out_len)
{
    size_t w = 0;
    if(!out_len) return;
    if(in)
    {
        for(; *in && w + 1 < out_len; in++)
        {
            unsigned char c = (unsigned char)*in;
            if(c >= 0x20u && c <= 0x7Eu) out[w++] = (char)c;
        }
    }
    while(w > 0 && out[w - 1] == ' ') w--;
    out[w] = 0;
}

/* Nombre del tema en curso: el del metadata si lo hay y, si no, el del fichero
   sin extension (un WAV no lleva titulo y "A Mi - Rels B" se lee mucho mejor
   que "A Mi - Rels B.wav"). */
static void SetDisplayName(const char* meta_title, const char* file_name)
{
    char buf[128];
    char* dot;

    if(meta_title && *meta_title)
    {
        snprintf(buf, sizeof buf, "%s", meta_title);
    }
    else if(file_name)
    {
        snprintf(buf, sizeof buf, "%s", file_name);
        dot = strrchr(buf, '.');
        if(dot && dot != buf) *dot = 0;
    }
    else
    {
        buf[0] = 0;
    }

    pthread_mutex_lock(&s_nameLock);
    CopyAsciiClamped(buf, s_dispName, sizeof s_dispName);
    pthread_mutex_unlock(&s_nameLock);
}

/* Mide el render real con un widget sonda y lo libera. OS_ScreenGetWidth() no
   sirve: dio 1024x600 con un render de 1600x720. El widget va centrado en el
   centro del espacio virtual (320,224), que el motor proyecta al centro del
   render, asi que con el borde izquierdo de un cuadrado de 100 px sale el ancho
   (W = (left + 50) * 2) y con el superior el alto. Se descarta en cuanto se
   logra, y si el motor no llegara a construirlo se cae al tamano medido en este
   dispositivo para no dejar la UI sin pintar. */
static void ProbeScreen(void)
{
    float l = 0.0f, t = 0.0f, r = 0.0f, b = 0.0f;
    uint64_t now = NowMs();

    if(s_screenW > 0.0f) return;

    if(s_probe)
    {
        if(s_api->GetRect(s_probe, &l, &t, &r, &b))
        {
            s_screenW = (l + 50.0f) * 2.0f;
            s_screenH = (t + 50.0f) * 2.0f;
            s_api->RemoveWidget(s_probe);
            s_probe = NULL;
            logger->Info("InternetRadio: render medido %.0fx%.0f px", s_screenW, s_screenH);
            return;
        }
        if(now - s_probeAt < 1500u) return;
    }

    s_screenW = 1600.0f;
    s_screenH = 720.0f;
    if(s_probe) { s_api->RemoveWidget(s_probe); s_probe = NULL; }
    logger->Error("InternetRadio: la sonda no dio rect; asumo %.0fx%.0f",
                  s_screenW, s_screenH);
}

/* Recorta a max_chars y remata con ".." si sobra texto. El ancho de la fuente es
   len*6*scale, asi que el recorte es lo que evita que el titulo se salga de la
   tarjeta. */
static void FitText(const char* in, char* out, size_t out_len, int max_chars)
{
    int n = 0;
    if(!out_len) return;
    if(in)
        while(in[n] && n < max_chars && (size_t)n + 1 < out_len)
        { out[n] = in[n]; ++n; }
    if(in && in[n] && n == max_chars && n >= 2)
    { n -= 2; out[n++] = '.'; out[n++] = '.'; }
    out[n] = 0;
}

/* Puntos de un arco de 90 grados, del angulo a0 a a0+90 (screen: y crece hacia
   abajo, asi que los angulos van al reves que en matematicas). Se usan
   poligonos, no DrawCircle: la API solo trae el circulo COMPLETO, y al trazar
   sus 4 esquinas se dibujaba tambien el arco interior de cada una. Con
   DrawPoly sale el cuarto exacto. */
#define ARC_SEGS 8
static int ArcPts(float cx, float cy, float r, float a0, float* xy)
{
    int i, n = 0;
    for(i = 0; i <= ARC_SEGS; ++i)
    {
        const float a = a0 + (float)i * (3.14159265f * 0.5f / (float)ARC_SEGS);
        xy[n * 2]     = cx + r * cosf(a);
        xy[n * 2 + 1] = cy + r * sinf(a);
        ++n;
    }
    return n;
}

/* Cuarto de disco relleno: el abanico del lienzo triangula desde el primer
   vertice, asi que basta con poner el CENTRO del arco delante y luego los
   puntos del arco. Lleno el cuarto exacto, sin salirse hacia dentro. */
static void FillQuarterDisc(float cx, float cy, float r, float a0, unsigned int c)
{
    float xy[2 * (ARC_SEGS + 2)];
    int n = 0;
    xy[n * 2] = cx; xy[n * 2 + 1] = cy; ++n;              /* centro: ancla del fan */
    n += ArcPts(cx, cy, r, a0, xy + n * 2);
    s_api->DrawPoly(xy, n, c, 1, 1);
}

/* Cuarto de arco como simple linea: closed=0 deja que Canvas_Stroke no cierre
   la polilinea, que es justo lo que aqui no queremos (cerrarla pintaria la
   cuerda interior). */
static void StrokeQuarterArc(float cx, float cy, float r, float a0, unsigned int c)
{
    float xy[2 * (ARC_SEGS + 1)];
    const int n = ArcPts(cx, cy, r, a0, xy);
    s_api->DrawPoly(xy, n, c, 0, 0);
}

/* Relleno redondeado: 4 cuartos de disco + los 3 rectangulos centrales. Ahora las
   piezas NO se solapan (los cuartos de disco se quedan dentro de su cuadrado de
   esquina), asi que el relleno translucido ya no se compone dos veces y las
   esquinas no se ensucian. Todas las piezas son convexas: lo que el abanico de
   triangulos del lienzo sabe dibujar sin titilar. */
static void FillRoundRect(float x, float y, float w, float h, float r, unsigned int c)
{
    const float PI_2 = 3.14159265f * 0.5f;
    FillQuarterDisc(x + r,     y + r,     r, PI_2 * 2.0f,        c);   /* sup izq */
    FillQuarterDisc(x + w - r, y + r,     r, PI_2 * 3.0f, c);   /* sup der */
    FillQuarterDisc(x + w - r, y + h - r, r, 0.0f,       c);   /* inf der */
    FillQuarterDisc(x + r,     y + h - r, r, PI_2,       c);   /* inf izq */
    s_api->DrawRect(x + r, y,         w - 2.0f * r, h, c, 1);
    s_api->DrawRect(x,     y + r,     r,           h - 2.0f * r, c, 1);
    s_api->DrawRect(x + w - r, y + r, r,           h - 2.0f * r, c, 1);
}

/* Borde: 4 arcos de 90 grados + 4 lados rectos. Los arcos van como polilinea
   abierta, no como circulo completo: DrawCircle(..., filled=0) traza los 360
   grados y por eso se veia el arco interior de cada esquina. */
static void StrokeRoundRect(float x, float y, float w, float h, float r, unsigned int c)
{
    const float PI_2 = 3.14159265f * 0.5f;
    StrokeQuarterArc(x + r,         y + r,     r, PI_2 * 2.0f,          c);
    StrokeQuarterArc(x + w - r,     y + r,     r, PI_2 * 3.0f, c);
    StrokeQuarterArc(x + w - r,     y + h - r, r, 0.0f,        c);
    StrokeQuarterArc(x + r,         y + h - r, r, PI_2,        c);
    s_api->DrawLine(x + r, y,         x + w - r, y,         c, 1.0f);
    s_api->DrawLine(x + r, y + h,     x + w - r, y + h,     c, 1.0f);
    s_api->DrawLine(x,     y + r,     x,         y + h - r, c, 1.0f);
    s_api->DrawLine(x + w, y + r,     x + w,     y + h - r, c, 1.0f);
}

static void DrawPlayGlyph(float cx, float cy, float r, unsigned int c)
{
    const float h = r * 1.15f;
    s_api->DrawTriangle(cx - r*0.45f, cy - h*0.5f,
                        cx - r*0.45f, cy + h*0.5f,
                        cx + r*0.85f, cy, c, 1);
}

static void DrawPauseGlyph(float cx, float cy, float r, unsigned int c)
{
    const float bw = r * 0.32f, bh = r * 1.10f;
    s_api->DrawRect(cx - r*0.60f, cy - bh*0.5f, bw, bh, c, 1);
    s_api->DrawRect(cx + r*0.28f, cy - bh*0.5f, bw, bh, c, 1);
}

/* Doble chevron: la convencion de "pista anterior/siguiente". dir = -1 izquierda,
   +1 derecha. Cada triangulo lleva el apex hacia dir y la base hacia el lado
   contrario; el segundo se desplaza hacia fuera para que se lean los dos. */
static void DrawSkipGlyph(float cx, float cy, float r, int dir, unsigned int c)
{
    const float h  = r * 0.80f;
    const float s  = (float)dir;          /* +1 mira a la derecha */
    const float a1 = cx + 0.05f * r * s;
    const float b1 = cx - 0.75f * r * s;
    const float a2 = cx + 0.60f * r * s;
    const float b2 = cx - 0.20f * r * s;
    s_api->DrawTriangle(a1, cy - h, b1, cy, a1, cy + h, c, 1);
    s_api->DrawTriangle(a2, cy - h, b2, cy, a2, cy + h, c, 1);
}

/* Chevron de dos segmentos, la convencion de "sube/baja": en el lanzador es "abre
   la tarjeta" y en la tarjeta es "repliegala". up = 1 apunta arriba. */
static void DrawChevronGlyph(float cx, float cy, float r, int up, unsigned int c)
{
    const float d = r * 0.60f;
    const float s = (float)(up ? 1 : -1);
    s_api->DrawLine(cx - d, cy + d * s, cx, cy - d * s, c, 1.0f);
    s_api->DrawLine(cx,     cy - d * s, cx + d, cy + d * s, c, 1.0f);
}

/* Nota musical: dos corcheas con la barra encima. Dos cabezas (circulo relleno),
   dos plumas y la barra (lineas). Aqui DrawCircle si sirve relleno plano, que es
   justo lo que se busca; el problema del circulo completo era del BORDE. */
static void DrawNoteGlyph(float cx, float cy, float r, unsigned int c)
{
    const float hw = r * 0.30f;        /* radio de cabeza */
    const float dx = r * 0.40f;        /* separacion entre cabezas */
    const float ax = cx - dx, bx = cx + dx;
    const float by = cy + r * 0.50f;    /* linea donde viven las cabezas */
    s_api->DrawCircle(ax, by, hw, c, 1, 16);
    s_api->DrawCircle(bx, by, hw, c, 1, 16);
    s_api->DrawLine(ax, by - hw * 0.5f, ax, cy - r * 0.70f, c, 1.0f);
    s_api->DrawLine(bx, by - hw * 0.5f, bx, cy - r * 1.00f, c, 1.0f);
    s_api->DrawLine(ax, cy - r * 0.70f, bx, cy - r * 1.00f, c, 1.0f);
}

static int  RadioStart(void);
static void RadioStop(void);
static void RadioSetPaused(int paused);

static float HitRadius(int id)
{
    /* Cada boton tiene su radio visual y el acierto se amplia un poco, porque lo
       que toca es un dedo y no un raton. */
    if(id == B_BTN_PLAY) return BTN_R_PLAY + BTN_TOUCH_PAD;
    if(id == B_ICO_OPEN)  return ICO_R + BTN_TOUCH_PAD;
    return BTN_R + BTN_TOUCH_PAD;
}

/* Un rectangulo de toque alrededor del centro. Cuadrado y no circulo porque es lo
   que espera un dedo, y con BTN_TOUCH_PAD de margen el acierto no exige punteria. */
static int HitTest(float px, float py, float cx, float cy, int id)
{
    const float r = HitRadius(id);
    return px >= cx - r && px <= cx + r && py >= cy - r && py <= cy + r;
}

static void LayoutCard(void)
{
    const float cx = s_screenW * 0.5f;
    s_cardX = cx - CARD_W * 0.5f;
    s_cardY = s_screenH - CARD_BOTTOM - CARD_H;
    s_bxPrev = cx - BTN_GAP;
    s_bxPlay = cx;
    s_bxNext = cx + BTN_GAP;
    s_bY     = s_cardY + CARD_H - 48.0f;
    /* El chevron de replegar va al extremo derecho de la fila de botones: a la
       derecha de "next" sobra sitio de sobra, y la esquina de arriba esta
       ocupada por el titulo a la izquierda y el estado a la derecha. */
    s_closeX = s_cardX + CARD_W - CARD_PAD - 14.0f;
    s_closeY = s_bY;

    /* Lanzador abajo al centro: es donde sale la tarjeta, asi que abrir y cerrar
       no salta de un sitio a otro, y queda a tiro de los dos pulgares. */
    s_icoX = s_screenW * 0.5f;
    s_icoY = s_screenH - ICO_MARGIN - ICO_R;
}

/* Hit-test y accion. El flanco lo da el estado ANTERIOR del puntero: sin el, un
   dedo que se queda apoyado sobre el play lo alternaria en cada frame. */
/* Cuanto se ve el lanzador ahora mismo: 1.0 recien tocado, bajando hasta
   ICO_FADE_MIN pasado ICO_FADE_DELAY sin que se toque nada. */
static float IconFade(uint64_t now)
{
    float t;
    if(now <= s_idleAt || now - s_idleAt <= (uint64_t)ICO_FADE_DELAY) return 1.0f;
    t = (float)(now - s_idleAt - (uint64_t)ICO_FADE_DELAY) / ICO_FADE_TIME;
    if(t > 1.0f) t = 1.0f;
    return 1.0f - (1.0f - ICO_FADE_MIN) * t;
}

static void HandleInput(uint64_t now)
{
    float px = 0.0f, py = 0.0f;
    int down = 0, id = 0;

    if(!s_api->GetPointer(&px, &py, &down)) { s_wasDown = down; s_press = 0; return; }

    /* Con el menu del juego abierto el dedo es suyo, no nuestro. El raw touch
       sigue llegando aqui mientras el mapa esta abierto, asi que sin esta puerta
       el toque con el que se cierra el mapa se lee como un tap en nuestro icono
       y nos abre el reproductor. GetMenuUp lee la pila de screens del propio
       juego, asi que es su definicion de "ocupado", no una heuristica.

       Salimos sin interpretar nada y resincronizamos el flanco con el estado
       real del dedo, para que el toque de cierre no quede pendiente. */
    if(s_api->GetMenuUp && s_api->GetMenuUp() == 1)
    {
        s_wasDown = down;
        s_press = 0;
        s_tickPrev = now;
        return;
    }

    /* Respaldo para un framework viejo que no exponga GetMenuUp (-1): si el pump
       dejo de correr, el juego abrio su propia UI mientras tanto y el flanco que
       vemos no es nuestro. */
    if(s_api->GetMenuUp && s_api->GetMenuUp() < 0
       && now > s_tickPrev && now - s_tickPrev > 250u) s_wasDown = 1;
    s_tickPrev = now;

    if(down)
    {
        if(s_cardOpen)
        {
            if     (HitTest(px, py, s_bxPrev, s_bY,     B_BTN_PREV))  id = B_BTN_PREV;
            else if(HitTest(px, py, s_bxPlay, s_bY,     B_BTN_PLAY))  id = B_BTN_PLAY;
            else if(HitTest(px, py, s_bxNext, s_bY,     B_BTN_NEXT))  id = B_BTN_NEXT;
            else if(HitTest(px, py, s_closeX, s_closeY, B_BTN_CLOSE)) id = B_BTN_CLOSE;
        }
        else if(HitTest(px, py, s_icoX, s_icoY, B_ICO_OPEN)) id = B_ICO_OPEN;
    }

    if(down && !s_wasDown && id)
    {
        /* Cualquier toque sobre nuestra UI reinicia el fundido. Un toque abre
           siempre, este o no apagado: pedir dos golpes para lo mismo es
           engorroso. El apagado es solo estetica, no una puerta. */
        s_idleAt = now;
        if(id == B_BTN_CLOSE)     s_cardOpen = false;
        else if(id == B_ICO_OPEN) s_cardOpen = true;
        else if(id == B_BTN_PREV) radio_local_skip(s_local, -1);
        else if(id == B_BTN_NEXT) radio_local_skip(s_local, +1);
        else if(id == B_BTN_PLAY)
        {
            if(!s_radioOn) RadioStart();
            else RadioSetPaused(!s_paused);
        }
    }

    s_press = id;
    s_wasDown = down;
}

static void DrawCard(uint64_t now)
{
    char title[64], meta[32], stat[16];
    const float t2 = 12.0f;           /* avance a escala 2 (metadatos) */
    const float t3 = 18.0f;           /* avance a escala 3 (titulo) */
    int idx, count;
    float nameY, barsY, i, amp;

    /* --- tarjeta --- */
    FillRoundRect(s_cardX, s_cardY, CARD_W, CARD_H, CARD_R, R_BG);
    StrokeRoundRect(s_cardX, s_cardY, CARD_W, CARD_H, CARD_R, R_EDGE);

    /* --- titulo: el nombre real de la pista --- */
    pthread_mutex_lock(&s_nameLock);
    if(s_dispName[0])
        FitText(s_dispName, title, sizeof title, (int)((CARD_W - 2*CARD_PAD) / t3));
    else
        snprintf(title, sizeof title, "%s", s_radioOn ? "SCANNING" : "TAP PLAY");
    pthread_mutex_unlock(&s_nameLock);

    /* --- metadatos: posicion en la lista + estado --- */
    idx   = s_local ? radio_local_index(s_local) : -1;
    count = s_local ? radio_local_count(s_local) : 0;
    if(idx >= 0 && count > 0) snprintf(meta, sizeof meta, "%d/%d", idx + 1, count);
    else                      snprintf(meta, sizeof meta, "%d TRACKS", count);

    nameY = s_cardY + 22.0f;
    s_api->DrawText(s_cardX + CARD_PAD, nameY, title, 3, R_TEXT);
    s_api->DrawText(s_cardX + CARD_PAD, nameY + 34.0f, meta, 2, R_DIM);

    snprintf(stat, sizeof stat, "%s", s_paused ? "PAUSED" : (s_radioOn ? "PLAYING" : "STOPPED"));
    s_api->DrawText(s_cardX + CARD_W - CARD_PAD - strlen(stat) * t2, nameY + 34.0f,
                    stat, 2, s_paused ? R_DIM : (s_radioOn ? R_ACCENT : R_DIM));

    /* --- actividad: cinco barras que laten SOLO si el hilo de audio esta
           metiendo muestras en el ring. Sin duracion ni posicion, esta es la
           unica lectura honesta del estado de la reproduccion.
           s_flow ya viene suavizado (sube a tope con audio, decae sin el), asi
           que la amplitud y el color salen de el directo. --- */
    amp = 2.0f + 13.0f * s_flow;
    barsY = s_cardY + 104.0f;
    for(i = 0; i < 5; ++i)
    {
        const float ph = (float)(now % 1100) * 0.0057f + i * 0.85f;
        const float sw = 0.5f + 0.5f * sinf(ph);
        const float bh = 3.0f + amp * (0.25f + 0.75f * sw);
        s_api->DrawRect(s_cardX + CARD_PAD + i * 11.0f, barsY - bh, 6.0f, bh,
                        s_flow > 0.05f ? R_LIVE : R_DIM, 1);
    }

    /* --- controles: play y pause nunca coexisten, el estado real decide --- */
    DrawSkipGlyph(s_bxPrev, s_bY, BTN_R, -1, s_press == B_BTN_PREV ? R_ACCENT : R_DIM);
    DrawSkipGlyph(s_bxNext, s_bY, BTN_R, +1, s_press == B_BTN_NEXT ? R_ACCENT : R_DIM);
    if(s_radioOn && !s_paused) DrawPauseGlyph(s_bxPlay, s_bY, BTN_R_PLAY, R_ACCENT);
    else                      DrawPlayGlyph(s_bxPlay, s_bY, BTN_R_PLAY, R_TEXT);
    DrawChevronGlyph(s_closeX, s_closeY, BTN_R, 0, s_press == B_BTN_CLOSE ? R_ACCENT : R_DIM);
}

/* Modula el canal alpha de un color 0xAARRGGBB. El fundido del lanzador es lo
   unico que lo necesita; el resto de la UI dibuja opaca. */
static unsigned int FadeAlpha(unsigned int c, float k)
{
    const unsigned int a = (unsigned int)((float)((c >> 24) & 0xFFu) * k);
    return (a << 24) | (c & 0x00FFFFFFu);
}

/* Lanzador replegado. Con la tarjeta cerrada solo se ve esto: un boton-redondo
   con una nota musical que la abre. El borde va con DrawCircle a proposito: aqui
   si queremos el circulo COMPLETO, que es el anillo del boton. */
static void DrawLauncher(uint64_t now)
{
    /* Aleta: color de acento mientras suena, apagado si esta parado. Asi el
       icono dice si hay musica sin abrir el reproductor. */
    const float k = IconFade(now);
    const unsigned int c = (s_radioOn && !s_paused) ? R_ACCENT : R_DIM;
    s_api->DrawCircle(s_icoX, s_icoY, ICO_R, FadeAlpha(R_BG, k), 1, 40);
    s_api->DrawCircle(s_icoX, s_icoY, ICO_R, FadeAlpha(R_EDGE, k), 0, 40);
    DrawNoteGlyph(s_icoX, s_icoY, ICO_R * 0.52f,
                  FadeAlpha(s_press == B_ICO_OPEN ? R_TEXT : c, k));
}

static void OnTick(void* userdata)
{
    long long samples;
    uint64_t now;
    (void)userdata;
    if(!s_api) return;
    now = NowMs();

    ProbeScreen();
    if(s_screenW <= 0.0f) return;   /* todavia no se sabe cuanto mide la pantalla */

    LayoutCard();
    HandleInput(now);

    /* Actividad real del audio: el total de muestras solo crece si el hilo de
       reproduccion esta escribiendo, asi que su crecimiento es la senal de "suena".
       Cuando el decodificador se atasca, las barras se quedan planas.
       El crecimiento llega a trozos (un chunk por cada ~186 ms de audio), asi que
       se cierra una ventana cada 200 ms en vez de mirar el frame: mirar el frame
       hacia que el nivel alternase chunk/no-chunk y las barras parpadeasen. Luego
       se recorre hacia el objetivo en rampa (sube mas rapido que baja, que es lo
       que se ve natural) y las barras se mueven sin saltos. */
    samples = s_local ? radio_local_samples(s_local) : 0;
    if(now - s_winAt >= 200u)
    {
        s_flowT = (s_radioOn && !s_paused && samples > s_winPrev) ? 1.0f : 0.0f;
        s_winPrev = samples;
        s_winAt    = now;
    }
    s_flow += (s_flowT - s_flow) * (s_flowT > s_flow ? 0.35f : 0.12f);
    if(s_flow < 0.0f) s_flow = 0.0f;
    if(s_flow > 1.0f) s_flow = 1.0f;

    if(s_cardOpen) DrawCard(now);
    else           DrawLauncher(now);
}

/* HILO DE AUDIO. Corren en el hilo de reproduccion, justo al abrir cada pista,
   con 1.49 s de cushion en el ring PCM, asi que los ~100 ms de JNI + escritura
   del fichero de portada no se oyen. La API de MenuKit solo admite llamadas de
   widget desde el hilo del juego en la practica, pero SetText/SetVisible solo
   voltean buffers (el pump repinta al frame siguiente), asi que aqui aguanta. */
static void OnTrackChanged(void* ctx, const char* name, const char* path)
{
    char cover[512];
    char title[80];
    int  rc;

    (void)ctx;
    cover[0] = 0;
    title[0] = 0;

    rc = radio_cover_extract(aml->GetJNIEnvironment(), path, RADIO_COVER_DIR,
                             cover, sizeof cover, title, sizeof title);
    if(rc < 0)
        logger->Error("InternetRadio: sin API de Java para la portada de %s", name);
    else if(!cover[0])
        logger->Info("InternetRadio: %s no trae portada", name);
    else
        logger->Info("InternetRadio: portada cacheada %s", cover);

    SetDisplayName(title, name);
}

static int RadioStart(void)
{
    if(s_radioOn) return 0;
    if(!s_local) return -1;
    if(radio_local_start(s_local) != 0)
    {
        logger->Error("InternetRadio: no se pudo arrancar la reproduccion");
        return -1;
    }
    s_radioOn = true;
    s_paused  = false;
    logger->Info("InternetRadio: ON -> %s", RADIO_MUSIC_DIR);
    return 0;
}

/* Pausa de verdad: el hilo y el decodificador siguen vivos, asi que reanudar
   continua en el punto exacto. Antes esto era stop() + start(), que reiniciaba
   la lista desde la primera pista al volver a pulsar. */
static void RadioSetPaused(int paused)
{
    if(!s_radioOn || !s_local) return;
    if(s_paused == (paused != 0)) return;
    radio_local_pause(s_local, paused ? 1 : 0);
    s_paused = (paused != 0);
    s_flow = 0.0f;               /* el medidor arranca plano al reanudar */
    logger->Info("InternetRadio: %s", paused ? "PAUSE" : "RESUME");
}

static void RadioStop(void)
{
    if(!s_radioOn) return;
    radio_local_stop(s_local);
    RadioLogStats();
    s_radioOn = false;
    s_paused  = false;
    s_flow = 0.0f;
    logger->Info("InternetRadio: OFF");
}

ON_MOD_LOAD()
{
    logger->SetTag("InternetRadio");

    s_api = MenuKit_GetAPI(aml);
    if(!s_api)
    {
        logger->Error("InternetRadio: MenuKit framework not loaded (load AML_PSDK_MenuKit64 first)");
        return;
    }
    /* La UI es lienzo puro con DrawText y GetPointer: v10 es el minimo. */
    if(s_api->version < 10)
    {
        logger->Error("InternetRadio: MenuKit API v10 required (got v%u) - actualiza AML_PSDK_MenuKit64",
                      s_api->version);
        s_api = NULL;
        return;
    }

    s_local = radio_local_new(RADIO_MUSIC_DIR);
    if(!s_local)
    {
        logger->Error("InternetRadio: no hay musica en %s", RADIO_MUSIC_DIR);
        return;
    }
    radio_local_set_log(s_local, RadioLog, NULL);
    radio_local_set_track_cb(s_local, OnTrackChanged, NULL);

    /* Sonda de tamano de pantalla: cuadrada, centrada y APARCADA (SetVisible 0 =
       ni se dibuja ni se toca). OnTick la lee una vez y la devuelve al pool. */
    s_probe = s_api->AddButton(0, "shoot", 320.0f, 224.0f, 20.0f, NULL, NULL, NULL);
    if(s_probe)
    {
        s_api->SetSize(s_probe, 100.0f, 100.0f);
        s_api->SetVisible(s_probe, 0);
        s_probeAt = NowMs();
    }
    else
    {
        logger->Error("InternetRadio: sin slots para la sonda de pantalla");
    }

    s_api->SetTick(OnTick, NULL);

    logger->Info("InternetRadio: UI de lienzo lista (%d pistas en %s)",
                 radio_local_count(s_local), RADIO_MUSIC_DIR);
}

/* "Not guaranteed" segun amlmod.h: si el framework no avisa, la muerte del
   proceso se lleva los hilos igualmente. Si avisa, los paramos limpios. */
ON_MOD_UNLOAD()
{
    RadioStop();
    radio_local_free(s_local);
    s_local = NULL;
    logger->Info("InternetRadio: unload, musica parada");
}