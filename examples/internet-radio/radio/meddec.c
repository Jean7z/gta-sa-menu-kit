/* Internet Radio mod - decoder de fichero local (ver meddec.h).
   Threads: se usa desde el hilo de decode del sink. No es reentrante por
   instancia (un solo hilo por radio_meddec), que es justo lo que hace falta. */
#include "meddec.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaCodec.h>

/* Las claves AMEDIAFORMAT_KEY_* no existen en el NDK: son de Java. Literales. */
#define K_MIME     "mime"
#define K_RATE     "sample-rate"
#define K_CH       "channel-count"
#define K_PCM      "pcm-encoding"

/* ENCODING_PCM_16BIT. Es lo que dan los tres formatos medidos (flac, aac, opus).
   Si algun dia saliera 24 bits o float, el sink lo dira en vez de sonar a ruido. */
#define PCM_ENCODING_16BIT 2

/* Tras meter EOS el decoder necesita unas vueltas para drenar su cola antes de
   marcar EOS en la salida. Con un bucle de dequeue sin dormir eso son miles de
   vueltas, no una: 2000 es holgado para darlo por muerto sin falso positivo. */
#define MEDDEC_STALL_LIMIT 2000
/* Techo de trabajo por vuelta de pump(). Acota el pisotón de CPU para que el
   control vuelva al sink (quien impone el ritmo) aunque el codec no rinda. */
#define MEDDEC_MAX_SPINS   4000

struct RadioMeddec {
    int fd;
    AMediaExtractor* ex;
    AMediaCodec* dec;
    char* mime;
    char err[128];
    int  eos;        /* la pista se leyo entera */
    int  eof;        /* el extractor se agoto (se metio EOS en el codec) */
    int  failed;     /* error duro: hay que abandonar la pista */
    int  stall;      /* vueltas sin progreso tras EOF */
    int16_t* stage;  /* PCM decodificado pendiente de entregar */
    size_t stage_cap;
    size_t stage_len;
    size_t stage_pos;
    RadioFormat fmt;
    int  have_fmt;
};

static void fail(RadioMeddec* d, const char* what)
{
    if(!d->err[0]) snprintf(d->err, sizeof d->err, "%s", what);
    d->failed = 1;
}

static int gi32(AMediaFormat* f, const char* k, int32_t dflt)
{
    int32_t v = dflt;
    AMediaFormat_getInt32(f, k, &v);
    return v;
}

static const char* gstr(AMediaFormat* f, const char* k, char* buf, size_t n)
{
    const char* v = NULL;
    if(AMediaFormat_getString(f, k, &v) && v) { snprintf(buf, n, "%s", v); return buf; }
    buf[0] = 0;
    return buf;
}

RadioMeddec* radio_meddec_open(const char* path, char* why, size_t why_len)
{
    RadioMeddec* d;
    struct stat st;
    char mb[128] = {0};
    AMediaFormat* fmt = NULL;
    ssize_t sel = -1;
    size_t t, ntracks;
    int fd;

    if(why && why_len) why[0] = 0;
    if(!path) { if(why && why_len) snprintf(why, why_len, "ruta nula"); return NULL; }

    d = (RadioMeddec*)calloc(1, sizeof(*d));
    if(!d) { if(why && why_len) snprintf(why, why_len, "sin memoria"); return NULL; }
    d->fd = -1;

    fd = open(path, O_RDONLY);
    if(fd < 0) { snprintf(d->err, sizeof d->err, "no se pudo abrir"); goto bad; }
    d->fd = fd;
    if(fstat(fd, &st) != 0) { snprintf(d->err, sizeof d->err, "no se pudo stat"); goto bad; }

    d->ex = AMediaExtractor_new();
    if(!d->ex) { snprintf(d->err, sizeof d->err, "sin extractor"); goto bad; }

    /* SIEMPRE por fd: setDataSource(ruta) exige hilo de Java. Ver meddec.h. */
    if(AMediaExtractor_setDataSourceFd(d->ex, fd, 0, st.st_size) != AMEDIA_OK)
    {
        snprintf(d->err, sizeof d->err, "setDataSourceFd fallo (no es media?)");
        goto bad;
    }

    ntracks = (size_t)AMediaExtractor_getTrackCount(d->ex);
    for(t = 0; t < ntracks; t++)
    {
        fmt = AMediaExtractor_getTrackFormat(d->ex, t);
        if(!fmt) continue;
        gstr(fmt, K_MIME, mb, sizeof mb);
        if(!mb[0]) { AMediaFormat_delete(fmt); fmt = NULL; continue; }
        if(strncmp(mb, "audio/", 6) == 0) { snprintf(mb, sizeof mb, "%s", mb); sel = (ssize_t)t; break; }
        AMediaFormat_delete(fmt);
        fmt = NULL;
    }
    if(sel < 0) { snprintf(d->err, sizeof d->err, "sin pista de audio"); goto bad; }
    d->mime = strdup(mb);

    AMediaExtractor_selectTrack(d->ex, (size_t)sel);
    fmt = AMediaExtractor_getTrackFormat(d->ex, (size_t)sel);
    if(!fmt) { snprintf(d->err, sizeof d->err, "sin formato"); goto bad; }

    d->dec = AMediaCodec_createDecoderByType(d->mime);
    if(!d->dec)
    {
        snprintf(d->err, sizeof d->err, "el dispositivo no decodifica '%s'", d->mime);
        goto bad;
    }
    if(AMediaCodec_configure(d->dec, fmt, NULL, NULL, 0) != AMEDIA_OK)
    { snprintf(d->err, sizeof d->err, "configure fallo para '%s'", d->mime); goto bad; }
    if(AMediaCodec_start(d->dec) != AMEDIA_OK)
    { snprintf(d->err, sizeof d->err, "start fallo para '%s'", d->mime); goto bad; }
    AMediaFormat_delete(fmt);
    fmt = NULL;

    d->stage_cap = 16384;              /* ~370 ms a 44.1 kHz estereo */
    d->stage = (int16_t*)malloc(d->stage_cap * sizeof(int16_t));
    if(!d->stage) { snprintf(d->err, sizeof d->err, "sin memoria"); goto bad; }
    return d;

bad:
    if(fmt) AMediaFormat_delete(fmt);
    /* El motivo vive en d->err y hay que sacarlo ANTES de cerrar: close() lo
       libera. Sin esto el llamador solo veria "NULL". */
    if(why && why_len) snprintf(why, why_len, "%s", d->err[0] ? d->err : "fallo desconocido");
    radio_meddec_close(d);
    return NULL;
}

void radio_meddec_close(RadioMeddec* d)
{
    if(!d) return;
    if(d->dec) { AMediaCodec_stop(d->dec); AMediaCodec_delete(d->dec); }
    if(d->ex)  AMediaExtractor_delete(d->ex);
    if(d->fd >= 0) close(d->fd);
    free(d->mime);
    free(d->stage);
    free(d);
}

const char* radio_meddec_err(const RadioMeddec* d)  { return d ? d->err : "null"; }
const char* radio_meddec_mime(const RadioMeddec* d) { return d && d->mime ? d->mime : "?"; }
int radio_meddec_done(const RadioMeddec* d)        { return d ? d->eos : 0; }

/* Una vuelta de la bomba: alimenta al codec y recoge la salida a `stage`.
   Devuelve muestras depositadas, o -1 si fallo. No espera: los dequeue son
   con timeout 0 y el extractor lee de disco.
   `max` acota el trabajo por vuelta para que la bomba devuelva el control al
   sink (que es quien impone el ritmo) aunque el codec se atasque. */
static int pump(RadioMeddec* d, int max)
{
    AMediaCodecBufferInfo info;
    int spins = 0;

    d->stage_len = 0;
    d->stage_pos = 0;
    if(d->failed) return -1;

    memset(&info, 0, sizeof info);

    for(;;)
    {
        ssize_t ii, oi;

        if(++spins > max) return d->stage_len ? (int)d->stage_len : 0;

        /* --- entrada --- */
        ii = AMediaCodec_dequeueInputBuffer(d->dec, 0);
        if(ii >= 0)
        {
            size_t cap = 0;
            uint8_t* buf = AMediaCodec_getInputBuffer(d->dec, (size_t)ii, &cap);
            if(buf && cap)
            {
                ssize_t rd = AMediaExtractor_readSampleData(d->ex, buf, cap);
                if(rd < 0)
                {
                    d->eof = 1;
                    AMediaCodec_queueInputBuffer(d->dec, (size_t)ii, 0, 0, 0,
                                                 AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                }
                else
                {
                    int64_t ts = AMediaExtractor_getSampleTime(d->ex);
                    int sync = (AMediaExtractor_getSampleFlags(d->ex) &
                                AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC) != 0;
                    AMediaCodec_queueInputBuffer(d->dec, (size_t)ii, 0, (size_t)rd,
                                                 (uint64_t)ts,
                                                 sync ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME : 0);
                    AMediaExtractor_advance(d->ex);
                }
            }
            else
            {
                d->eof = 1;
                AMediaCodec_queueInputBuffer(d->dec, (size_t)ii, 0, 0, 0,
                                             AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            }
        }

        /* --- salida --- */
        oi = AMediaCodec_dequeueOutputBuffer(d->dec, &info, 0);
        if(oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
        {
            /* AKI el formato que manda es el de aqui, no el del fichero:
               el decodificador remuestrea (m4a 22050 -> salida 44100). */
            AMediaFormat* of = AMediaCodec_getOutputFormat(d->dec);
            if(of)
            {
                int32_t pcmenc = gi32(of, K_PCM, PCM_ENCODING_16BIT);
                d->fmt.sample_rate = gi32(of, K_RATE, 0);
                d->fmt.channels    = gi32(of, K_CH, 0);
                AMediaFormat_delete(of);

                if(pcmenc != PCM_ENCODING_16BIT)
                {
                    snprintf(d->err, sizeof d->err,
                             "el decodificador entrega pcm_encoding=%d, solo se admite 16 bits",
                             pcmenc);
                    d->failed = 1;
                }
                else if(d->fmt.sample_rate <= 0 || d->fmt.channels < 1 || d->fmt.channels > 2)
                {
                    snprintf(d->err, sizeof d->err,
                             "formato no soportado por el sink: %d Hz/%d ch",
                             d->fmt.sample_rate, d->fmt.channels);
                    d->failed = 1;
                }
                else
                {
                    d->have_fmt = 1;
                }
            }
        }
        else if(oi >= 0)
        {
            if(info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) d->eos = 1;
            if(!(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) && info.size > 0)
            {
                size_t nsamp = (size_t)info.size / sizeof(int16_t);
                if(nsamp > d->stage_cap) nsamp = d->stage_cap;
                if(nsamp)
                {
                    size_t osz = 0;
                    uint8_t* ob = AMediaCodec_getOutputBuffer(d->dec, (size_t)oi, &osz);
                    if(ob)
                    {
                        size_t copy = info.size;
                        if(copy > osz) copy = osz;
                        if(copy > nsamp * sizeof(int16_t)) copy = nsamp * sizeof(int16_t);
                        memcpy(d->stage, ob, copy);
                        d->stage_len = copy / sizeof(int16_t);
                    }
                }
            }
            AMediaCodec_releaseOutputBuffer(d->dec, (size_t)oi, false);
            if(d->failed)   return -1;
            if(d->stage_len) return (int)d->stage_len;
            if(d->eos)      return 0;
            d->stall = 0;
        }
        else if(oi == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        {
            /* Tras marcar EOS el decoder necesita unas vueltas para drenar su
               cola: NO es un atasco. Solo si se queda sin progreso mucho rato
               damos el codec por muerto. */
            if(d->eof && ++d->stall > MEDDEC_STALL_LIMIT)
            {
                fail(d, "el decodificador se atascó antes de finish");
                return -1;
            }
        }

        if(d->eos) return 0;
    }
}

int radio_meddec_read(RadioMeddec* d, int16_t* out, size_t out_capacity,
                      RadioFormat* fmt)
{
    size_t left, want, n;
    int got;

    if(!d || !out || out_capacity == 0) return -1;
    if(d->failed) return -1;

    /* 1) vaciar lo que quedara de la vuelta anterior */
    if(d->stage_pos < d->stage_len)
    {
        left  = d->stage_len - d->stage_pos;
        want  = out_capacity < left ? out_capacity : left;
        memcpy(out, d->stage + d->stage_pos, want * sizeof(int16_t));
        d->stage_pos += want;
        if(fmt && d->have_fmt) *fmt = d->fmt;
        return (int)want;
    }

    /* 2) bombear hasta que haya PCM, se termine la pista o falle */
    if(d->eos) { if(fmt && d->have_fmt) *fmt = d->fmt; return 0; }
    got = pump(d, MEDDEC_MAX_SPINS);
    if(got < 0) { if(fmt && d->have_fmt) *fmt = d->fmt; return -1; }
    if(got == 0) { if(fmt && d->have_fmt) *fmt = d->fmt; return 0; }

    n = (size_t)got < out_capacity ? (size_t)got : out_capacity;
    memcpy(out, d->stage, n * sizeof(int16_t));
    d->stage_pos = n;
    if(fmt && d->have_fmt) *fmt = d->fmt;
    return (int)n;
}
