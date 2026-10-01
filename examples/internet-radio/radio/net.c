/* Internet Radio mod - HTTP streaming client (see net.h).
   Slice B1: HTTP/1.0 GET over plain TCP; the body goes to a RadioRing.
   HTTP/1.0 keeps the parsing trivial: no chunked transfer-encoding (the
   server closes the connection at EOF), which is also how icecast/shoutcast
   servers stream. */
#include "net.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */
#include <time.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

extern const char radio_ca_pem[]; /* ca_bundle.c, generated from cacert.pem */

#define NET_RECV_BUF        8192
#define NET_HDR_MAX         16384
#define NET_MAX_REDIRECTS   3
#define NET_RECV_TIMEOUT_S  1     /* so stop is noticed while idle */
#define NET_TLS_PERSISTENCE "gta-sa-internet-radio"

typedef struct {
    char host[256];
    int  port;
    int  tls;
    char path[768];
} Url;

typedef struct {
    int                     fd;
    int                     active;
    mbedtls_ssl_context     ssl;
    mbedtls_ssl_config      conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt        ca;
} Tls;

/* Transport in use: plain TCP, or the same socket wrapped in TLS. */
typedef struct {
    int  fd;
    Tls* tls;   /* NULL = no TLS */
} Conn;

static void net_set_error(RadioNet* n, const char* msg)
{
    /* Con stop puesto, el "error" suele ser nuestro propio shutdown: no
       queremos ensuciar last_error con un fallo falso al parar. */
    if(n->stop) return;
    snprintf(n->last_error, sizeof(n->last_error), "%s", msg);
}

/* http://host[:port]/path or https://host[:port]/path -> fields. */
static int url_parse(const char* url, Url* u)
{
    const char* p;
    const char* host_end;
    const char* colon;
    size_t host_len;

    memset(u, 0, sizeof(*u));
    if(strncmp(url, "http://", 7) == 0)
    {
        u->port = 80;
        p = url + 7;
    }
    else if(strncmp(url, "https://", 8) == 0)
    {
        u->tls = 1;
        u->port = 443;
        p = url + 8;
    }
    else return -1;

    host_end = strchr(p, '/');
    if(!host_end) host_end = p + strlen(p);

    colon = (const char*)memchr(p, ':', (size_t)(host_end - p));
    host_len = colon ? (size_t)(colon - p) : (size_t)(host_end - p);
    if(host_len == 0 || host_len >= sizeof(u->host)) return -1;

    memcpy(u->host, p, host_len);
    u->host[host_len] = '\0';
    if(colon)
    {
        u->port = atoi(colon + 1);
        if(u->port <= 0 || u->port > 65535) return -1;
    }

    snprintf(u->path, sizeof(u->path), "%s", *host_end ? host_end : "/");
    return 0;
}

static int tcp_connect(const Url* u)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[8];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", u->port);

    if(getaddrinfo(u->host, portstr, &hints, &res) != 0) return -1;

    for(ai = res; ai; ai = ai->ai_next)
    {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if(fd < 0) continue;
        if(connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if(fd >= 0)
    {
        struct timeval tv;
        tv.tv_sec = NET_RECV_TIMEOUT_S;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    return fd;
}

/* BIO: SO_RCVTIMEO expiry arrives as EAGAIN, so an idle socket becomes
   WANT_READ and TLS reports "nothing yet" exactly like the plain path. */
static int tls_bio_send(void* ctx, const unsigned char* buf, size_t len)
{
    int fd = *(const int*)ctx;
    ssize_t w;

    do { w = send(fd, buf, len, 0); } while(w < 0 && errno == EINTR);
    if(w > 0) return (int)w;
    if(w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int tls_bio_recv(void* ctx, unsigned char* buf, size_t len)
{
    int fd = *(const int*)ctx;
    ssize_t r;

    do { r = recv(fd, buf, len, 0); } while(r < 0 && errno == EINTR);
    if(r > 0) return (int)r;
    if(r == 0) return 0;
    if(errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

static void tls_error(RadioNet* n, const char* what, int ret)
{
    char msg[96];

    snprintf(msg, sizeof(msg), "%s failed (-0x%04x)", what, (unsigned)(-(int)ret));
    net_set_error(n, msg);
}

static void tls_end(Tls* t)
{
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    mbedtls_x509_crt_free(&t->ca);
    memset(t, 0, sizeof(*t));
}

static int tls_begin(RadioNet* n, Tls* t, const char* host, int fd)
{
    int ret;

    memset(t, 0, sizeof(*t));
    t->fd = fd;

    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_x509_crt_init(&t->ca);

    ret = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy,
                                (const unsigned char*)NET_TLS_PERSISTENCE,
                                sizeof(NET_TLS_PERSISTENCE) - 1);
    if(ret != 0) { tls_error(n, "tls rng", ret); tls_end(t); return -1; }

    ret = mbedtls_x509_crt_parse(&t->ca, (const unsigned char*)radio_ca_pem,
                                 strlen(radio_ca_pem) + 1);
    if(ret != 0) { tls_error(n, "tls ca", ret); tls_end(t); return -1; }

    ret = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if(ret != 0) { tls_error(n, "tls config", ret); tls_end(t); return -1; }

    mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);

    ret = mbedtls_ssl_setup(&t->ssl, &t->conf);
    if(ret != 0) { tls_error(n, "tls setup", ret); tls_end(t); return -1; }

    ret = mbedtls_ssl_set_hostname(&t->ssl, host); /* SNI + hostname check */
    if(ret != 0) { tls_error(n, "tls hostname", ret); tls_end(t); return -1; }

    mbedtls_ssl_set_bio(&t->ssl, &t->fd, tls_bio_send, tls_bio_recv, NULL);

    for(;;)
    {
        ret = mbedtls_ssl_handshake(&t->ssl);
        if(ret == 0) { t->active = 1; return 0; }
        if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            tls_error(n, "tls handshake", ret);
            tls_end(t);
            return -1;
        }
        if(n->stop) { tls_end(t); return -1; }
        usleep(20000);
    }
}

/* recv: >0 bytes, 0 = end of stream, NET_IDLE = nothing yet, -1 = error. */
#define NET_IDLE (-2)

static ssize_t conn_recv(Conn* c, void* buf, size_t len)
{
    if(c->tls)
    {
        int r = mbedtls_ssl_read(&c->tls->ssl, (unsigned char*)buf, len);
        if(r > 0) return r;
        if(r == 0) return 0;
        if(r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return NET_IDLE;
        if(r == MBEDTLS_ERR_SSL_CONN_EOF || r == MBEDTLS_ERR_NET_RECV_FAILED) return 0; /* peer dropped */
        return -1;
    }
    {
        ssize_t r = recv(c->fd, buf, len, 0);
        if(r > 0) return r;
        if(r == 0) return 0;
        if(errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return NET_IDLE;
        return -1;
    }
}

static void conn_close(RadioNet* n, Conn* c)
{
    n->fd = -1; /* deja de ser "el socket vivo": stop() ya no lo shutdowna */
    if(c->tls) tls_end(c->tls);
    if(c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->tls = NULL;
}

static int conn_send(Conn* c, const char* buf, size_t len)
{
    size_t sent = 0;

    while(sent < len)
    {
        if(c->tls)
        {
            int r = mbedtls_ssl_write(&c->tls->ssl, (const unsigned char*)buf + sent, len - sent);
            if(r > 0) { sent += (size_t)r; continue; }
            if(r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            return -1;
        }
        else
        {
            ssize_t w = send(c->fd, buf + sent, len - sent, 0);
            if(w > 0) { sent += (size_t)w; continue; }
            if(w < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            return -1;
        }
    }
    return 0;
}

static int http_send_request(Conn* c, const Url* u)
{
    char req[1400];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\n"
                     "Host: %s:%d\r\n"
                     "User-Agent: gta-sa-internet-radio/0.1\r\n"
                     "Icy-MetaData: 0\r\n"   /* Slice E turns metadata on */
                     "Accept: */*\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     u->path, u->host, u->port);
    if(n <= 0 || (size_t)n >= sizeof(req)) return -1;
    return conn_send(c, req, (size_t)n);
}

static const char* header_value(const char* hdr, const char* name, size_t name_len)
{
    const char* line = hdr;

    while(*line)
    {
        const char* eol = strstr(line, "\r\n");
        size_t len = eol ? (size_t)(eol - line) : strlen(line);

        if(len > name_len && strncasecmp(line, name, name_len) == 0 && line[name_len] == ':')
        {
            const char* v = line + name_len + 1;
            while(*v == ' ' || *v == '\t') v++;
            return v;
        }
        if(!eol) break;
        line = eol + 2;
    }
    return NULL;
}

/* Reads until the header terminator. Returns the body offset inside buf
   (>= 0) and sets *total to the bytes read, or -1 on error/garbage. */
static long http_read_headers(Conn* c, char* buf, size_t cap, size_t* total,
                              int* status, char* ctype, size_t ctsz,
                              char* loc, size_t locsz)
{
    size_t used = 0;
    long body = -1;

    *status = 0;
    *total = 0;
    ctype[0] = '\0';
    loc[0] = '\0';

    for(;;)
    {
        ssize_t r;
        char* end;

        if(used >= cap - 1) return -1;
        r = conn_recv(c, buf + used, cap - 1 - used);
        if(r == 0) return -1; /* EOF before headers completed */
        if(r < 0)
        {
            if(r == NET_IDLE) continue;
            return -1;
        }
        used += (size_t)r;
        buf[used] = '\0';

        end = strstr(buf, "\r\n\r\n");
        if(end)
        {
            body = (long)((size_t)(end - buf) + 4);
            break;
        }
    }

    /* status line: HTTP/1.x NNN reason */
    if(strncmp(buf, "HTTP/", 5) != 0) return -1;
    {
        const char* sp = strchr(buf, ' ');
        if(!sp) return -1;
        *status = atoi(sp + 1);
    }

    {
        const char* v = header_value(buf, "content-type", 12);
        if(v) snprintf(ctype, ctsz, "%.*s", (int)(strcspn(v, "\r\n")), v);
        v = header_value(buf, "location", 8);
        if(v) snprintf(loc, locsz, "%.*s", (int)(strcspn(v, "\r\n")), v);
    }

    *total = used;
    return body;
}

/* One connect+stream attempt. Pushes body bytes into the ring. */
static void net_stream_once(RadioNet* n, const Url* start)
{
    Url u = *start;
    int redirects = 0;

    while(!n->stop)
    {
        int status = 0;
        long body;
        size_t total = 0;
        char ctype[128], loc[1024];
        char* hbuf = (char*)malloc(NET_HDR_MAX);
        unsigned char* rbuf = NULL;
        Tls tls;
        Conn c;

        memset(&tls, 0, sizeof(tls));
        memset(&c, 0, sizeof(c));
        c.fd = -1;
        c.tls = u.tls ? &tls : NULL;

        if(!hbuf) { net_set_error(n, "OOM"); return; }

        c.fd = tcp_connect(&u);
        if(c.fd < 0)
        {
            net_set_error(n, "connect failed");
            free(hbuf);
            return;
        }
        n->fd = c.fd; /* publicado: stop() puede despertarlo con shutdown() */
        if(c.tls && tls_begin(n, c.tls, u.host, c.fd) != 0)
        {
            conn_close(n, &c);
            free(hbuf);
            return;
        }
        if(http_send_request(&c, &u) != 0)
        {
            net_set_error(n, "send failed");
            conn_close(n, &c);
            free(hbuf);
            return;
        }

        body = http_read_headers(&c, hbuf, NET_HDR_MAX, &total, &status,
                                 ctype, sizeof(ctype), loc, sizeof(loc));
        n->status = status;
        if(body < 0)
        {
            net_set_error(n, "bad response headers");
            conn_close(n, &c);
            free(hbuf);
            return;
        }

        if(status >= 300 && status < 400 && loc[0] && redirects < NET_MAX_REDIRECTS)
        {
            Url nu;
            if(url_parse(loc, &nu) == 0)
            {
                u = nu;
                redirects++;
                conn_close(n, &c);
                free(hbuf);
                continue;
            }
        }

        if(status != 200)
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "HTTP %d", status);
            net_set_error(n, msg);
            conn_close(n, &c);
            free(hbuf);
            return;
        }

        /* bytes already read past the headers are the first body bytes */
        if(total > (size_t)body)
        {
            size_t pre = total - (size_t)body;
            size_t w = radio_ring_write(n->ring, hbuf + body, pre);
            n->bytes += (long long)w;
            if(w < pre) { free(hbuf); conn_close(n, &c); return; } /* ring closed */
        }

        rbuf = (unsigned char*)malloc(NET_RECV_BUF);
        if(!rbuf) { net_set_error(n, "OOM"); free(hbuf); conn_close(n, &c); return; }

        while(!n->stop)
        {
            ssize_t r = conn_recv(&c, rbuf, NET_RECV_BUF);

            if(r > 0)
            {
                size_t w = radio_ring_write(n->ring, rbuf, (size_t)r);
                n->bytes += (long long)w;
                if(w < (size_t)r) break; /* ring closed */
                continue;
            }
            if(r == 0) break; /* server closed: end of stream */
            if(r == NET_IDLE) continue; /* socket idle: lets stop through */
            net_set_error(n, "recv failed");
            break;
        }

        free(rbuf);
        free(hbuf);
        conn_close(n, &c);
        return;
    }
}

static void* net_thread(void* arg)
{
    RadioNet* n = (RadioNet*)arg;
    Url u;

    if(url_parse(n->url, &u) != 0)
    {
        net_set_error(n, "unsupported URL (need http:// or https://)");
        radio_ring_close(n->ring);
        return NULL;
    }

    while(!n->stop)
    {
        net_stream_once(n, &u);
        if(n->stop || !n->reconnect) break;
        /* ponytail: backoff de 1 s troceado en naps de 20 ms, para que un OFF
           durante la reconexion no espere a la nap en curso (100 ms congelaba
           el juego ~3 frames; con 20 ms el tope practico es un frame). */
        {
            int i;
            for(i = 0; i < 50 && !n->stop; i++)
            {
                struct timespec ts;
                ts.tv_sec = 0; ts.tv_nsec = 20L * 1000L * 1000L;
                nanosleep(&ts, NULL);
            }
        }
    }

    radio_ring_close(n->ring);
    return NULL;
}

int radio_net_start_ex(RadioNet* n, RadioRing* ring, const char* url, int reconnect)
{
    if(!n || !ring || !url) return -1;

    memset(n, 0, sizeof(*n));
    n->ring = ring;
    n->fd = -1; /* memset deja 0 (= stdin): nunca shutdown(0) */
    n->reconnect = reconnect ? 1 : 0;
    snprintf(n->url, sizeof(n->url), "%s", url);

    if(pthread_create(&n->thread, NULL, net_thread, n) != 0)
    {
        net_set_error(n, "pthread_create failed");
        return -1;
    }
    return 0;
}

int radio_net_start(RadioNet* n, RadioRing* ring, const char* url)
{
    return radio_net_start_ex(n, ring, url, 1);
}

void radio_net_stop(RadioNet* n)
{
    if(!n || !n->ring) return;
    n->stop = 1;
    /* Despertar el recv bloqueado de golpe: sin esto el join espera al
       SO_RCVTIMEO de 1 s y el OFF congela el juego un segundo. */
    if(n->fd >= 0) shutdown(n->fd, SHUT_RDWR);
    radio_ring_close(n->ring); /* unblock writes/reads so join cannot hang */
    /* ponytail: el join espera a la llamada bloqueante que este en vuelo
       (getaddrinfo/connect/handshake), que no tiene timeout propio. Medido:
       21 ms en el caso normal, pero si el DNS del dispositivo se queda colgado
       el OFF se queda esperando con el. Si alguna vez se ve, meter el resolve
       en un hilo aparte o cachear la IP del host. */
    pthread_join(n->thread, NULL);
}

const char* radio_net_last_error(const RadioNet* n)
{
    return n ? n->last_error : "";
}
