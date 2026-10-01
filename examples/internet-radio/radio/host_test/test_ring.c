/* Host test para radio/ring.c: semantica de buffer lleno (el productor debe
   bloquearse, no perder bytes) e integridad byte a byte bajo consumo lento.
   Uso: test_ring */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../ring.h"

#define CAP   1024
#define TOTAL 2500 /* > CAP: fuerza el bloqueo del productor */

static RadioRing g_ring;
static unsigned char g_src[TOTAL];
static size_t g_written;

static unsigned char expect(size_t i) { return (unsigned char)((i * 7u + 3u) & 0xffu); }

static void* producer(void* arg)
{
    (void)arg;
    g_written = radio_ring_write(&g_ring, g_src, TOTAL);
    return NULL;
}

/* 1) el productor se bloquea con el ring lleno y nada se pierde al drenar */
static int test_backpressure(void)
{
    size_t avail_blocked, got_total = 0;
    unsigned char buf[256];
    pthread_t th;

    for(size_t i = 0; i < TOTAL; i++) g_src[i] = expect(i);
    g_written = 0;

    if(radio_ring_init(&g_ring, CAP) != 0) { printf("FAIL: ring_init\n"); return 1; }
    if(pthread_create(&th, NULL, producer, NULL) != 0) { printf("FAIL: pthread_create\n"); return 1; }

    usleep(100000); /* tiempo de sobra para llenar CAP y bloquearse */
    avail_blocked = radio_ring_available(&g_ring);
    printf("ring lleno: %zu/%d bytes, productor bloqueado\n", avail_blocked, CAP);

    while(got_total < TOTAL)
    {
        size_t n = radio_ring_read(&g_ring, buf, sizeof(buf));
        if(n == 0) break;

        for(size_t i = 0; i < n; i++)
        {
            if(buf[i] != expect(got_total + i))
            {
                printf("FAIL: integridad rota en el byte %zu\n", got_total + i);
                return 1;
            }
        }
        got_total += n;
        usleep(200); /* consumidor lento: obliga al productor a esperar */
    }
    pthread_join(th, NULL);
    radio_ring_free(&g_ring);

    printf("escritos %zu/%d, leidos %zu/%d\n", g_written, TOTAL, got_total, TOTAL);
    if(avail_blocked != CAP) { printf("FAIL: no llego a llenarse\n"); return 1; }
    if(g_written != TOTAL)   { printf("FAIL: se perdieron bytes escritos\n"); return 1; }
    if(got_total != TOTAL)   { printf("FAIL: se perdieron bytes leidos\n"); return 1; }
    return 0;
}

/* 2) close() desbloquea a un productor atascado (no cuelga) */
static int test_close_unblocks(void)
{
    pthread_t th;

    g_written = 0;
    if(radio_ring_init(&g_ring, CAP) != 0) { printf("FAIL: ring_init\n"); return 1; }
    if(pthread_create(&th, NULL, producer, NULL) != 0) { printf("FAIL: pthread_create\n"); return 1; }

    usleep(100000);
    radio_ring_close(&g_ring); /* con el ring lleno, el productor esta esperando */
    pthread_join(th, NULL);    /* debe terminar, no colgarse */
    radio_ring_free(&g_ring);

    printf("close() con ring lleno: escritos %zu/%d (parcial, sin colgarse)\n", g_written, TOTAL);
    if(g_written == 0 || g_written > TOTAL) { printf("FAIL: escritura fuera de rango\n"); return 1; }
    return 0;
}

/* 3) lectura sin bloqueo: 0 si esta vacio, y nunca espera (es el camino que
   usa el callback de OpenSL, que no puede dormirse) */
static int test_read_nonblock(void)
{
    unsigned char buf[64];
    const unsigned char payload[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    size_t n;

    if(radio_ring_init(&g_ring, CAP) != 0) { printf("FAIL: ring_init\n"); return 1; }

    n = radio_ring_read_nonblock(&g_ring, buf, sizeof(buf));
    if(n != 0) { printf("FAIL: vacio devolvio %zu, esperaba 0\n", n); return 1; }

    if(radio_ring_write(&g_ring, payload, sizeof(payload)) != sizeof(payload))
    { printf("FAIL: write\n"); return 1; }

    n = radio_ring_read_nonblock(&g_ring, buf, sizeof(buf));
    if(n != sizeof(payload)) { printf("FAIL: leidos %zu, esperados %zu\n", n, sizeof(payload)); return 1; }
    if(memcmp(buf, payload, sizeof(payload)) != 0) { printf("FAIL: datos distintos\n"); return 1; }

    n = radio_ring_read_nonblock(&g_ring, buf, sizeof(buf));
    if(n != 0) { printf("FAIL: drenado devolvio %zu\n", n); return 1; }

    /* 2000 intentos vacios: si alguno bloqueara, el test se colgaria aqui */
    for(int i = 0; i < 2000; i++)
        if(radio_ring_read_nonblock(&g_ring, buf, sizeof(buf)) != 0)
        { printf("FAIL: lectura vacia devolvio datos en el intento %d\n", i); return 1; }

    radio_ring_free(&g_ring);
    printf("read_nonblock: vacio -> 0, drenado completo, 2000 vacias sin bloquear\n");
    return 0;
}

int main(void)
{
    int bad = 0;
    bad += test_backpressure();
    bad += test_close_unblocks();
    bad += test_read_nonblock();
    printf("%s\n", bad ? "RING FAIL" : "RING OK");
    return bad ? 1 : 0;
}
