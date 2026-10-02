/* SA Music Player mod - backend OpenSL ES del sink (Android, API 24).
   Aqui no hay nada de ritmo: OpenSL ES ya consume a velocidad de hardware.
   El unico trabajo es copiar el PCM del ring al buffer que toca; si aun no
   hay PCM, radio_sink_pull() mete silencio y cuenta el underrun.

   El NDK expone SLAndroidSimpleBufferQueueItf, que NO tiene GetBuffer: cada
   Enqueue lleva su propio puntero, asi que el pool de buffers es nuestro y
   el callback va rotando sobre el. Solo lo toca el hilo de OpenSL, asi que no
   hace falta cerrojo. */
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <stdlib.h>
#include <string.h>

#include "sink.h"
#include "sink_dev.h"

/* Buffers en vuelo. Con RADIO_SINK_FRAME_MS=20 son ~60 ms de cola. */
#define RADIO_SINK_NBUF 3

typedef struct {
    SLObjectItf engine_obj;
    SLEngineItf engine;
    SLObjectItf mix_obj;
    SLObjectItf player_obj;
    SLPlayItf   play;
    SLAndroidSimpleBufferQueueItf bq;
    unsigned char* pool;
    int   bytes;   /* bytes por buffer */
    int   samples; /* muestras int16 por buffer */
    int   next;    /* buffer que toca rellenar (solo hilo de OpenSL) */
} RadioDev;

static void SLAPIENTRY bq_callback(SLAndroidSimpleBufferQueueItf bq, void* ctx)
{
    RadioSink* s = (RadioSink*)ctx;
    RadioDev* d = (RadioDev*)s->dev_state;
    unsigned char* buf;

    if(!d) return;
    buf = d->pool + (size_t)d->next * (size_t)d->bytes;
    d->next = (d->next + 1) % RADIO_SINK_NBUF;

    radio_sink_pull(s, (int16_t*)buf, d->samples);
    (*bq)->Enqueue(bq, buf, (SLuint32)d->bytes);
}

int radio_dev_open(RadioSink* s, int frames_per_buf)
{
    RadioDev* d;
    SLDataLocator_AndroidSimpleBufferQueue bqloc;
    SLDataFormat_PCM pcmfmt;
    SLDataSource src;
    SLDataLocator_OutputMix mixloc;
    SLDataSink dst;
    const SLInterfaceID iface[1] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
    const SLboolean     req[1]  = { SL_BOOLEAN_TRUE };
    int i;

    if(frames_per_buf <= 0 || s->fmt.channels < 1 || s->fmt.channels > 2) return -1;

    d = (RadioDev*)calloc(1, sizeof(*d));
    if(!d) return -1;
    d->samples = frames_per_buf * s->fmt.channels;
    d->bytes   = d->samples * (int)sizeof(int16_t);
    d->pool    = (unsigned char*)calloc(RADIO_SINK_NBUF, (size_t)d->bytes);
    if(!d->pool) { free(d); return -1; }
    s->dev_state = d;

    if(slCreateEngine(&d->engine_obj, 0, NULL, 0, NULL, NULL) != SL_RESULT_SUCCESS) goto fail;
    if((*d->engine_obj)->Realize(d->engine_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) goto fail;
    if((*d->engine_obj)->GetInterface(d->engine_obj, SL_IID_ENGINE, &d->engine) != SL_RESULT_SUCCESS) goto fail;

    if((*d->engine)->CreateOutputMix(d->engine, &d->mix_obj, 0, NULL, NULL) != SL_RESULT_SUCCESS) goto fail;
    if((*d->mix_obj)->Realize(d->mix_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) goto fail;

    /* Formato PCM real del stream. OJO: samplesPerSec va en MILLIhercios. */
    memset(&pcmfmt, 0, sizeof(pcmfmt));
    pcmfmt.formatType    = SL_DATAFORMAT_PCM;
    pcmfmt.numChannels   = (SLuint32)s->fmt.channels;
    pcmfmt.samplesPerSec = (SLuint32)(s->fmt.sample_rate * 1000);
    pcmfmt.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
    pcmfmt.containerSize = SL_PCMSAMPLEFORMAT_FIXED_16;
    pcmfmt.channelMask   = (s->fmt.channels == 1)
                          ? SL_SPEAKER_FRONT_CENTER
                          : (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT);
    pcmfmt.endianness    = SL_BYTEORDER_LITTLEENDIAN;

    bqloc.locatorType = SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE;
    bqloc.numBuffers = RADIO_SINK_NBUF;
    /* En este NDK el locator no lleva formato: va en pFormat. */
    src.pLocator = &bqloc;
    src.pFormat  = &pcmfmt;

    mixloc.locatorType = SL_DATALOCATOR_OUTPUTMIX;
    mixloc.outputMix   = d->mix_obj;
    dst.pLocator = &mixloc;
    dst.pFormat  = NULL;

    if((*d->engine)->CreateAudioPlayer(d->engine, &d->player_obj, &src, &dst,
                                       1, iface, req) != SL_RESULT_SUCCESS) goto fail;
    /* Sin SetPriority: el default (NORMAL, preemptable) ya es lo que queremos,
       y este NDK pide 3 args. */
    if((*d->player_obj)->Realize(d->player_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) goto fail;
    if((*d->player_obj)->GetInterface(d->player_obj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &d->bq) != SL_RESULT_SUCCESS) goto fail;

    /* Encolar el pool completo (ya en silencio) para que el consumo arranque. */
    for(i = 0; i < RADIO_SINK_NBUF; i++)
        if((*d->bq)->Enqueue(d->bq, d->pool + (size_t)i * (size_t)d->bytes,
                             (SLuint32)d->bytes) != SL_RESULT_SUCCESS) goto fail;

    if((*d->bq)->RegisterCallback(d->bq, bq_callback, s) != SL_RESULT_SUCCESS) goto fail;
    if((*d->player_obj)->GetInterface(d->player_obj, SL_IID_PLAY, &d->play) != SL_RESULT_SUCCESS) goto fail;
    if((*d->play)->SetPlayState(d->play, SL_PLAYSTATE_PLAYING) != SL_RESULT_SUCCESS) goto fail;
    return 0;

fail:
    radio_dev_close(s);
    return -1;
}

void radio_dev_close(RadioSink* s)
{
    RadioDev* d = (RadioDev*)s->dev_state;
    if(!d) return;
    s->dev_state = NULL;

    /* Quitar el callback ANTES de destruir: el hilo de OpenSL puede estar
       dentro de bq_callback leyendo el ring PCM en este momento. */
    if(d->bq)         (*d->bq)->RegisterCallback(d->bq, NULL, NULL);
    if(d->play)       (*d->play)->SetPlayState(d->play, SL_PLAYSTATE_STOPPED);
    if(d->bq)         (*d->bq)->Clear(d->bq);
    if(d->player_obj) (*d->player_obj)->Destroy(d->player_obj);
    if(d->mix_obj)    (*d->mix_obj)->Destroy(d->mix_obj);
    if(d->engine_obj) (*d->engine_obj)->Destroy(d->engine_obj);
    free(d->pool);
    free(d);
}
