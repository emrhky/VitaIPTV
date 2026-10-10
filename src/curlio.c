/* A small HTTP/1.1 client with its own TLS (mbedTLS, built into the app from third_party/mbedtls).
 * The Vita's own TLS fails on most sites today (0x80431075) and the SDK's libcurl does not link
 * with the SDK's OpenSSL, so https:// addresses come here: BSD sockets (newlib -> sceNet), poll()
 * for timeouts, redirects, chunked bodies, basic authentication. Certificates are not checked
 * (the Vita has no current root certificates); the connection is still encrypted. */
#include "curlio.h"
#include "netua.h"
#include "hls.h"                        /* hls_resolve() for redirects */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef HAVE_TLS
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "psa/crypto.h"

int getentropy(void *buf, size_t len);   /* newlib (Vita) and glibc; at most 256 bytes per call */

#include <time.h>
#include "mbedtls/platform_time.h"

/* milliseconds for TLS 1.3 session tickets (MBEDTLS_PLATFORM_MS_TIME_ALT) */
mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)time(NULL) * 1000; }

/* the entropy source mbedTLS asks for (MBEDTLS_ENTROPY_HARDWARE_ALT) */
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen)
{
    (void)data;
    size_t done = 0;
    while (done < len) {
        size_t n = len - done > 256 ? 256 : len - done;
        if (getentropy(out + done, n) != 0) break;
        done += n;
    }
    *olen = done;
    return done ? 0 : -1;
}

typedef struct {
    int fd;
    int tls;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    const CioRequest *rq;
    int idle_ms;                        /* time without data so far */
} Conn;

#define TICK_MS 500

static int stopped(const CioRequest *rq) { return rq->stop && rq->stop(rq->ctx); }

/* ---- sockets ---- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    Conn *c = ctx;
    ssize_t n = send(c->fd, buf, len, 0);
    if (n < 0) return (errno == EAGAIN || errno == EINTR) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)n;
}

/* waits up to one tick; MBEDTLS_ERR_SSL_TIMEOUT lets the caller check stop() and the idle time */
static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout)
{
    Conn *c = ctx;
    struct pollfd p = { c->fd, POLLIN, 0 };
    int r = poll(&p, 1, timeout ? (int)timeout : TICK_MS);
    if (r == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
    if (r < 0) return errno == EINTR ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    ssize_t n = recv(c->fd, buf, len, 0);
    if (n < 0) return (errno == EAGAIN || errno == EINTR) ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)n;                      /* 0 = closed */
}

static int tcp_connect(const char *host, int port, char *err, size_t errsz)
{
    struct addrinfo hints, *res = NULL;
    char ps[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { snprintf(err, errsz, "Could not resolve %s", host); return -1001; }
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        /* A big receive buffer, set before connecting so TCP can offer a big window: with the small default
         * the speed stops near buffer / round trip (films over https stayed at about 300 KB/s while the
         * Vita's own HTTP reached 800 KB/s). */
        int rcv = 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { snprintf(err, errsz, "Could not connect to the server"); return -1002; }
    return fd;
}

/* ---- one connection (plain or TLS) ---- */

static void conn_close(Conn *c)
{
    if (c->tls) {
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ctr_drbg_free(&c->drbg);
        mbedtls_entropy_free(&c->ent);
        c->tls = 0;
    }
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

static int conn_open(Conn *c, const char *host, int port, int tls, char *err, size_t errsz)
{
    c->fd = tcp_connect(host, port, err, errsz);
    if (c->fd < 0) return c->fd;
    if (!tls) return 0;
    c->tls = 1;
    mbedtls_entropy_init(&c->ent);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ssl_init(&c->ssl);
    int r = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->ent, (const unsigned char *)"VitaIPTV", 8);
    if (!r) r = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (!r) {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
        mbedtls_ssl_conf_read_timeout(&c->conf, TICK_MS);
        r = mbedtls_ssl_setup(&c->ssl, &c->conf);
    }
    if (!r) r = mbedtls_ssl_set_hostname(&c->ssl, host);     /* SNI: CDNs need it */
    if (r) { snprintf(err, errsz, "TLS setup failed (-0x%04X)", (unsigned)-r); return -1003; }
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, NULL, bio_recv_timeout);
    int waited = 0;
    while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_TIMEOUT) {
            if (stopped(c->rq)) return -2;
            if ((waited += TICK_MS) > 15000) { snprintf(err, errsz, "TLS handshake timed out"); return -1004; }
            continue;
        }
        char e[80];
        mbedtls_strerror(r, e, sizeof e);
        snprintf(err, errsz, "TLS handshake failed (-0x%04X %s)", (unsigned)-r, e);
        return -1005;
    }
    return 0;
}

static int conn_write(Conn *c, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = c->tls ? mbedtls_ssl_write(&c->ssl, (const unsigned char *)buf + off, len - off)
                       : (int)send(c->fd, buf + off, len - off, 0);
        if (c->tls && (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)) continue;
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* > 0 bytes, 0 = closed, -2 = stopped, < 0 error; waits in ticks so stop() is honoured */
static int conn_read(Conn *c, uint8_t *buf, size_t len)
{
    for (;;) {
        if (stopped(c->rq)) return -2;
        int n;
        if (c->tls) {
            n = mbedtls_ssl_read(&c->ssl, buf, len);
            if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
            if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;   /* TLS 1.3 */
#endif
        } else {
            n = bio_recv_timeout(c, buf, len, TICK_MS);
        }
        if (n == MBEDTLS_ERR_SSL_TIMEOUT) {
            c->idle_ms += TICK_MS;
            int limit = (c->rq->idle_timeout_s ? c->rq->idle_timeout_s : 15) * 1000;
            if (c->idle_ms >= limit) return -1006;
            continue;
        }
        if (n > 0) c->idle_ms = 0;
        return n;
    }
}

/* ---- HTTP ---- */

static int parse_url(const char *url, int *tls, char *host, size_t hcap, int *port, const char **path)
{
    const char *p;
    if (!strncasecmp(url, "https://", 8)) { *tls = 1; *port = 443; p = url + 8; }
    else if (!strncasecmp(url, "http://", 7)) { *tls = 0; *port = 80; p = url + 7; }
    else return -1;
    const char *end = p + strcspn(p, "/?#");
    const char *at = memchr(p, '@', (size_t)(end - p));
    if (at) p = at + 1;                                     /* user:pass@ is not sent this way */
    const char *colon = memchr(p, ':', (size_t)(end - p));
    size_t hl = (size_t)((colon ? colon : end) - p);
    if (!hl || hl >= hcap) return -1;
    memcpy(host, p, hl);
    host[hl] = 0;
    if (colon) *port = atoi(colon + 1);
    *path = *end ? end : "/";
    return 0;
}

static void b64(const char *in, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in), o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < n) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned char)in[i + 2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
}

static const char *header(const char *hdrs, const char *name, char *out, size_t cap)
{
    size_t n = strlen(name);
    for (const char *l = strstr(hdrs, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        const char *s = l + 2;
        if (!strncasecmp(s, name, n) && s[n] == ':') {
            s += n + 1;
            while (*s == ' ' || *s == '\t') s++;
            size_t k = strcspn(s, "\r\n");
            if (k >= cap) k = cap - 1;
            memcpy(out, s, k);
            out[k] = 0;
            return out;
        }
    }
    return NULL;
}

/* body reader: content-length, chunked or until close */
typedef struct {
    Conn *c;
    uint8_t *pend;                      /* bytes read with the headers, not used yet */
    size_t npend;
    int chunked;
    long long left;                     /* content-length left, or bytes left in this chunk; -1 = unknown */
    int done;
} Body;

static int raw_read(Body *b, uint8_t *buf, size_t len)
{
    if (b->npend) {
        size_t n = b->npend < len ? b->npend : len;
        memcpy(buf, b->pend, n);
        b->pend += n;
        b->npend -= n;
        return (int)n;
    }
    return conn_read(b->c, buf, len);
}

static int raw_byte(Body *b)
{
    uint8_t ch;
    int r = raw_read(b, &ch, 1);
    return r == 1 ? ch : (r == 0 ? -1000 : r);
}

/* next piece of the body: > 0 bytes, 0 = end, < 0 error */
static int body_read(Body *b, uint8_t *buf, size_t len)
{
    if (b->done) return 0;
    if (b->chunked) {
        if (b->left <= 0) {                                 /* chunk size line */
            char line[32];
            size_t k = 0;
            int ch;
            if (b->left == 0) {                             /* CRLF after the previous chunk */
                if ((ch = raw_byte(b)) < 0) return ch == -1000 ? 0 : ch;
                if (ch == '\r' && (ch = raw_byte(b)) < 0) return ch == -1000 ? 0 : ch;
            }
            for (;;) {
                if ((ch = raw_byte(b)) < 0) return ch == -1000 ? 0 : ch;
                if (ch == '\n') break;
                if (k + 1 < sizeof line) line[k++] = (char)ch;
            }
            line[k] = 0;
            b->left = strtoll(line, NULL, 16);
            if (b->left <= 0) { b->done = 1; return 0; }
        }
        size_t want = (size_t)b->left < len ? (size_t)b->left : len;
        int n = raw_read(b, buf, want);
        if (n > 0) b->left -= n;                            /* 0: the chunk's CRLF comes next */
        return n;
    }
    if (b->left == 0) { b->done = 1; return 0; }
    size_t want = b->left > 0 && (size_t)b->left < len ? (size_t)b->left : len;
    int n = raw_read(b, buf, want);
    if (n > 0 && b->left > 0) b->left -= n;
    if (n == 0) b->done = 1;
    return n;
}

int cio_available(void) { return 1; }

void cio_init(void)
{
    psa_crypto_init();                  /* TLS 1.3 uses PSA */
}

int cio_get(const CioRequest *rq, int *status, char *err, size_t errsz)
{
    char url[HLS_URL_MAX], host[256], req[2048], auth[400], enc[560], val[HLS_URL_MAX];
    const char *path;
    int tls, port, ret = -1;
    char e2[1];
    if (!err) { err = e2; errsz = sizeof e2; }
    err[0] = 0;
    *status = 0;
    snprintf(url, sizeof url, "%s", rq->url);
    uint8_t *hb = malloc(16384 + 1), *buf = malloc(16384);
    if (!hb || !buf) { free(hb); free(buf); snprintf(err, errsz, "Out of memory"); return -1; }
    for (int redirect = 0; redirect < 8; redirect++) {
        if (parse_url(url, &tls, host, sizeof host, &port, &path) < 0) { snprintf(err, errsz, "Bad address"); ret = -1; break; }
        Conn c;
        memset(&c, 0, sizeof c);
        c.fd = -1;
        c.rq = rq;
        int r = conn_open(&c, host, port, tls, err, errsz);
        if (r < 0) { conn_close(&c); ret = r; break; }
        int def = (tls && port == 443) || (!tls && port == 80);
        int k = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s", path, host);
        if (!def) k += snprintf(req + k, sizeof req - (size_t)k, ":%d", port);
        k += snprintf(req + k, sizeof req - (size_t)k, "\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: close\r\n", NET_UA);
        if (rq->user && rq->user[0] && redirect == 0) {
            snprintf(auth, sizeof auth, "%s:%s", rq->user, rq->pass ? rq->pass : "");
            b64(auth, enc);
            k += snprintf(req + k, sizeof req - (size_t)k, "Authorization: Basic %s\r\n", enc);
        }
        if (rq->range) {
            if (rq->range_to) k += snprintf(req + k, sizeof req - (size_t)k, "Range: bytes=%llu-%llu\r\n",
                                            (unsigned long long)rq->range_from, (unsigned long long)rq->range_to);
            else k += snprintf(req + k, sizeof req - (size_t)k, "Range: bytes=%llu-\r\n", (unsigned long long)rq->range_from);
        }
        k += snprintf(req + k, sizeof req - (size_t)k, "\r\n");
        if (conn_write(&c, req, (size_t)k) < 0) { conn_close(&c); snprintf(err, errsz, "Could not send the request"); ret = -1007; break; }
        /* headers */
        size_t hl = 0;
        char *eoh = NULL;
        while (!eoh && hl < 16384) {
            int n = conn_read(&c, hb + hl, 16384 - hl);
            if (n <= 0) { r = n; break; }
            hl += (size_t)n;
            hb[hl] = 0;
            eoh = strstr((char *)hb, "\r\n\r\n");
        }
        if (!eoh) {
            conn_close(&c);
            if (r == -2) { ret = -2; break; }
            snprintf(err, errsz, r == -1006 ? "The server did not answer" : "Bad answer from the server");
            ret = r == -1006 ? -1006 : -1008;
            break;
        }
        *eoh = 0;
        int code = 0;
        if (sscanf((char *)hb, "HTTP/%*d.%*d %d", &code) != 1) { conn_close(&c); snprintf(err, errsz, "Bad answer from the server"); ret = -1008; break; }
        *status = code;
        if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) && header((char *)hb, "Location", val, sizeof val)) {
            char next[HLS_URL_MAX];
            hls_resolve(url, val, -1, next, sizeof next);
            snprintf(url, sizeof url, "%s", next);
            conn_close(&c);
            continue;
        }
        if (code >= 400) { conn_close(&c); ret = 0; break; }   /* the caller looks at the status */
        Body b;
        memset(&b, 0, sizeof b);
        b.c = &c;
        b.pend = (uint8_t *)eoh + 4;
        b.npend = hl - (size_t)(b.pend - hb);
        b.left = -1;
        if (header((char *)hb, "Transfer-Encoding", val, sizeof val) && strstr(val, "chunked")) { b.chunked = 1; b.left = -1; }
        else if (header((char *)hb, "Content-Length", val, sizeof val)) b.left = atoll(val);
        if (b.chunked) b.left = -1;
        if (rq->total) {
            uint64_t tot = 0;
            if (header((char *)hb, "Content-Range", val, sizeof val)) {
                const char *sl = strchr(val, '/');
                if (sl && sl[1] != '*') tot = strtoull(sl + 1, NULL, 10);
            } else if (code == 200 && b.left > 0) tot = (uint64_t)b.left;
            *rq->total = tot;
        }
        ret = 0;
        for (;;) {
            int n = body_read(&b, buf, 16384);
            if (n == 0) break;
            if (n < 0) {
                if (n == -2) ret = -2;
                else { ret = n; snprintf(err, errsz, n == -1006 ? "No data for too long" : "Connection lost"); }
                break;
            }
            if (rq->data && rq->data(rq->ctx, buf, (size_t)n)) break;
        }
        conn_close(&c);
        break;
    }
    if (ret == -1 && !err[0]) snprintf(err, errsz, "Too many redirects");
    free(hb);
    free(buf);
    return ret;
}

#else   /* built without TLS */

int cio_available(void) { return 0; }
void cio_init(void) {}
int cio_get(const CioRequest *rq, int *status, char *err, size_t errsz)
{
    (void)rq;
    *status = 0;
    if (err) snprintf(err, errsz, "HTTPS support is not built in");
    return -1;
}

#endif
