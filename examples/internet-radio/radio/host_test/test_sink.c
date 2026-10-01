/* Host test para radio/sink.c: ritmo real (1x) y backpressure.
   No hay tarjeta de sonido aqui, asi que el test hace de dispositivo: un
   hilo que cada RADIO_SINK_FRAME_MS pide un bloque al sink, igual que haria
   el callback de SLAndroidBufferQueueItf. Como el unico freno posible es el
   ring PCM, que se llena, medir el tiempo da la respuesta:
     - el hilo de decode NO adelanta (el MP3 no se consume a toda velocidad)
     - el dispositivo recibe el ritmo justo (ni 1x ni mas rapido)
   Uso: ./test_sink <in.mp3> [ancho del ring mp3] */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../decode.h"
#include "../ring.h"
#include "../sink.h"
#include "../sink_dev.h"

#define MP3_RING_BYTES (64u * 1024u)

typedef struct {
    RadioSink* sink;
    int stop;
    int samples;      /* muestras int16 por bloque */
    long long first;  /* marca del primer pull (us) */
    long long last;   /* marca del ultimo pull (us) */
    long long pulls;
} Dev;

static int g_frames_seen; /* frames_per_buf que pidio abrir el dispositivo */

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* ---- backend del test (sustituye a sink_opensl.c) ---- */

int radio_dev_open(RadioSink* s, int frames_per_buf)
{
    if(!s->have_fmt || frames_per_buf <= 0) return -1;
    g_frames_seen = frames_per_buf;
    return 0;
}

void radio_dev_close(RadioSink* s) { (void)s; }

/* ---- dispositivo simulado ---- */

static void* fake_device(void* arg)
{
    Dev* d = (Dev*)arg;
    int16_t* buf;
    long long next;

    while(!d->stop && !d->sink->dev) usleep(2000);
    if(d->stop) return NULL;

    d->samples = d->sink->frames_per_buf * d->sink->fmt.channels;
    buf = (int16_t*)malloc((size_t)d->samples * sizeof(int16_t));
    if(!buf) { d->stop = 1; return NULL; }

    next = now_us() + RADIO_SINK_FRAME_MS * 1000;
    d->first = now_us();
    while(!d->stop)
    {
        radio_sink_pull(d->sink, buf, d->samples);
        d->pulls++;
        d->last = now_us();

        next += RADIO_SINK_FRAME_MS * 1000; /* dormir contra el reloj: sin deriva */
        long long wait = next - now_us();
        if(wait > 0) usleep((useconds_t)wait);
    }
    free(buf);
    return NULL;
}

/* ---- referencia: decodificar el fichero entero de una pasada ---- */

static long long decode_all(const uint8_t* data, size_t len, RadioFormat* fmt)
{
    RadioDecoder* d = radio_decoder_new();
    int16_t* pcm = (int16_t*)malloc((size_t)RADIO_SINK_DEC_SAMPLES * sizeof(int16_t));
    long long total = 0;
    size_t pos = 0;

    if(!d || !pcm) { free(pcm); return -1; }
    memset(fmt, 0, sizeof(*fmt));

    while(pos < len)
    {
        size_t chunk = len - pos;
        int got;
        if(chunk > RADIO_SINK_READ_BYTES) chunk = RADIO_SINK_READ_BYTES;
        got = radio_decoder_feed(d, data + pos, chunk, pcm, RADIO_SINK_DEC_SAMPLES, fmt);
        total += got;
        pos += chunk;
    }
    for(;;)
    {
        int got = radio_decoder_flush(d, pcm, RADIO_SINK_DEC_SAMPLES, fmt);
        if(got <= 0) break;
        total += got;
    }
    radio_decoder_free(d);
    free(pcm);
    return total;
}

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1] : "test.mp3";
    size_t mp3_cap = (argc > 2) ? (size_t)atol(argv[2]) : MP3_RING_BYTES;
    uint8_t* data;
    long fsize;
    FILE* f;
    RadioRing mp3;
    RadioSink sink;
    RadioFormat fmt, got_fmt;
    Dev dev;
    pthread_t devth;
    long long expect, elapsed, feed_ms, ratio;
    int bad = 0;

    f = fopen(path, "rb");
    if(!f) { printf("FAIL: no se abre %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (uint8_t*)malloc((size_t)fsize);
    if(!data || fread(data, 1, (size_t)fsize, f) != (size_t)fsize)
    { printf("FAIL: lectura de %s\n", path); fclose(f); return 1; }
    fclose(f);

    expect = decode_all(data, (size_t)fsize, &fmt);
    if(expect <= 0) { printf("FAIL: la referencia no decodifico nada\n"); free(data); return 1; }
    printf("referencia: %lld muestras int16, %d Hz, %d ch\n", expect, fmt.sample_rate, fmt.channels);

    if(radio_ring_init(&mp3, mp3_cap) != 0) { printf("FAIL: ring mp3\n"); return 1; }
    if(radio_sink_init(&sink, &mp3) != 0) { printf("FAIL: sink_init\n"); return 1; }

    memset(&dev, 0, sizeof(dev));
    dev.sink = &sink;

    if(radio_sink_start(&sink) != 0) { printf("FAIL: sink_start\n"); return 1; }
    if(pthread_create(&devth, NULL, fake_device, &dev) != 0)
    { printf("FAIL: pthread del dispositivo\n"); return 1; }

    /* Alimentar el ring como si la red fuera rapidisima: si el sink no
       limitase el ritmo, esto terminaria en milisegundos. */
    {
        long long t0 = now_us();
        radio_ring_write(&mp3, data, (size_t)fsize);
        feed_ms = (now_us() - t0) / 1000;
    }
    radio_ring_close(&mp3);

    /* Esperar a que se entregue todo el PCM, con margen */
    {
        long long deadline = now_us() + 40000000LL;
        for(;;)
        {
            if(radio_ring_available(&mp3) == 0 && sink.pcm_in >= expect &&
               radio_ring_available(&sink.pcm) == 0) break;
            if(now_us() > deadline) { printf("FAIL: no se entrego todo en 40 s\n"); bad = 1; break; }
            usleep(20000);
        }
    }

    dev.stop = 1;
    pthread_join(devth, NULL);
    radio_sink_stop(&sink);
    elapsed = (dev.last > dev.first) ? (dev.last - dev.first) / 1000000 : 0;

    if(radio_sink_format(&sink, &got_fmt) != 0) { printf("FAIL: sin formato\n"); bad = 1; }
    else if(got_fmt.sample_rate != fmt.sample_rate || got_fmt.channels != fmt.channels)
    { printf("FAIL: formato %d Hz/%dch, esperado %d Hz/%dch\n",
           got_fmt.sample_rate, got_fmt.channels, fmt.sample_rate, fmt.channels); bad = 1; }

    printf("mp3 alimentado en %lld ms (sin limite seria ~0)\n", feed_ms);
    printf("dispositivo: %lld bloques de %d muestras en %lld s (%.3f s de audio)\n",
           dev.pulls, dev.samples, elapsed,
           (double)sink.pcm_out / got_fmt.channels / got_fmt.sample_rate);
    printf("pcm: dentro %lld, fuera %lld, silencio %lld en %d eventos, cambios de fmt %d\n",
           sink.pcm_in, sink.pcm_out, sink.underrun, sink.underruns, sink.fmt_changes);
    printf("frames por bloque pedidos: %d (esperado %d)\n",
           g_frames_seen, fmt.sample_rate * RADIO_SINK_FRAME_MS / 1000);

    /* 1) nada de MP3 perdido: el total debe coincidir con la referencia */
    if(sink.pcm_in != expect) { printf("FAIL: pcm_in %lld, esperado %lld\n", sink.pcm_in, expect); bad = 1; }
    /* 2) nada se queda atascado en el ring PCM */
    if(sink.pcm_out != expect) { printf("FAIL: entregado %lld, esperado %lld\n", sink.pcm_out, expect); bad += 1; }
    /* 3) el productor respeta el ritmo. El sink solo puede retener lo que cabe
          en el ring MP3 mas el jitter PCM; todo lo demas tiene que salir a
          velocidad real, asi que alimentar el fichero no puede ser instantaneo.
          El limite se deriva de los datos (con margen del 50%), no es magico. */
    {
        double audio_s  = (double)expect / fmt.channels / fmt.sample_rate;
        double mp3_rate = (double)fsize / audio_s;   /* bytes MP3 por segundo de audio */
        double retain_s = (double)mp3_cap / mp3_rate
                        + (double)RADIO_SINK_PCM_BYTES / (fmt.channels * 2.0 * fmt.sample_rate);
        double min_feed = (audio_s - retain_s) * 0.5;
        if(feed_ms / 1000.0 < min_feed)
        {
            printf("FAIL: alimentado en %lld ms, sin freno deberia caber en %.0f ms\n",
                   feed_ms, min_feed * 1000.0);
            bad += 1;
        }
    }
    /* 4) el dispositivo recibio el ritmo real, ni mas rapido ni mas lento */
    ratio = elapsed ? (long long)((double)sink.pcm_out / got_fmt.channels / got_fmt.sample_rate * 1000.0 / elapsed) : 0;
    if(ratio < 850 || ratio > 1150) { printf("FAIL: ritmo %.3f (esperado ~1.000)\n", ratio / 1000.0); bad += 1; }
    /* 5) el silencio solo puede ser el arranque, antes del primer frame */
    if(sink.underruns > 6) { printf("FAIL: %d underruns\n", sink.underruns); bad += 1; }
    if(g_frames_seen != fmt.sample_rate * RADIO_SINK_FRAME_MS / 1000)
    { printf("FAIL: frames por bloque %d\n", g_frames_seen); bad += 1; }

    radio_sink_free(&sink);
    radio_ring_free(&mp3);
    free(data);

    printf("%s\n", bad ? "SINK FAIL" : "SINK OK");
    return bad ? 1 : 0;
}
