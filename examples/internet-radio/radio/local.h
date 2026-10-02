/* Internet Radio mod - reproductor local de una carpeta de musica.
   Sustituye a la red como fuente: en vez de un stream MP3 de una estacion, se
   recorren los ficheros de un directorio y los decodifica el propio telefono
   (radio/meddec.c), asi que el mod reproduce lo que el dispositivo admita en
   vez de solo MP3.
   El ritmo sigue siendo el del ring PCM de sink.c: este hilo decodifica en un
   bucle que se bloquea en radio_ring_write cuando el dispositivo va atras, y ahi
   se queda hasta que el consumidor alcanza el ritmo. */
#ifndef RADIO_LOCAL_H
#define RADIO_LOCAL_H

#include "playlist.h"
#include "sink.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RadioLocal RadioLocal;

/* Log opcional, estilo printf. main.cpp lo ata a logger->Info para que los
   mensajes aparezcan en el log del juego: sin esto, un fichero que no se puede
   abrir seria un silencio sin explicacion, y no hay forma de depurarlo. */
typedef void (*RadioLocalLog)(void* ctx, const char* fmt, ...);
void radio_local_set_log(RadioLocal* l, RadioLocalLog fn, void* ctx);

/* Aviso de cambio de pista, desde el hilo de reproduccion y en el instante en
   que el fichero queda abierto. `name` es el nombre de fichero y `path` la ruta
   completa, que es lo que necesita la capa de portada.
   main.cpp lo usa para refrescar la etiqueta y el icono en el momento, sin
   polling: el juego no ofrece ningun hook de frame, y SetText se puede llamar
   en runtime porque el motor lo aplica en el siguiente frame.
   OJO: corre en el hilo de audio, asi que el callback no puede tardar mas que
   el cushion del ring PCM (1.49 s) o se oye un corte. */
typedef void (*RadioLocalTrack)(void* ctx, const char* name, const char* path);
void radio_local_set_track_cb(RadioLocal* l, RadioLocalTrack fn, void* ctx);

/* Crea el reproductor para el directorio `dir`. Devuelve NULL si no se puede
   abrir el directorio. La lista se relee al arrancar, asi que puedes copiar
   canciones nuevas en la carpeta sin reiniciar el juego. */
RadioLocal* radio_local_new(const char* dir);
void radio_local_free(RadioLocal* l);

/* Arranca/para el hilo de reproduccion. Idempotente. */
int  radio_local_start(RadioLocal* l);
void radio_local_stop(RadioLocal* l);

/* Pausa/reanuda SIN perder la posicion: el hilo y el decodificador siguen
   vivos y el ring se descarta, asi que reanudar continua en el punto exacto.
   Es lo que distingue esto de stop() + start(), que reinician desde la primera
   pista. El skip sigue valiendo en pausa. on = 1 pausa, on = 0 reanuda. */
void radio_local_pause(RadioLocal* l, int on);
int  radio_local_paused(const RadioLocal* l);

/* Salta de pista: dir = +1 siguiente, -1 anterior, 0 no hace nada. La carpeta
   es una lista en bucle, asi que los extremos dan la vuelta.
   No hace falta despertador: el hilo comprueba el comando entre chunks de
   RADIO_SINK_DEC_SAMPLES, y radio_ring_write espera como mucho a que se vacie
   un chunk entero (32 KiB = 186 ms a 44.1 kHz estereo). La latencia maxima del
   skip es ese tiempo, y no hay ningun sleep que lo acorte. */
void radio_local_skip(RadioLocal* l, int dir);

/* Contadores, para el log al parar. */
int  radio_local_tracks(const RadioLocal* l);
int  radio_local_underruns(const RadioLocal* l);
long long radio_local_samples(const RadioLocal* l);
/* Cambios de formato: con ficheros de la plataforma esto NO deberia ser 0, cada
   cancion con un sample rate distinto reabre el dispositivo de audio. */
int         radio_local_fmt_changes(const RadioLocal* l);
/* Nombre del fichero en curso ("" si nada). */
const char* radio_local_name(const RadioLocal* l);
/* Indice en la lista del tema en curso (-1 si nada). Es la posicion que el
   skip respeta, asi que la UI puede mostrar "3/12" sin consultar el fichero. */
int         radio_local_index(const RadioLocal* l);
int         radio_local_count(const RadioLocal* l);

#ifdef __cplusplus
}
#endif
#endif /* RADIO_LOCAL_H */
