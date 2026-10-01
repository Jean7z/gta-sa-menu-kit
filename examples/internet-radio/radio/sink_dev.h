/* Internet Radio mod - contrato del backend de dispositivo (ver sink.c).
   Android : sink_opensl.c (OpenSL ES + SLAndroidBufferQueueItf, API 21+).
   Host    : test_sink.c, que simula un dispositivo que consume a ritmo real.
   Solo hay una implementacion por plataforma: el sink.c no sabe de OpenSL. */
#ifndef RADIO_SINK_DEV_H
#define RADIO_SINK_DEV_H

#include "sink.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Abre el dispositivo con el formato de s->fmt y el periodo frames_per_buf
   (= sample_rate * RADIO_SINK_FRAME_MS / 1000). El backend debe consumir
   desde el ring PCM con radio_sink_pull() a ritmo real, desde su propio hilo,
   y devolver 0 si el dispositivo queda listo o -1 si falla. Debe guardar su
   estado en s->dev_state. */
int radio_dev_open(RadioSink* s, int frames_per_buf);

/* Cierra el dispositivo y libera s->dev_state. Idempotente. */
void radio_dev_close(RadioSink* s);

/* Rellena `samples` muestras int16 desde el ring PCM. Lo que no haya todavia
   sale como silencio y se cuenta como underrun. Es lo que el backend llama
   en cada periodo; la implementacion vive en sink.c para que ambos backends
   compartan la contabilidad. */
int radio_sink_pull(RadioSink* s, int16_t* out, int samples);

#ifdef __cplusplus
}
#endif
#endif /* RADIO_SINK_DEV_H */
