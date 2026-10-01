/* Internet Radio mod - HTTP/HTTPS streaming client.
   A producer thread connects to an internet-radio URL, issues a GET and
   pushes the raw body bytes into a RadioRing; the decoder/audio side drains
   it. https:// wraps the same socket in mbedTLS (vendor/mbedtls-2.28.10),
   with the Mozilla CA bundle compiled in (ca_bundle.c), so there is no file
   to deploy and no cleartext fallback.

   Scope notes (deliberate, see slices):
   - requests Icy-MetaData: 0 so no interleaved metadata is injected; a
     stream that sends it anyway will need Slice E's metaint stripping.
   - redirects: up to 3, absolute Location only.
   - reconnects: optional (start_ex with reconnect=0 for finite sources). */
#ifndef RADIO_NET_H
#define RADIO_NET_H

#include "ring.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RadioNet {
    RadioRing* ring;
    pthread_t thread;
    volatile int stop;
    int reconnect;
    /* Socket en uso (-1 si no hay). Lo publica el hilo de red para que
       radio_net_stop() lo despierte con shutdown() en vez de esperar al
       SO_RCVTIMEO de 1 s. */
    volatile int fd;
    char url[1024];
    volatile int status;        /* last HTTP status, 0 = no response yet */
    volatile long long bytes;   /* body bytes pushed into the ring */
    char last_error[160];
} RadioNet;

/* Connects and streams forever, reconnecting on drop (radio behaviour). */
int radio_net_start(RadioNet* n, RadioRing* ring, const char* url);

/* reconnect = 0 stops after the first EOF/error (finite sources, tests).
   Returns 0 if the thread was started, -1 on bad args. */
int radio_net_start_ex(RadioNet* n, RadioRing* ring, const char* url, int reconnect);

/* Stops the producer (closes the ring so neither side can hang) and joins. */
void radio_net_stop(RadioNet* n);

const char* radio_net_last_error(const RadioNet* n);

#ifdef __cplusplus
}
#endif
#endif /* RADIO_NET_H */
