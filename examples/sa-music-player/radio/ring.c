/* SA Music Player mod - ring buffer (see ring.h). */
#include "ring.h"

#include <stdlib.h>
#include <string.h>

int radio_ring_init(RadioRing* r, size_t cap)
{
    memset(r, 0, sizeof(*r));
    if(cap == 0) return -1;

    r->buf = (unsigned char*)malloc(cap);
    if(!r->buf) return -1;
    r->cap = cap;

    if(pthread_mutex_init(&r->mtx, NULL) != 0) { free(r->buf); r->buf = NULL; return -1; }
    pthread_cond_init(&r->not_empty, NULL);
    pthread_cond_init(&r->not_full, NULL);
    return 0;
}

void radio_ring_free(RadioRing* r)
{
    if(!r) return;
    pthread_mutex_destroy(&r->mtx);
    pthread_cond_destroy(&r->not_empty);
    pthread_cond_destroy(&r->not_full);
    free(r->buf);
    memset(r, 0, sizeof(*r));
}

static size_t ring_copy_in(RadioRing* r, const unsigned char* src, size_t len)
{
    size_t idx = r->tail % r->cap;
    size_t first = r->cap - idx;
    if(first > len) first = len;

    memcpy(r->buf + idx, src, first);
    if(len > first) memcpy(r->buf, src + first, len - first);
    return len;
}

static size_t ring_copy_out(RadioRing* r, unsigned char* dst, size_t len)
{
    size_t idx = r->head % r->cap;
    size_t first = r->cap - idx;
    if(first > len) first = len;

    memcpy(dst, r->buf + idx, first);
    if(len > first) memcpy(dst + first, r->buf, len - first);
    return len;
}

size_t radio_ring_write(RadioRing* r, const void* data, size_t len)
{
    const unsigned char* p = (const unsigned char*)data;
    size_t written = 0;

    pthread_mutex_lock(&r->mtx);
    while(written < len)
    {
        size_t space;

        while(!r->closed && (r->tail - r->head) >= r->cap)
            pthread_cond_wait(&r->not_full, &r->mtx);
        if(r->closed) break; /* consumer is gone: do not block forever */

        space = r->cap - (r->tail - r->head);
        if(space > len - written) space = len - written;

        ring_copy_in(r, p + written, space);
        r->tail += space;
        written += space;

        pthread_cond_signal(&r->not_empty);
    }
    pthread_mutex_unlock(&r->mtx);
    return written;
}

size_t radio_ring_read(RadioRing* r, void* out, size_t len)
{
    size_t got = 0;

    pthread_mutex_lock(&r->mtx);
    while(got == 0)
    {
        size_t avail;

        while(!r->closed && r->tail == r->head)
            pthread_cond_wait(&r->not_empty, &r->mtx);

        avail = r->tail - r->head;
        if(avail == 0) break; /* closed and drained */

        if(avail > len) avail = len;
        ring_copy_out(r, (unsigned char*)out, avail);
        r->head += avail;
        got = avail;

        pthread_cond_signal(&r->not_full);
    }
    pthread_mutex_unlock(&r->mtx);
    return got;
}

size_t radio_ring_read_nonblock(RadioRing* r, void* out, size_t len)
{
    size_t got;

    if(len == 0) return 0;
    pthread_mutex_lock(&r->mtx);

    got = r->tail - r->head;
    if(got > len) got = len;
    if(got)
    {
        ring_copy_out(r, (unsigned char*)out, got);
        r->head += got;
        pthread_cond_signal(&r->not_full);
    }
    pthread_mutex_unlock(&r->mtx);
    return got;
}

size_t radio_ring_available(RadioRing* r)
{
    size_t n;

    pthread_mutex_lock(&r->mtx);
    n = r->tail - r->head;
    pthread_mutex_unlock(&r->mtx);
    return n;
}

void radio_ring_discard(RadioRing* r)
{
    if(!r) return;
    pthread_mutex_lock(&r->mtx);
    r->head = r->tail;
    pthread_cond_broadcast(&r->not_full);
    pthread_mutex_unlock(&r->mtx);
}

void radio_ring_close(RadioRing* r)
{
    pthread_mutex_lock(&r->mtx);
    r->closed = 1;
    pthread_cond_broadcast(&r->not_empty);
    pthread_cond_broadcast(&r->not_full);
    pthread_mutex_unlock(&r->mtx);
}
