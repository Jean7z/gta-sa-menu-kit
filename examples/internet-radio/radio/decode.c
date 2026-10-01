/* Internet Radio mod - MP3 decoder (minimp3 wrapper), streaming-friendly.
   minimp3 decodifica UN frame por llamada. Su slow path hace
   memset(dec,0) (destruye el bit reservoir L3 + filtro QMF) cuando el buffer
   no contiene un frame completo + header del siguiente; eso es PERDIDA de
   audio en streaming por chunks. Para evitarlo solo llamamos a
   mp3dec_decode_frame cuando el buffer tiene >= RADIO_MIN_WINDOW bytes
   (frame max estandar + header siguiente garantizado), conservando el fast
   path que NO resetea estado. El sobrante queda en un carry interno;
   el buffer crece dinamicamente (chunks de cualquier tamano). */
#include "decode.h"

#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

/* Frame MP3 maximo estandar (MPEG1 L2 @32kHz 384kbps = 1728; free format
   hasta MAX_FREE_FORMAT_FRAME_SIZE=2304) + header del siguiente frame.
   Con esta cantidad presente, el fast path de minimp3 siempre puede
   decodificar el frame cabeza sin resetear dec->reserv_buf/qmf_state. */
#define RADIO_MIN_WINDOW  (MAX_FREE_FORMAT_FRAME_SIZE + HDR_SIZE) /* 2308 */

/* Techo del carry por si llega basura no-MP3 (evita crecer sin limite);
   si se supera, se descarta la mitad mas vieja. Un stream MP3 real nunca
   deberia acercarse (el llamador drena out en cada ciclo); el techo alto
   evita romper un stream valido con chunks muy grandes. */
#define RADIO_CARRY_MAX (256u * 1024u)

struct RadioDecoder {
    mp3dec_t dec;
    uint8_t* buf;      /* ventana dinamica */
    size_t buf_cap;    /* bytes asignados */
    size_t carry_len;  /* bytes validos al inicio de buf */
    RadioFormat fmt;
    int fmt_valid;
};

RadioDecoder* radio_decoder_new(void)
{
    RadioDecoder* d = (RadioDecoder*)calloc(1, sizeof(RadioDecoder));
    if(!d) return NULL;
    mp3dec_init(&d->dec);
    return d;
}

void radio_decoder_free(RadioDecoder* d)
{
    if(!d) return;
    free(d->buf);
    free(d);
}

int radio_decoder_feed(RadioDecoder* d, const uint8_t* chunk, size_t len,
                       int16_t* out, size_t out_capacity, RadioFormat* fmt)
{
    if(!d || !chunk || !out || !fmt) return 0;

    if(d->carry_len > RADIO_CARRY_MAX)
    {
        size_t drop = d->carry_len / 2;
        memmove(d->buf, d->buf + drop, d->carry_len - drop);
        d->carry_len -= drop;
    }

    size_t need = d->carry_len + len;
    if(need > d->buf_cap)
    {
        size_t nc = need + 1024;
        uint8_t* nb = (uint8_t*)realloc(d->buf, nc);
        if(!nb) return 0; /* mantener estado, siguiente llamada reintenta */
        d->buf = nb;
        d->buf_cap = nc;
    }
    memcpy(d->buf + d->carry_len, chunk, len);
    size_t n = d->carry_len + len;

    size_t pos = 0, written = 0; /* written = muestras int16 interleaved */
    while(pos + HDR_SIZE <= n && written + MINIMP3_MAX_SAMPLES_PER_FRAME <= out_capacity)
    {
        /* no llamar a minimp3 salvo con frame completo + header siguiente */
        if(n - pos < RADIO_MIN_WINDOW) break;

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&d->dec, d->buf + pos, (int)(n - pos),
                                          out + written, &info);
        if(samples > 0)
        {
            if(!d->fmt_valid)
            {
                d->fmt.channels = info.channels;
                d->fmt.sample_rate = info.hz;
                d->fmt_valid = 1;
            }
            /* muestras por canal * canales = int16 escritos por el frame.
               Los streams MP3 no cambian de canales/rate a mitad de emision;
               si ocurriera, el sink (slice C) re-sincronizara. */
            written += (size_t)samples * (size_t)info.channels;
            pos += (size_t)info.frame_bytes;
        }
        else if((size_t)info.frame_bytes < n - pos)
        {
            /* minimp3 consumio bytes sin decodificar (frame completo pero
               indecodificable: reserva L3 rota por resync, frame corrupto):
               ya fueron escaneados, saltarlos */
            pos += (size_t)info.frame_bytes;
        }
        else
        {
            /* frame_bytes == resto: no avanza nada, romper para no ciclar */
            break;
        }
    }

    d->carry_len = n - pos;
    memmove(d->buf, d->buf + pos, d->carry_len);

    if(d->fmt_valid) *fmt = d->fmt;
    return (int)written;
}

int radio_decoder_flush(RadioDecoder* d, int16_t* out, size_t out_capacity,
                        RadioFormat* fmt)
{
    /* Fin de stream: decodifica los frames completos del carry aunque queden
       menos de RADIO_MIN_WINDOW bytes. La cabecera sobrante se descarta. */
    if(!d || !out || !fmt) return 0;

    size_t pos = 0, written = 0;
    while(pos + HDR_SIZE <= d->carry_len &&
          written + MINIMP3_MAX_SAMPLES_PER_FRAME <= out_capacity)
    {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&d->dec, d->buf + pos,
                                          (int)(d->carry_len - pos),
                                          out + written, &info);
        if(samples > 0)
        {
            if(!d->fmt_valid)
            {
                d->fmt.channels = info.channels;
                d->fmt.sample_rate = info.hz;
                d->fmt_valid = 1;
            }
            written += (size_t)samples * (size_t)info.channels;
            pos += (size_t)info.frame_bytes;
        }
        else if((size_t)info.frame_bytes < d->carry_len - pos)
        {
            pos += (size_t)info.frame_bytes;
        }
        else
        {
            break;
        }
    }

    d->carry_len -= pos;
    memmove(d->buf, d->buf + pos, d->carry_len);

    if(d->fmt_valid) *fmt = d->fmt;
    return (int)written;
}