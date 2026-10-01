/* Internet Radio mod - hilo del reproductor local (ver local.h). */
#include "local.h"
#include "meddec.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LLOG(l, ...) do { if((l)->log) (l)->log((l)->log_ctx, __VA_ARGS__); } while(0)

struct RadioLocal {
    RadioSink      sink;     /* solo se usan su ring PCM y su backend de audio */
    RadioPlaylist* list;
    pthread_t      th;
    int            th_started;
    int            stop;
    int            tracks;   /* ficheros que si se pudieron abrir */
    char           dir[320];
    char           cur[128]; /* nombre del tema en curso, para el log */
    RadioLocalLog  log;
    void*          log_ctx;
    RadioLocalTrack on_track; /* aviso de cambio de pista (ver local.h) */
    void*          on_track_ctx;
    /* Skip pendiente: 0 nada, +1 siguiente, -1 anterior. Lo escribe el hilo del
       juego y lo lee el de audio; volatile basta porque un comando perdido solo
       cuesta un chunk de audio, y perderlo NO puede desincronizar el indice (el
       hilo es el unico que mueve i). */
    volatile int   cmd;
    volatile int   cur_index; /* tema en curso, para la UI */
};

/* Recorre la lista en bucle (al llegar al final vuelve al principio) hasta que
   le digan parar. El ritmo lo impone el ring PCM: el write de abajo se bloquea
   mientras el dispositivo va atras, y ahi se queda el hilo hasta que el
   consumidor alcanza el ritmo. Decodificar mas rapido de lo que se reproduce no
   sirve de nada, y por eso este bucle no lleva ningun sleep. */
static void* local_thread(void* arg)
{
    RadioLocal* l = (RadioLocal*)arg;
    int16_t* pcm = (int16_t*)malloc((size_t)RADIO_SINK_DEC_SAMPLES * sizeof(int16_t));
    int i = 0;
    int n;

    if(!pcm) { LLOG(l, "local: sin memoria para el buffer de PCM"); return NULL; }
    n = radio_playlist_count(l->list);
    if(n <= 0) { free(pcm); return NULL; }

    while(!l->stop)
    {
        const char* name = radio_playlist_name(l->list, i);
        char why[128];
        RadioMeddec* d;
        int skipped = 0;

        why[0] = 0;
        d = radio_meddec_open(radio_playlist_path(l->list, i), why, sizeof why);
        if(!d)
        {
            /* Fichero que no es audio, o que este movil no sabe decodificar.
               No es fatal: se salta y sigue con el siguiente. */
            LLOG(l, "local: %s no se pudo abrir (%s)", name ? name : "?", why);
        }
        else
        {
            l->tracks++;
            snprintf(l->cur, sizeof l->cur, "%s", name ? name : "?");
            l->cur_index = i;
            LLOG(l, "local: %s [%s]", l->cur, radio_meddec_mime(d));

            /* La UI se entera aqui, no con un poll: el juego no da hook de frame.
               Este callback hace JNI y escritura a disco, o sea que puede tardar
               ~100 ms, pero estamos en una frontera de pista con 1.49 s de
               cushion en el ring, asi que no se oye. */
            if(l->on_track)
                l->on_track(l->on_track_ctx, l->cur, radio_playlist_path(l->list, i));

            while(!l->stop)
            {
                RadioFormat fmt;
                size_t want;
                int got;

                /* Skip: se comprueba ENTRE chunks. radio_ring_write de abajo
                   espera como mucho a que se libere un chunk, asi que la
                   latencia maxima es ese vaciado, no un frame. */
                if(l->cmd)
                {
                    int c = l->cmd;
                    l->cmd = 0;
                    i = c > 0 ? (i + 1) % n : (i - 1 + n) % n;
                    skipped = 1;
                    break;
                }

                got = radio_meddec_read(d, pcm, RADIO_SINK_DEC_SAMPLES, &fmt);

                if(got < 0) { LLOG(l, "local: %s: %s", l->cur, radio_meddec_err(d)); break; }
                if(got == 0) break;                       /* fin de pista */

                /* El formato se toma de la SALIDA del decodificador, no del
                   fichero: la plataforma remuestrea (m4a 22050 -> 44100). Y si el
                   tema siguiente trae otro, ensure_dev reabre el dispositivo. */
                if(radio_sink_ensure_dev(&l->sink, &fmt) != 0)
                {
                    LLOG(l, "local: %s no se pudo abrir a %d Hz/%d ch",
                         l->cur, fmt.sample_rate, fmt.channels);
                    break;
                }

                want = (size_t)got * sizeof(int16_t);
                if(radio_ring_write(&l->sink.pcm, pcm, want) < want) break; /* cerrado */
                l->sink.pcm_in += (long long)(want / sizeof(int16_t));
            }
            radio_meddec_close(d);
            l->cur[0] = 0;
        }

        if(l->stop) break;
        if(skipped) continue;          /* el skip ya dejo i en su sitio */
        if(++i >= n) i = 0;   /* la carpeta es una playlist en bucle */
    }

    free(pcm);
    return NULL;
}

void radio_local_set_log(RadioLocal* l, RadioLocalLog fn, void* ctx)
{
    if(!l) return;
    l->log = fn;
    l->log_ctx = ctx;
}

void radio_local_set_track_cb(RadioLocal* l, RadioLocalTrack fn, void* ctx)
{
    if(!l) return;
    l->on_track = fn;
    l->on_track_ctx = ctx;
}

RadioLocal* radio_local_new(const char* dir)
{
    RadioLocal* l;

    if(!dir || !*dir) return NULL;
    l = (RadioLocal*)calloc(1, sizeof(*l));
    if(!l) return NULL;
    snprintf(l->dir, sizeof l->dir, "%s", dir);

    l->list = radio_playlist_new(dir);
    if(!l->list) { free(l); return NULL; }
    if(radio_playlist_count(l->list) == 0)
    {
        radio_playlist_free(l->list);
        free(l);
        return NULL;
    }

    /* mp3 = NULL: aqui no hay stream ni decoder MP3, el PCM ya viene decodificado.
       Asi el sink no reserva tampoco el estado de minimp3, que no se usa. */
    if(radio_sink_init(&l->sink, NULL) != 0)
    {
        radio_playlist_free(l->list);
        free(l);
        return NULL;
    }
    return l;
}

void radio_local_free(RadioLocal* l)
{
    if(!l) return;
    radio_local_stop(l);
    radio_sink_free(&l->sink);
    radio_playlist_free(l->list);
    free(l);
}

int radio_local_start(RadioLocal* l)
{
    RadioPlaylist* fresh;

    if(!l) return -1;
    if(l->th_started) return 0;

    /* Relectura de la carpeta: el usuario puede haber copiado canciones desde la
       ultima vez, y recargar la lista es mucho mas barato que reiniciar el juego. */
    fresh = radio_playlist_new(l->dir);
    if(fresh && radio_playlist_count(fresh) > 0)
    {
        radio_playlist_free(l->list);
        l->list = fresh;
    }
    else if(fresh) radio_playlist_free(fresh);

    if(radio_playlist_count(l->list) == 0)
    {
        LLOG(l, "local: %s ya no tiene ficheros", l->dir);
        return -1;
    }

    l->stop = 0;
    l->tracks = 0;
    l->cur[0] = 0;
    l->cmd = 0;
    l->cur_index = -1;

    /* Ring PCM nuevo: el del arranque anterior puede llevar audio del formato
       viejo, que en un dispositivo recien abierto sonaria a otra velocidad. */
    radio_ring_free(&l->sink.pcm);
    if(radio_ring_init(&l->sink.pcm, RADIO_SINK_PCM_BYTES) != 0) return -1;
    l->sink.stop = 0;
    l->sink.dev = 0;
    l->sink.dev_error = 0;
    l->sink.dev_state = NULL;
    l->sink.have_fmt = 0;
    l->sink.frames_per_buf = 0;
    l->sink.pcm_in = l->sink.pcm_out = l->sink.underrun = 0;
    l->sink.underruns = l->sink.fmt_changes = 0;

    if(pthread_create(&l->th, NULL, local_thread, l) != 0) return -1;
    l->th_started = 1;
    return 0;
}

void radio_local_stop(RadioLocal* l)
{
    if(!l) return;
    l->stop = 1;
    if(l->th_started)
    {
        /* Cerrar el ring PCM es lo que despierta al hilo si esta bloqueado
           escribiendo, que es el unico punto donde puede quedarse esperando. */
        radio_ring_close(&l->sink.pcm);
        pthread_join(l->th, NULL);
        l->th_started = 0;
    }
    /* radio_sink_stop no hace join porque este sink no arranco su propio hilo
       (mp3 == NULL), pero si cierra el dispositivo de audio. */
    radio_sink_stop(&l->sink);
}

void radio_local_skip(RadioLocal* l, int dir)
{
    if(!l || dir == 0) return;
    /* No hace falta despertar al hilo: comprueba el comando entre chunks. Con
       dir a 0 el comando anterior sigue pendiente y se aplicaria dos veces, asi
       que un signo gana al otro (el ultimo que se pulse manda). */
    l->cmd = dir > 0 ? 1 : -1;
}

int         radio_local_tracks(const RadioLocal* l)      { return l ? l->tracks : 0; }
int         radio_local_underruns(const RadioLocal* l)   { return l ? l->sink.underruns : 0; }
long long   radio_local_samples(const RadioLocal* l)     { return l ? l->sink.pcm_in : 0; }
int         radio_local_fmt_changes(const RadioLocal* l) { return l ? l->sink.fmt_changes : 0; }
const char* radio_local_name(const RadioLocal* l)        { return l ? l->cur : ""; }
int         radio_local_index(const RadioLocal* l)       { return l ? l->cur_index : -1; }
int         radio_local_count(const RadioLocal* l)       { return (l && l->list) ? radio_playlist_count(l->list) : 0; }
