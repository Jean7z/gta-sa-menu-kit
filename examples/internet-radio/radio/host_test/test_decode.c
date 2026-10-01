/* Host test para radio/decode.c: lee un MP3 en chunks (simulando la llegada
   por red y forzando el carry entre chunks), decodifica con
   radio_decoder_feed y escribe un WAV PCM16. Se valida con ffprobe desde el
   script de verificacion (duracion/canales/rate/codec).
   Uso: test_decode in.mp3 out.wav [chunk_bytes]   (chunk por defecto: 2048) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../decode.h"

#define OUT_SAMPLES (1u << 16) /* capacidad por llamada de feed() */

static void* xrealloc(void* p, size_t n)
{
    void* q = realloc(p, n);
    if(!q) { fprintf(stderr, "OOM\n"); exit(1); }
    return q;
}

int main(int argc, char** argv)
{
    if(argc < 3 || argc > 4)
    {
        fprintf(stderr, "usage: %s in.mp3 out.wav [chunk_bytes]\n", argv[0]);
        return 2;
    }
    size_t chunk_bytes = argc == 4 ? (size_t)strtoul(argv[3], NULL, 10) : 2048u;
    if(!chunk_bytes) chunk_bytes = 2048u;

    FILE* fi = fopen(argv[1], "rb");
    if(!fi) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }

    RadioDecoder* dec = radio_decoder_new();
    RadioFormat fmt = { 0, 0 };

    int16_t* pcm = NULL;
    size_t pcm_len = 0, pcm_cap = 0;
    uint8_t* chunk = (uint8_t*)malloc(chunk_bytes);
    static int16_t out[OUT_SAMPLES];

    for(;;)
    {
        size_t got = fread(chunk, 1, chunk_bytes, fi);
        if(got == 0) break;

        int n = radio_decoder_feed(dec, chunk, got, out, OUT_SAMPLES, &fmt);
        if(n <= 0) continue; /* 0 = frame incompleto, normal en stream */

        if(pcm_len + (size_t)n > pcm_cap)
        {
            while(pcm_cap == 0 || pcm_len + (size_t)n > pcm_cap)
                pcm_cap = pcm_cap ? pcm_cap * 2 : (1u << 16);
            pcm = xrealloc(pcm, pcm_cap * sizeof(int16_t));
        }
        memcpy(pcm + pcm_len, out, (size_t)n * sizeof(int16_t));
        pcm_len += (size_t)n;
    }
    fclose(fi);
    free(chunk);

    /* fin de la emision: drenar todo el carry. flush() llena `out` en cada
       llamada; repetir hasta que no queden frames. */
    for(;;)
    {
        int n = radio_decoder_flush(dec, out, OUT_SAMPLES, &fmt);
        if(n <= 0) break;

        if(pcm_len + (size_t)n > pcm_cap)
        {
            while(pcm_cap == 0 || pcm_len + (size_t)n > pcm_cap)
                pcm_cap = pcm_cap ? pcm_cap * 2 : (1u << 16);
            pcm = xrealloc(pcm, pcm_cap * sizeof(int16_t));
        }
        memcpy(pcm + pcm_len, out, (size_t)n * sizeof(int16_t));
        pcm_len += (size_t)n;
    }

    if(pcm_len == 0 || fmt.sample_rate == 0 || fmt.channels == 0)
    {
        fprintf(stderr, "no audio decoded from %s\n", argv[1]);
        return 1;
    }

    FILE* fo = fopen(argv[2], "wb");
    if(!fo) { fprintf(stderr, "cannot write %s\n", argv[2]); return 2; }

    unsigned data_len = (unsigned)pcm_len * 2u; /* 16-bit mono/stereo */
    unsigned short ch = (unsigned short)fmt.channels;
    unsigned rate = (unsigned)fmt.sample_rate;

    fwrite("RIFF", 1, 4, fo);
    unsigned riff_sz = 36u + data_len; fwrite(&riff_sz, 4, 1, fo);
    fwrite("WAVE", 1, 4, fo);
    fwrite("fmt ", 1, 4, fo);
    unsigned fmt_sz = 16u; fwrite(&fmt_sz, 4, 1, fo);
    unsigned short audio_fmt = 1u; fwrite(&audio_fmt, 2, 1, fo);  /* PCM */
    fwrite(&ch, 2, 1, fo);
    fwrite(&rate, 4, 1, fo);
    unsigned byte_rate = rate * ch * 2u; fwrite(&byte_rate, 4, 1, fo);
    unsigned short block_align = (unsigned short)(ch * 2u); fwrite(&block_align, 2, 1, fo);
    unsigned short bits = 16u; fwrite(&bits, 2, 1, fo);
    fwrite("data", 1, 4, fo);
    fwrite(&data_len, 4, 1, fo);
    fwrite(pcm, 2, pcm_len, fo);
    fclose(fo);

    free(pcm);
    radio_decoder_free(dec);

    printf("chunk %5zu B: wrote %s: %u bytes PCM, %u ch, %u Hz, %.3f s\n",
           chunk_bytes, argv[2], data_len, ch, rate, (double)pcm_len / ch / rate);
    return 0;
}