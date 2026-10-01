/* Internet Radio mod - sink de audio (slice C).
   Parte portable: el hilo de decode y el ring PCM viven aqui; el dispositivo
   real (OpenSL ES en Android) es un backend aparte (sink_dev.h). Asi el
   ritmo y la backpressure se verifican en host con un dispositivo simulado.

   Tuberia: red -> ring MP3 -> [hilo de decode] -> ring PCM -> dispositivo.
   El ritmo NO lo impose este modulo: lo impone el consumidor. Cuando el ring
   PCM se llena, radio_ring_write bloquea, el hilo de decode deja de tirar de
   red y la backpressure llega hasta la conexion. Ese es el mecanismo. */
#ifndef RADIO_SINK_H
#define RADIO_SINK_H

#include <pthread.h>
#include <stdint.h>

#include "decode.h"
#include "ring.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Periodo por bloque entregado al dispositivo (ms). 20 ms con 3 bloques en
   vuelo = ~60 ms de cola, de sobra para el latido de Android. */
#define RADIO_SINK_FRAME_MS 20
/* Ring PCM = jitter buffer. 256 KiB stereo 16-bit a 44.1 kHz = 1.49 s. */
#define RADIO_SINK_PCM_BYTES (256u * 1024u)
/* MP3 leido del ring de red por vuelta. */
#define RADIO_SINK_READ_BYTES (16u * 1024u)
/* Muestras int16 por vuelta de decode: 8 frames MP3 estereo (1152*2) con
   holgura. El decoder exige out_capacity >= MINIMP3_MAX_SAMPLES_PER_FRAME. */
#define RADIO_SINK_DEC_SAMPLES 16384

typedef struct RadioSink {
    RadioRing*   mp3;     /* entrada: lo llena el hilo de red (propiedad del
                              llamador, pero radio_sink_stop() lo cierra) */
    RadioRing    pcm;     /* salida: lo consume el dispositivo */
    RadioDecoder* dec;
    pthread_t    th;
    int          th_started;
    int          stop;
    int          dev;     /* 1 con el backend abierto */
    int          dev_error; /* lo que devolvio radio_dev_open si fallo */
    void*        dev_state;
    RadioFormat  fmt;     /* formato con el que se abrio el dispositivo */
    int          have_fmt;
    int          frames_per_buf;
    long long    pcm_in;    /* muestras int16 escritas en el ring PCM */
    long long    pcm_out;   /* muestras int16 entregadas al dispositivo */
    long long    underrun;  /* muestras int16 servidas como silencio */
    int          underruns; /* eventos de underrun */
    int          fmt_changes;
} RadioSink;

/* mp3 = ring de entrada; el sink NO lo inicializa ni lo libera, solo lo lee
   (y lo cierra en stop). mp3 NULL = este sink no se alimenta de un stream, lo
   usa el reproductor local (radio/local.c), que escribe PCM ya decodificado y
   no necesita ni red ni decoder MP3. 0 = ok, -1 = error. */
int  radio_sink_init(RadioSink* s, RadioRing* mp3);
void radio_sink_free(RadioSink* s);

/* Arranca el hilo de decode. Para reiniciar tras un stop hay que volver a
   inicializar el ring mp3 (radio_ring_init) porque stop() lo deja cerrado. */
int  radio_sink_start(RadioSink* s);

/* Para el hilo de decode, cierra el ring mp3 de entrada y el dispositivo, y
   espera al hilo. Idempotente. */
void radio_sink_stop(RadioSink* s);

int  radio_sink_running(const RadioSink* s);
int  radio_sink_format(const RadioSink* s, RadioFormat* fmt);
long long radio_sink_underruns(const RadioSink* s);

/* Abre el dispositivo con `fmt`, o lo reabre (cerrando y tirando el PCM
   pendiente) si el formato no es el que ya tiene abierto. 0 = listo.
   -1 si el formato no lo admite el backend o si el backend no abrio.
   Portable: solo habla con el backend via sink_dev.h, asi que el banco de host
   lo ejercita igual que al resto. */
int  radio_sink_ensure_dev(RadioSink* s, const RadioFormat* fmt);

#ifdef __cplusplus
}
#endif
#endif /* RADIO_SINK_H */
