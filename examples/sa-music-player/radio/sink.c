/* SA Music Player mod - sink de audio portable (ver sink.h).
   Hilo de decode: MP3 (ring de red) -> decoder -> PCM (ring jitter).
   El ritmo lo impone el consumidor, no este modulo: cuando el ring PCM se
   llena, radio_ring_write bloquea, el hilo deja de tirar de red y la
   backpressure llega hasta la conexion. */
#include "sink.h"
#include "sink_dev.h"

#include <stdlib.h>
#include <string.h>

/* Contabilidad compartida por los dos backends: rellena `samples` muestras
   desde el ring PCM y pone silencio en lo que falte, contando el hueco. */
int radio_sink_pull(RadioSink* s, int16_t* out, int samples)
{
    size_t want, got;

    if(samples <= 0) return 0;
    want = (size_t)samples * sizeof(int16_t);
    got  = radio_ring_read_nonblock(&s->pcm, out, want);
    if(got < want)
    {
        memset((unsigned char*)out + got, 0, want - got);
        s->underrun  += (long long)((want - got) / sizeof(int16_t));
        s->underruns++;
    }
    s->pcm_out += (long long)(got / sizeof(int16_t));
    return (int)(got / sizeof(int16_t));
}

static int fmt_differs(const RadioSink* s, const RadioFormat* f)
{
    return s->have_fmt && (f->sample_rate != s->fmt.sample_rate ||
                           f->channels     != s->fmt.channels);
}

/* Vacia el ring PCM. Solo se usa al cambiar de formato, donde lo que hay dentro
   pertenece al tema anterior y sonaria a la velocidad anterior. */
static void sink_drain_pcm(RadioSink* s)
{
    unsigned char scratch[4096];
    while(radio_ring_available(&s->pcm) > 0)
        if(radio_ring_read_nonblock(&s->pcm, scratch, sizeof scratch) == 0) break;
}

/* Abre el dispositivo con `fmt`, o lo reabre si el formato ha cambiado.
   El backend no se puede reconfigurar en caliente, asi que un cambio obliga a
   cerrar, tirar el PCM pendiente del tema anterior y abrir de nuevo. Sin tirar
   ese PCM, la cola (1.5 s) se despondria por el dispositivo nuevo y el final
   de la cancion anterior sonaria a velocidad doble.
   En modo streaming un MP3 de radio no cambia de formato a mitad de emision, asi
   que esto no hace nada ahi: el banco de host lo sigue viendo abrir una vez. */
int radio_sink_ensure_dev(RadioSink* s, const RadioFormat* fmt)
{
    if(!s || !fmt) return -1;
    if(fmt->sample_rate <= 0 || fmt->channels < 1 || fmt->channels > 2)
    {
        s->dev_error = -1;
        return -1;
    }

    if(s->dev && !fmt_differs(s, fmt)) return 0;   /* ya abierto con este formato */

    if(s->dev)
    {
        s->fmt_changes++;
        radio_dev_close(s);   /* quita el callback antes de tirar el ring */
        s->dev = 0;
        sink_drain_pcm(s);
    }

    s->fmt = *fmt;
    s->have_fmt = 1;
    s->frames_per_buf = fmt->sample_rate * RADIO_SINK_FRAME_MS / 1000;
    s->dev_error = radio_dev_open(s, s->frames_per_buf);
    if(s->dev_error != 0) return -1;
    s->dev = 1;
    return 0;
}

/* Alimenta el decoder con `data` y escribe en el ring PCM TODO lo que salga,
   incluidos los frames que ya estaban en su carry. Se vacia el carry en el
   mismo ciclo a proposito: un solo feed por lectura solo cabe en
   RADIO_SINK_DEC_SAMPLES, y lo que no cupiera se acumularia en el carry
   interno del decoder (que trunca a 256 KiB, perdiendo audio). Asi el ring
   PCM es el unico sitio donde se acumula audio y el ring MP3 sigue siendo
   la fuente de backpressure.
   Bloquea al escribir si el dispositivo va atras: de ahi sale el ritmo 1x.
   Devuelve las muestras producidas, o -1 si el dispositivo no abrio. */
static int sink_feed_decode(RadioSink* s, const uint8_t* data, size_t len,
                            int16_t* pcm, const uint8_t* empty, RadioFormat* fmt)
{
    size_t off = 0;
    int total = 0;

    for(;;)
    {
        const uint8_t* chunk;
        size_t clen;
        size_t w;
        int got;

        if(off < len) { chunk = data + off; clen = len - off; off = len; }
        else          { chunk = empty;  clen = 0; } /* drena el carry */

        got = radio_decoder_feed(s->dec, chunk, clen, pcm, RADIO_SINK_DEC_SAMPLES, fmt);
        if(got <= 0) break;

        /* El primer PCM que sale ya trae el formato definitivo del stream, que
           es justo lo que OpenSL necesita para no sonar afinado. Si el formato
           cambiara, ensure_dev reabre el dispositivo por su cuenta. */
        if(radio_sink_ensure_dev(s, fmt) != 0) return -1;

        w = radio_ring_write(&s->pcm, pcm, (size_t)got * sizeof(int16_t));
        s->pcm_in += (long long)(w / sizeof(int16_t));
        total += got;
        if(s->stop) break;
    }
    return total;
}

static void* sink_thread(void* arg)
{
    RadioSink* s = (RadioSink*)arg;
    uint8_t* mp3 = (uint8_t*)malloc(RADIO_SINK_READ_BYTES);
    int16_t* pcm = (int16_t*)malloc((size_t)RADIO_SINK_DEC_SAMPLES * sizeof(int16_t));
    RadioFormat fmt;

    if(!mp3 || !pcm) { free(mp3); free(pcm); return NULL; }

    while(!s->stop)
    {
        size_t n = radio_ring_read(s->mp3, mp3, RADIO_SINK_READ_BYTES);
        if(n == 0) break; /* el ring mp3 se cerro y quedo drenado */
        if(sink_feed_decode(s, mp3, n, pcm, mp3, &fmt) < 0) break;
    }

    /* Cola de la emision: el carry del decoder puede traer frames completos
       que solo salen con flush. Sin esto se pierde el ultimo segundo. */
    if(!s->stop && s->dev)
    {
        int got;
        do
        {
            size_t w;
            got = radio_decoder_flush(s->dec, pcm, RADIO_SINK_DEC_SAMPLES, &fmt);
            if(got <= 0) break;
            w = radio_ring_write(&s->pcm, pcm, (size_t)got * sizeof(int16_t));
            s->pcm_in += (long long)(w / sizeof(int16_t));
        } while(!s->stop);
    }

    free(mp3);
    free(pcm);
    return NULL;
}

/* mp3 = ring de entrada. NULL significa que este sink NO se alimenta de un
   stream: lo usa el reproductor local, que ya entrega PCM decodificado y no
   tiene ni red ni ring comprimido. En ese caso radio_sink_start() no debe
   llamarse (no hay hilo de decode propio). 0 = ok, -1 = error. */
int radio_sink_init(RadioSink* s, RadioRing* mp3)
{
    if(!s) return -1;
    memset(s, 0, sizeof(*s));
    s->mp3 = mp3;
    if(mp3)
    {
        s->dec = radio_decoder_new();
        if(!s->dec) return -1;
    }
    if(radio_ring_init(&s->pcm, RADIO_SINK_PCM_BYTES) != 0)
    {
        if(s->dec) { radio_decoder_free(s->dec); s->dec = NULL; }
        return -1;
    }
    return 0;
}

int radio_sink_start(RadioSink* s)
{
    if(!s) return -1;
    s->stop = 0;
    s->dev = 0;
    s->dev_error = 0;
    s->dev_state = NULL;
    s->have_fmt = 0;
    s->frames_per_buf = 0;
    s->pcm_in = s->pcm_out = s->underrun = 0;
    s->underruns = s->fmt_changes = 0;

    /* Decoder y ring PCM nuevos en cada arranque: no se arrastra audio de la
       emision anterior ni del ring cerrado. */
    if(s->dec) radio_decoder_free(s->dec);
    s->dec = radio_decoder_new();
    if(!s->dec) return -1;
    radio_ring_free(&s->pcm);
    if(radio_ring_init(&s->pcm, RADIO_SINK_PCM_BYTES) != 0) return -1;

    if(pthread_create(&s->th, NULL, sink_thread, s) != 0) return -1;
    s->th_started = 1;
    return 0;
}

void radio_sink_stop(RadioSink* s)
{
    if(!s) return;
    s->stop = 1;
    if(s->th_started)
    {
        /* Cerrar los dos rings despierta al hilo este donde este esperando:
           leyendo MP3 o escribiendo PCM. Sin esto, stop() colgaria. */
        if(s->mp3) radio_ring_close(s->mp3);
        radio_ring_close(&s->pcm);
        pthread_join(s->th, NULL);
        s->th_started = 0;
    }
    if(s->dev) { radio_dev_close(s); s->dev = 0; }
}

void radio_sink_free(RadioSink* s)
{
    if(!s) return;
    radio_sink_stop(s);
    radio_ring_free(&s->pcm);
    if(s->dec) radio_decoder_free(s->dec);
    s->dec = NULL;
    s->mp3 = NULL;
}

int radio_sink_running(const RadioSink* s)
{
    return s ? s->th_started : 0;
}

int radio_sink_format(const RadioSink* s, RadioFormat* fmt)
{
    if(!s || !fmt || !s->have_fmt) return -1;
    *fmt = s->fmt;
    return 0;
}

long long radio_sink_underruns(const RadioSink* s)
{
    return s ? s->underruns : 0;
}
