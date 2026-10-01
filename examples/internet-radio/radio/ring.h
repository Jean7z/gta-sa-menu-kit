/* Internet Radio mod - bounded byte queue between the network thread
   (producer) and the audio/decoder side (consumer).
   Slice B1: minimum viable SPSC ring - one mutex, two condvars, monotonic
   head/tail counters (no wrap ambiguity, no index arithmetic tricks). */
#ifndef RADIO_RING_H
#define RADIO_RING_H

#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RadioRing {
    unsigned char* buf;
    size_t cap;   /* bytes */
    size_t head;  /* bytes consumed (monotonic) */
    size_t tail;  /* bytes produced (monotonic) */
    int closed;   /* no more writes; readers drain then get 0 */
    pthread_mutex_t mtx;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} RadioRing;

/* cap = bytes of payload to hold (>= 1). Returns 0 on success, -1 on error. */
int radio_ring_init(RadioRing* r, size_t cap);
void radio_ring_free(RadioRing* r);

/* Copies up to len bytes; blocks while full. Returns bytes written (< len
   only if the ring got closed meanwhile). */
size_t radio_ring_write(RadioRing* r, const void* data, size_t len);

/* Blocks until at least 1 byte is available (or closed). Returns bytes read,
   or 0 once closed and fully drained. */
size_t radio_ring_read(RadioRing* r, void* out, size_t len);

/* Igual que radio_ring_read() pero NUNCA espera: devuelve 0 si todavia no
   hay datos. Para el consumidor de audio (callback de OpenSL), que no
   puede bloquearse; ahi lo que falte se rellena de silencio. */
size_t radio_ring_read_nonblock(RadioRing* r, void* out, size_t len);

/* Bytes currently buffered. */
size_t radio_ring_available(RadioRing* r);

/* Abort pending/future writes and wake every waiter. */
void radio_ring_close(RadioRing* r);

#ifdef __cplusplus
}
#endif
#endif /* RADIO_RING_H */
