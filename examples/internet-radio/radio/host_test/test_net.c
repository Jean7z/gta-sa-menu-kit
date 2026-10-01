/* Host test para la cadena de red de Slice B1: net.c + ring.c + decode.c.
   Descarga un MP3 por HTTP real (socket -> ring -> decoder -> PCM) y escribe
   un WAV PCM16; se valida con ffprobe. El servidor puede ser local
   (python3 net_srv.py) o una emisora real (max_seconds corta el stream).
   Uso: test_net <url> out.wav [max_seconds]   (0 o ausente = sin limite) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../decode.h"
#include "../net.h"
#include "../ring.h"

#define OUT_SAMPLES (1u << 16)     /* capacidad por llamada de feed() */
#define RING_BYTES  (512u * 1024u) /* ~33 s de MP3 128k */
#define READ_CHUNK  4096u

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void* xrealloc(void* p, size_t n)
{
    void* q = realloc(p, n);
    if(!q) { fprintf(stderr, "OOM\n"); exit(1); }
    return q;
}

static void pcm_append(int16_t** pcm, size_t* len, size_t* cap,
                       const int16_t* src, size_t n)
{
    if(*len + n > *cap)
    {
        while(*cap == 0 || *len + n > *cap)
            *cap = *cap ? *cap * 2 : (1u << 16);
        *pcm = (int16_t*)xrealloc(*pcm, *cap * sizeof(int16_t));
    }
    memcpy(*pcm + *len, src, n * sizeof(int16_t));
    *len += n;
}

int main(int argc, char** argv)
{
    if(argc < 3 || argc > 4)
    {
        fprintf(stderr, "usage: %s <url> out.wav [max_seconds]\n", argv[0]);
        return 2;
    }
    double max_s = argc == 4 ? atof(argv[3]) : 0.0;

    RadioRing ring;
    if(radio_ring_init(&ring, RING_BYTES) != 0)
    {
        fprintf(stderr, "ring init failed\n");
        return 2;
    }

    RadioNet net;
    if(radio_net_start_ex(&net, &ring, argv[1], 0) != 0) /* reconnect=0: fuente finita */
    {
        fprintf(stderr, "net start failed\n");
        radio_ring_free(&ring);
        return 2;
    }

    RadioDecoder* dec = radio_decoder_new();
    RadioFormat fmt = { 0, 0 };
    int16_t* pcm = NULL;
    size_t pcm_len = 0, pcm_cap = 0;
    static int16_t out[OUT_SAMPLES];
    unsigned char* stage = (unsigned char*)malloc(READ_CHUNK);

    if(!dec || !stage)
    {
        fprintf(stderr, "OOM\n");
        return 2;
    }

    double t0 = now_s();
    for(;;)
    {
        size_t got = radio_ring_read(&ring, stage, READ_CHUNK);
        if(got == 0) break; /* cerrado y drenado: fin de la fuente */

        int n = radio_decoder_feed(dec, stage, got, out, OUT_SAMPLES, &fmt);
        if(n > 0) pcm_append(&pcm, &pcm_len, &pcm_cap, out, (size_t)n);

        /* emisora en vivo: el cuerpo HTTP nunca termina, cortar por tiempo */
        if(max_s > 0.0 && now_s() - t0 >= max_s) break;
    }
    free(stage);

    for(;;) /* drenar frames pendientes en el carry */
    {
        int n = radio_decoder_flush(dec, out, OUT_SAMPLES, &fmt);
        if(n <= 0) break;
        pcm_append(&pcm, &pcm_len, &pcm_cap, out, (size_t)n);
    }

    radio_net_stop(&net);

    if(pcm_len == 0 || fmt.sample_rate == 0 || fmt.channels == 0)
    {
        fprintf(stderr, "no audio: HTTP %d, %lld bytes, error='%s'\n",
                net.status, net.bytes, radio_net_last_error(&net));
        radio_ring_free(&ring);
        radio_decoder_free(dec);
        free(pcm);
        return 1;
    }

    FILE* fo = fopen(argv[2], "wb");
    if(!fo) { fprintf(stderr, "cannot write %s\n", argv[2]); return 2; }

    unsigned data_len = (unsigned)pcm_len * 2u;
    unsigned short ch = (unsigned short)fmt.channels;
    unsigned rate = (unsigned)fmt.sample_rate;

    fwrite("RIFF", 1, 4, fo);
    unsigned riff_sz = 36u + data_len; fwrite(&riff_sz, 4, 1, fo);
    fwrite("WAVE", 1, 4, fo);
    fwrite("fmt ", 1, 4, fo);
    unsigned fmt_sz = 16u; fwrite(&fmt_sz, 4, 1, fo);
    unsigned short audio_fmt = 1u; fwrite(&audio_fmt, 2, 1, fo);
    fwrite(&ch, 2, 1, fo);
    fwrite(&rate, 4, 1, fo);
    unsigned byte_rate = rate * ch * 2u; fwrite(&byte_rate, 4, 1, fo);
    unsigned short block_align = (unsigned short)(ch * 2u); fwrite(&block_align, 2, 1, fo);
    unsigned short bits = 16u; fwrite(&bits, 2, 1, fo);
    fwrite("data", 1, 4, fo);
    fwrite(&data_len, 4, 1, fo);
    fwrite(pcm, 2, pcm_len, fo);
    fclose(fo);

    radio_ring_free(&ring);
    radio_decoder_free(dec);
    free(pcm);

    printf("net: HTTP %d, %lld B de cuerpo -> %u B PCM, %u ch, %u Hz, %.3f s\n",
           net.status, net.bytes, data_len, ch, rate, (double)pcm_len / ch / rate);
    return 0;
}
