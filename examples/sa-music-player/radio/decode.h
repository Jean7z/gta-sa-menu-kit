/* SA Music Player mod - MP3 decoder (minimp3 wrapper), streaming-friendly.
   Slice 1: decoder verificado en host (mismo .c compila con NDK). */
#ifndef RADIO_DECODE_H
#define RADIO_DECODE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int channels;    /* 1 = mono, 2 = stereo */
    int sample_rate; /* Hz */
} RadioFormat;

typedef struct RadioDecoder RadioDecoder;

/* Crea un decoder (estado minimp3 propio). */
RadioDecoder* radio_decoder_new(void);
void radio_decoder_free(RadioDecoder* d);

/* Alimenta un trozo del stream MP3 y decodifica todos los frames completos
   que entren en `out`; el sobrante queda en el buffer interno (carry) para
   la siguiente llamada, asi un frame partido entre dos chunks no se pierde.
   - out:              PCM int16 interleaved (mono: 1 muestra/frame, stereo: 2).
   - out_capacity:     capacidad de `out` en MUESTRAS int16 (no frames).
   - fmt (out):        se rellena en la primera decodificacion (canales+rate).
   Devuelve el numero de muestras int16 escritas; 0 si el trozo no cerro
   ningun frame (normal al inicio del stream). Nunca negativo. */
int radio_decoder_feed(RadioDecoder* d, const uint8_t* chunk, size_t len,
                       int16_t* out, size_t out_capacity, RadioFormat* fmt);

/* Fin de la emision: decodifica los frames completos que queden en el
   buffer interno (igual contrato de out/out_capacity/fmt que feed()).
   Drenar en bucle hasta que devuelva 0 (puede sacar varios frames por
   llamada si el carry es grande); descarta la cabecera rota final.
   Devuelve el numero de muestras int16 escritas. */
int radio_decoder_flush(RadioDecoder* d, int16_t* out, size_t out_capacity,
                        RadioFormat* fmt);

#endif /* RADIO_DECODE_H */