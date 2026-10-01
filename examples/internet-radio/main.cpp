/* Internet Radio - reproductor local de musica, cliente de la API v5 de SA Menu Kit.
   El boton recorre los ficheros de una carpeta y los decodifica el propio
   telefono (radio/meddec.c: AMediaExtractor + AMediaCodec), asi que suenan flac,
   m4a, opus, ogg... y no solo mp3. La eleccion de formato la hace el
   decodificador del sistema, no una lista nuestra.
   UI: un boton minimo siempre visible en una esquina y, al tocarlo, un grupo
   pequeño de controles que se autodestruye. Nada de barras permanentes ni
   portadas gigantes tapando el HUD (ver la seccion UI mas abajo).
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

MYMODCFG(net.psdk.samod.internetradio, Internet Radio, 0.3, Jean7z)

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
   AddButton recibe el CENTRO en unidades VIRTUALES 640x448; GetRect devuelve
   el rect en PIXELES REALES, y los dos ejes escalan DISTINTO. Medido en
   dispositivo (render 1600x720) con GetRect sobre AddButton:

       x: 1600/640 = 2.500        y: 720/448 = 1.607

   O sea que NO hay un factor uniforme, y dos consecuencias practicas:

   1) Todo widget sale cuadrado en px reales (100x100 medido), asi que no hay
      banners anchos ni rectangulos; el "panel" es un grupo apretado de
      cuadrados y por eso los textos son cortos.
   2) La huella VERTICAL de un widget en reales (100 px) es mayor que su
      separacion virtual (40 unidades x 1.607 = 64 px). Cualquier hueco
      calculado en verticales virtuales queda corto a proposito. Por eso el
      toast va alineado a la fila y no debajo de ella.

   Ojo: OS_ScreenGetWidth/Height devolvio 1024x600 cuando el render real era
   1600x720, asi que la escala NO se puede sacar de ahi. La unica fuente
   fiable es GetRect sobre un widget ya construido.
   La esquina es CONFIG (UI_ANCHOR), no una coordenada enterrada en el codigo. */
enum { UI_TOP_LEFT, UI_TOP_RIGHT, UI_BOTTOM_LEFT, UI_BOTTOM_RIGHT };

/* BOTTOM_LEFT porque es la unica esquina que no depende de resolver una
   contradiccion que no se puede zafar sin mirar la pantalla:

     - El comentario anterior de este archivo decia TOP_LEFT "por medicion":
       radar arriba-derecha, widgets nativos x=1229..1568 y=447..708 (bloque
       inferior-derecho), columna izquierda libre.
     - El usuario reporto que el boton trigger le cae ENCIMA del minimapa, lo
       que contradice esa medicion: implicaria radar en la columna izquierda.

   TOP_LEFT es incorrecta si el radar esta ahi; BOTTOM_LEFT es correcta bajo las
   DOS hipotesis (columna izquierda libre, o radar en la parte alta de la
   columna). Queda a un #define cambiarlo cuando se confirme donde esta el radar.
   Ojo: con ty = 448-36 el trigger baja hasta y 612..712 px reales, o sea a 8 px
   del borde inferior. Si al cambiar de esquina el trigger se recorta, es que el
   render util es menor que 720 y UI_MARGIN hay que subirlo. */
#define UI_ANCHOR        UI_BOTTOM_LEFT
#define UI_MARGIN        36.0f
#define UI_TRIGGER_S     20.0f   /* semiext => 40x40 virtuales (100x100 px reales) */
#define UI_BTN_S         20.0f
#define UI_BTN_GAP       4.0f
#define UI_ROW_GAP       8.0f
#define UI_TOAST_S       30.0f
#define UI_ALPHA_DIM     0x80    /* 50% con el panel cerrado: se ve que hay algo */
#define UI_ALPHA_ON      0xFF
#define UI_AUTO_HIDE_MS  4000
#define UI_TOAST_MS      2000

/* Glifo del trigger. AddButton YA acepta icono (ultimo argumento = ruta ABSOLUTA
   a un PNG; stbi_load detecta el formato por contenido, asi que un JPEG tambien
   valdria). En cuanto exista un nota-musical.png esto pasa a ser la ruta y el
   trigger deja de llevar texto. */
#define UI_TRIGGER_GLYPH "R"

/* Toast = el unico texto en pantalla, y solo 2 s. Corto a proposito: en un
   cuadrado de 60 virtuales la fuente 5x7 no aguanta un nombre largo sin volverse
   ilegible, asi que se corta aqui y el nombre entero vive en el log. */
#define TOAST_MAX_CHARS 16

/* Longitud maxima del nombre de pista que muestra el log y el toast. */
#define NAME_MAX_CHARS 40

static const MenuKitAPI* s_api = NULL;
static RadioLocal* s_local = NULL;
static bool s_radioOn = false;
static bool s_open = false;          /* panel desplegado */

static void* s_trigger = NULL;       /* siempre visible */
static void* s_prev    = NULL;       /* panel: solo con el panel abierto */
static void* s_play    = NULL;
static void* s_next    = NULL;
static void* s_toast   = NULL;       /* feedback de estado, 2 s */

static char  s_dispName[NAME_MAX_CHARS + 1];
static pthread_mutex_t s_nameLock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_hideAtMs = 0;      /* deadline de auto-hide del panel */
static uint64_t s_toastUntilMs = 0;

/* Posiciones de la UI para la esquina configurada. La fila de 3 botones se ancla
   al borde EXTERIOR del trigger y se abre hacia el centro de la pantalla, asi
   que cabe entera en cualquier esquina sin salirse. */
struct UiLayout { float tx, ty, rowY, x0, x1, x2, toastX, toastY; };

static void ComputeLayout(struct UiLayout* L)
{
    int onRight = (UI_ANCHOR == UI_TOP_RIGHT || UI_ANCHOR == UI_BOTTOM_RIGHT);
    int onTop   = (UI_ANCHOR == UI_TOP_LEFT   || UI_ANCHOR == UI_TOP_RIGHT);
    float pitch = 2.0f * UI_BTN_S + UI_BTN_GAP;   /* centro a centro */

    L->tx = onRight ? (640.0f - UI_MARGIN) : UI_MARGIN;
    L->ty = onTop   ? UI_MARGIN : (448.0f - UI_MARGIN);

    /* La fila arranca PEGADA al borde interior del trigger y se abre hacia el
       centro. Antes se anclaba al borde EXTERIOR y la fila se iba: en el lado
       izquierdo el hueco medido era de 88 unidades virtuales frente a 48 en el
       derecho, porque x0 salia de (tx - BTN_S) + total en vez de (tx + BTN_S).
       Medido en dispositivo con GetRect: el trigger ocupaba x 40..140 px y la
       fila empezaba en 360 px. */
    if(onRight)
    {
        L->x2 = L->tx - UI_TRIGGER_S - UI_BTN_GAP - UI_BTN_S;
        L->x1 = L->x2 - pitch;
        L->x0 = L->x1 - pitch;
    }
    else
    {
        L->x0 = L->tx + UI_TRIGGER_S + UI_BTN_GAP + UI_BTN_S;
        L->x1 = L->x0 + pitch;
        L->x2 = L->x1 + pitch;
    }

    L->rowY = onTop ? (L->ty + UI_TRIGGER_S + UI_ROW_GAP + UI_BTN_S)
                    : (L->ty - UI_TRIGGER_S - UI_ROW_GAP - UI_BTN_S);

    /* El toast va AL LADO de la fila, no debajo: los widgets son cuadrados en
       px reales (100 px de alto) pero su huella vertical virtual son solo ~62
       px, asi que cualquier separacion calculada en unidades virtuales se queda
       corta a proposito y el toast terminaba pisando a prev. Medido: toast en
       x 125..275 contra prev en 150..250. Alineado a la fila no colisiona y no
       depende de adivinar la escala vertical. */
    L->toastX = onRight ? (L->x0 - UI_BTN_S - UI_BTN_GAP - UI_TOAST_S)
                        : (L->x2 + UI_BTN_S + UI_BTN_GAP + UI_TOAST_S);
    L->toastY = L->rowY;
}

/* Reloj de milisegundos. El unico con el que se puede hacer un auto-hide: AML no
   da hook por frame (solo PRELOAD/LOAD/UNLOAD/CRASH), asi que el pump de
   CGame_Process es lo que llama a SetTick, una vez por frame. */
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

/* Feedback de estado: aparece 2 s y se va. Sustituye a la etiqueta permanente,
   que era justo lo que tapaba el HUD. */
static void ShowToast(const char* prefix)
{
    char name[NAME_MAX_CHARS + 1];
    char label[64];

    if(!s_api || !s_toast) return;

    pthread_mutex_lock(&s_nameLock);
    snprintf(name, sizeof name, "%s", s_dispName);
    pthread_mutex_unlock(&s_nameLock);
    name[TOAST_MAX_CHARS] = 0;

    if(prefix && *prefix && name[0]) snprintf(label, sizeof label, "%s %s", prefix, name);
    else if(name[0])                  snprintf(label, sizeof label, "%s", name);
    else                              snprintf(label, sizeof label, "%s", prefix ? prefix : "");
    if(!label[0]) snprintf(label, sizeof label, "-");

    s_api->SetText(s_toast, label);
    s_api->SetVisible(s_toast, 1);
    s_toastUntilMs = NowMs() + UI_TOAST_MS;
}

static void SetPanelOpen(int open)
{
    int on = open != 0;
    s_open = on;
    if(!s_api) return;
    if(s_prev) s_api->SetVisible(s_prev, on);
    if(s_play) s_api->SetVisible(s_play, on);
    if(s_next) s_api->SetVisible(s_next, on);
    /* El trigger se queda siempre visible; solo cambia de intensidad. */
    if(s_trigger) s_api->SetAlpha(s_trigger, on ? UI_ALPHA_ON : UI_ALPHA_DIM);
    s_hideAtMs = NowMs() + UI_AUTO_HIDE_MS;
}

/* ¿El punto cae dentro del panel O sobre el trigger? GetTap y GetRect dan ambos
   el mismo espacio (pixeles reales), asi que esto son cuatro comparaciones y
   nada mas. El trigger se cuenta como "dentro" a proposito: si se contara como
   fuera, al pulsarlo con el panel abierto mi logica de "toca fuera -> cerrar" lo
   cerraria y acto seguido el callback del trigger (que alterna) lo reabriria:
   parpadeo y nunca se cierra. Dejando el trigger fuera de este test, un toque
   sobre el trigger lo lleva solo su callback (que si alterna bien), y un toque
   fuera de ambos lo cierra esta logica. Un GetRect que devuelve 0 significa "el
   motor aun no ha colocado ese widget" y se cuenta como fuera. */
static int TapInsidePanel(float x, float y)
{
    void* hit[4];
    int   n = 0, i;
    float l, t, r, b;

    if(s_trigger) hit[n++] = s_trigger;   /* Ver nota: el trigger lo maneja su callback */
    if(s_prev)    hit[n++] = s_prev;
    if(s_play)    hit[n++] = s_play;
    if(s_next)    hit[n++] = s_next;
    for(i = 0; i < n; ++i)
    {
        if(!s_api->GetRect(hit[i], &l, &t, &r, &b)) continue;
        if(x >= l && x <= r && y >= t && y <= b) return 1;
    }
    return 0;
}

/* Timer del cliente (SetTick). Auto-hide del panel y retirada del toast.
   Los deadlines en 0 significan "nada pendiente": sin ese guardia el toast, que
   arranca aparcado, se volveria a aparcar en cada frame (y a loguear). */
/* ---- v8 canvas demo ----------------------------------------------------
   Immediate-mode: no hay estado retenido, asi que REDIBUJAR cada frame es
   justamente como se anima. Todo esto se dibuja en pixeles reales, el mismo
   espacio que GetRect/GetTap. Borra este bloque para quitar la demo. */
static void DrawCanvasDemo(uint64_t now)
{
    if(!s_api || s_api->version < 8) return;

    const float x = 60.0f, y = 60.0f, w = 300.0f, h = 100.0f;
    const float pulse = 0.5f + 0.5f * (float)sin((float)(now % 1600) * 0.003927f);
    const unsigned int a = (unsigned int)(60 + 195.0f * pulse);

    /* Panel redondeado: relleno estable (sin fan de perímetro). Descompuesto en
       piezas convexas: 4 círculos de esquina + 3 rectángulos axis-aligned. Esto
       evita los parpadeos que produce un bucle de borde pasado a Canvas_Fan. */
    const float rr = 14.0f;
    /* Esquinas: TL, TR, BR, BL */
    const float cxr[4] = { x+rr,   x+w-rr, x+w-rr, x+rr   };
    const float cyr[4] = { y+rr,   y+rr,   y+h-rr, y+h-rr };
    for(int c = 0; c < 4; ++c)
        s_api->DrawCircle(cxr[c], cyr[c], rr, 0x101820u | (a << 24), 1, 20);

    /* Rectángulos para rellenar el cuerpo (cubren huecos entre esquinas) */
    /* Centro horizontal: cubre todo el ancho entre esquinas, altura h - 2*rr */
    s_api->DrawRect(x + rr, y, w - 2.0f*rr, h, 0x101820u | (a << 24), 1);
    /* Centro vertical: cubre columnas laterales entre esquinas, altura 2*rr */
    s_api->DrawRect(x, y + rr, rr, h - 2.0f*rr, 0x101820u | (a << 24), 1);
    s_api->DrawRect(x + w - rr, y + rr, rr, h - 2.0f*rr, 0x101820u | (a << 24), 1);

    /* Borde: polígono de perímetro cerrado (stroke). No usa fan, así que es estable. */
    {
        float p[8 * 3 * 2];
        int n = 0;
        const float cx[4] = { x+rr,   x+w-rr, x+w-rr, x+rr   };
        const float cy[4] = { y+rr,   y+rr,   y+h-rr, y+h-rr };
        const float st[4] = { 4.7124f, 0.0f,  1.5708f, 3.14159f };
        for(int c = 0; c < 4; ++c)
            for(int k = 0; k < 3; ++k) {
                const float ang = st[c] - (float)k * 0.7854f;
                p[n*2]     = cx[c] + rr * (float)cos(ang);
                p[n*2 + 1] = cy[c] + rr * (float)sin(ang);
                ++n;
            }
        s_api->DrawPoly(p, n, 0x38BDF8FFu, 0, 1);   /* borde fijo */
    }

    /* Circulo que recorre el panel de izquierda a derecha. */
    const float cxp = x + 20.0f + (w - 40.0f) * (((now % 2000) / 2000.0f));
    s_api->DrawCircle(cxp, y + h*0.5f, 12.0f, 0xF97316FFu, 1, 24);

    /* Triangulo girando sobre el borde inferior. */
    const float rot = (float)(now % 3000) * 0.002094f;
    const float ox = x + w - 34.0f, oy = y + 20.0f, orr = 13.0f;
    const float t0 = rot, t1 = rot + 2.0944f, t2 = rot + 4.1888f;
    s_api->DrawTriangle(ox + orr*(float)cos(t0), oy + orr*(float)sin(t0),
                        ox + orr*(float)cos(t1), oy + orr*(float)sin(t1),
                        ox + orr*(float)cos(t2), oy + orr*(float)sin(t2),
                        0xA78BFAFFu, 1);

    /* Linea barriendo, con grosor variable. */
    s_api->DrawLine(x + 10.0f, y + h + 26.0f, x + w - 10.0f, y + h + 26.0f,
                    0x22D3EEFFu, 2.0f + 6.0f * pulse);
}

static void OnTick(void* userdata)
{
    float tx = 0.0f, ty = 0.0f;
    uint64_t now;
    (void)userdata;
    if(!s_api) return;
    now = NowMs();

    /* v7: tap global. Antes esto era imposible -AML no expone touch y solo se
       reaccionaba a toques sobre los widgets propios-. Un toque DENTRO del panel
       solo reinicia el reloj; uno FUERA lo cierra, que es el comportamiento que
       se espera de un panel que aparece solo. */
    if(s_api->GetTap(&tx, &ty))
    {
        if(s_open)
        {
            if(TapInsidePanel(tx, ty)) s_hideAtMs = now + UI_AUTO_HIDE_MS;
            else                         SetPanelOpen(0);
        }
    }

    if(s_open && s_hideAtMs && now >= s_hideAtMs) SetPanelOpen(0);
    if(s_toastUntilMs && now >= s_toastUntilMs)
    {
        s_toastUntilMs = 0;
        if(s_toast) s_api->SetVisible(s_toast, 0);
    }

    DrawCanvasDemo(now);
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
    ShowToast(NULL);   /* solo el nombre: el estado ya se vio al pulsar play */
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
    logger->Info("InternetRadio: ON -> %s", RADIO_MUSIC_DIR);
    return 0;
}

static void RadioStop(void)
{
    if(!s_radioOn) return;
    radio_local_stop(s_local);
    RadioLogStats();
    s_radioOn = false;
    logger->Info("InternetRadio: OFF");
}

/* play/stop del panel. El boton es el que muestra ON/OFF, asi que su etiqueta
   se refresca en cada cambio de estado. */
static void OnPanelPlay(void* userdata)
{
    (void)userdata;
    if(!s_open) return;
    if(s_radioOn)
    {
        RadioStop();
        if(s_play) s_api->SetText(s_play, "OFF");
        ShowToast("OFF");
    }
    else if(RadioStart() == 0)
    {
        if(s_play) s_api->SetText(s_play, "ON");
        ShowToast("ON");
    }
    s_hideAtMs = NowMs() + UI_AUTO_HIDE_MS;   /* interactuar reinicia el reloj */
}

static void OnSkip(void* userdata)
{
    int dir = (int)(intptr_t)userdata;   /* -1 anterior, +1 siguiente */

    if(!s_open) return;
    if(!s_radioOn)
    {
        logger->Info("InternetRadio: skip sin reproduccion, se ignora");
        return;
    }
    radio_local_skip(s_local, dir);
    s_hideAtMs = NowMs() + UI_AUTO_HIDE_MS;
}

static void OnTrigger(void* userdata)
{
    (void)userdata;
    SetPanelOpen(!s_open);
}

ON_MOD_LOAD()
{
    struct UiLayout L;

    logger->SetTag("InternetRadio");

    s_api = MenuKit_GetAPI(aml);
    if(!s_api)
    {
        logger->Error("InternetRadio: MenuKit framework not loaded (load AML_PSDK_MenuKit64 first)");
        return;
    }
    if(s_api->version < 7)
    {
        logger->Error("InternetRadio: MenuKit API v7 required (got v%u) - actualiza AML_PSDK_MenuKit64",
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

    ComputeLayout(&L);

    /* "shoot" is a REAL texture name in the game's image DB (the native attack
       button CWidgetButtonAttackC2 references it at libGTASA.so+0x83d611), and it
       is never visible: with SetText the engine generates a dark panel with the
       text centred over it, and with SetVisible(0) it early-returns before Draw. */
    s_trigger = s_api->AddButton(0, "shoot", L.tx, L.ty, UI_TRIGGER_S, OnTrigger, NULL, NULL);
    s_prev    = s_api->AddButton(0, "shoot", L.x0, L.rowY, UI_BTN_S, OnSkip, (void*)(intptr_t)-1, NULL);
    s_play    = s_api->AddButton(0, "shoot", L.x1, L.rowY, UI_BTN_S, OnPanelPlay, NULL, NULL);
    s_next    = s_api->AddButton(0, "shoot", L.x2, L.rowY, UI_BTN_S, OnSkip, (void*)(intptr_t)+1, NULL);
    s_toast   = s_api->AddButton(0, "shoot", L.toastX, L.toastY, UI_TOAST_S, NULL, NULL, NULL);

    if(!s_trigger || !s_prev || !s_play || !s_next || !s_toast)
    {
        logger->Error("InternetRadio: could not add root widgets");
        return;
    }

    s_api->SetText(s_trigger, UI_TRIGGER_GLYPH);
    s_api->SetText(s_prev, "<");
    s_api->SetText(s_play, "OFF");
    s_api->SetText(s_next, ">");
    s_api->SetText(s_toast, "-");

    /* Estado inicial: solo el trigger, atenuado. El panel y el toast arrancan
       APARCADOS (SetVisible 0 = ni se dibujan ni tocan), no ocultos con alpha,
       porque un alpha 0 seguiria interceptando toques. */
    s_api->SetAlpha(s_trigger, UI_ALPHA_DIM);
    s_api->SetVisible(s_prev, 0);
    s_api->SetVisible(s_play, 0);
    s_api->SetVisible(s_next, 0);
    s_api->SetVisible(s_toast, 0);

    /* v6: forma en PIXELES REALES, ancla del motor intacta. AddButton solo
       acepta un `scale` uniforme, asi que todo widget que crea sale cuadrado;
       SetSize separa ancho de alto conservando el CENTRO que ya calculo el
       motor. Solo se cambia la FORMA: la esquina y el margen siguen siendo los
       que el motor projecting de las unidades virtuales (640x448), que ya
       funcionan y son independientes del dispositivo. Deliberadamente NO se
       tocan las coordenadas: en coordenadas absolutas habria que conocer el
       ancho real de pantalla, y OS_ScreenGetWidth() dio 1024x600 cuando el
       render real es 1600x720 (medido), lo que dejaba el boton a mitad de
       pantalla. */
    {
        /* Trigger: cuadrado de 120 px reales. */
        s_api->SetSize(s_trigger, 120.0f, 120.0f);
        /* Boton de play: PANEL RECTANGULAR de 240x70 px. Antes del v6 esto era
           imposible (cuadrado obligatorio). Es la prueba en vivo de que la
           limitacion esta rota. */
        s_api->SetSize(s_play, 240.0f, 70.0f);
    }

    s_api->SetTick(OnTick, NULL);

    logger->Info("InternetRadio: UI lista (trigger %d,%d | panel cerrado | auto-hide %d ms)",
                 (int)L.tx, (int)L.ty, UI_AUTO_HIDE_MS);
    logger->Info("InternetRadio: %d pistas en %s", radio_local_count(s_local), RADIO_MUSIC_DIR);
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
