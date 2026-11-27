/* A small HTTP/1.1 client with its own TLS (mbedTLS, built into the app from third_party/mbedtls).
 * The Vita's own TLS fails on most sites today (0x80431075) and the SDK's libcurl does not link
 * with the SDK's OpenSSL, so https:// addresses come here: BSD sockets (newlib -> sceNet), poll()
 * for timeouts, redirects, chunked bodies, basic authentication. Certificates are not checked
 * (the Vita has no current root certificates); the connection is still encrypted. */
#include "curlio.h"
#include "netua.h"
#include "hls.h"                        /* hls_resolve() for redirects */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef HAVE_TLS
#include <errno.h>
#include <fcntl.h>
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

#define KEEP_IDLE_MS 8000               /* (servers close idle ones after 5-15 s; an older one is not tried) */
/* 1.2.4r: two servers (a film's address and the server it sends the player on to: each request goes through both) */
#define KEEP_SLOTS 2
struct CioKeep {
    struct { Conn *c; char host[256]; int port, tls; uint64_t last_ms; } s[KEEP_SLOTS];   /* (c NULL: none) */
    int reused;                         /* the last request went over a kept one */
};

#define TICK_MS 500

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
static uint64_t now_ms64(void) { return sceKernelGetProcessTimeWide() / 1000; }
static void sleep_us(unsigned us) { sceKernelDelayThread(us); }
#else
static void sleep_us(unsigned us) { usleep(us); }
static uint64_t now_ms64(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000; }
#endif

static volatile int g_rcvbuf;            /* the receive buffer the last connection got (bytes) */
static volatile int g_connect_ms;        /* how long the last connect took (about one round trip) */
static volatile int g_connect_tries;     /* 1.5.1: the sockets it took (more than one: the first stayed silent) */
static volatile int g_dns_stale;         /* 1.2.4r: connections made to an earlier address (the lookup failed) */
int cio_dns_stale(void) { return g_dns_stale; }
static char g_rcv_note[160];             /* 0.6.0: the receive buffer the system gave ("512 KB", or what was refused) */
static volatile int g_rcv_new;           /* ... not logged yet */
int cio_last_rcvbuf(void) { return g_rcvbuf; }
int cio_last_connect_ms(void) { return g_connect_ms; }
int cio_last_connect_tries(void) { return g_connect_tries; }
const char *cio_rcvbuf_note(void)
{
    if (!g_rcv_new) return "";
    g_rcv_new = 0;
    return g_rcv_note;
}

/* 0.6.0: a 512 KB receive buffer. The Vita refuses anything bigger with ENOBUFS (1 MB, and even 528 KB, on the
 * device) and then keeps its default of 63 KB without a word: a connection then manages at most 64 KB per round
 * trip (0.5.0: about 285 KB/s from a server 200 ms away; with 512 KB: 1.7 MB/s). Should 512 KB be refused (other
 * firmware, or the network's memory, which all connections share, short for a moment) 256 and 128 KB are tried;
 * what was given is noted for the log. */
static int rcv_try(int fd, int v, int *e)
{
    int r = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof v);
    *e = r < 0 ? errno : 0;
    return r == 0;
}

static void set_rcvbuf(int fd)
{
    static const int SIZES[] = { 512 * 1024, 256 * 1024, 128 * 1024 };
    int e = 0, k = 0, ok = 0;
    char note[120];                                          /* (room left in g_rcv_note for ", using N KB") */
    note[0] = 0;
    for (size_t i = 0; i < sizeof SIZES / sizeof SIZES[0] && !ok; i++) {
        ok = rcv_try(fd, SIZES[i], &e);
        if (!ok && k >= 0 && (size_t)k < sizeof note)
            k += snprintf(note + k, sizeof note - (size_t)k, "%s%d KB refused (%d)", k ? ", " : "", SIZES[i] / 1024, e);
    }
    int got = 0;
    socklen_t rl = sizeof got;
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &rl) == 0) g_rcvbuf = got;
    if (note[0] || !g_rcv_note[0]) {                         /* (said once; again only when something was refused) */
        snprintf(g_rcv_note, sizeof g_rcv_note, "%s%s%d KB", note, note[0] ? ", using " : "", got / 1024);
        g_rcv_new = 1;
    }
}

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

/* connect() with a time limit: a server that silently drops the connection would otherwise block for the
 * system's own timeout (tens of seconds) and O could not stop it. Non-blocking connect + poll in ticks so
 * stop() is honoured; the socket is made blocking again afterwards (the rest of the code expects that).
 * (1.5.1: connect_race, with more than one socket.) */

/* 1.5.1: a connect whose first packet is lost waits for the system to send it again (2 s, then 6 s more: the
 * 1.5.0 logs: "connect 2087 ms", "6113 ms", "10002 ms" to a server that otherwise answers in 60-90 ms). After
 * 1.5 s (and 4 s) one more socket tries too; the first that connects is used, the others are closed before
 * anything is sent on them. Returns the connected socket, -2 stopped, -1 failed (*why as connect_wait). */
#define RACE_N 3
/* 1.2.4q: the second socket after 5 round trips of the last plain connects (at least 0.5 s, at most 1.5 s as
 * before), the third 2.5 s after it. On the device a provider's server let the first socket stay silent on most
 * requests ("connect 1611 / 2110 / 3613 ms, 2nd socket") while a connect took 70-130 ms: 1 s or more each time. */
static volatile int g_rtt_ms;            /* (a running average of connects the first socket made; 0 none yet) */
static int connect_race(const struct addrinfo *a, const CioRequest *rq, int *why, int *tries)
{
    int s1 = g_rtt_ms ? g_rtt_ms * 5 : 1500;
    if (s1 < 500) s1 = 500;
    if (s1 > 1500) s1 = 1500;
    const int START_MS[RACE_N] = { 0, s1, s1 + 2500 };
    int fds[RACE_N], fls[RACE_N], n = 0, alive = 0, win = -1;
    int limit_ms = (rq && rq->connect_timeout_s > 0 ? rq->connect_timeout_s : 15) * 1000;
    uint64_t t0 = now_ms64();
    *why = 0;
    *tries = 0;
    for (;;) {
        int el = (int)(now_ms64() - t0);
        if (n < RACE_N && (el >= START_MS[n] || !alive)) {    /* (all of them failed at once: the next one now) */
            int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            fds[n] = fd;
            fls[n] = -1;
            n++;
            *tries = n;
            if (fd >= 0) {
                set_rcvbuf(fd);
                fls[n - 1] = fcntl(fd, F_GETFL, 0);
                if (fls[n - 1] < 0 || fcntl(fd, F_SETFL, fls[n - 1] | O_NONBLOCK) < 0) {   /* no non-blocking mode: as before */
                    fcntl(fd, F_SETFL, fls[n - 1] < 0 ? 0 : fls[n - 1]);
                    if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) { win = n - 1; break; }
                    *why = errno;
                    close(fd); fds[n - 1] = -1;
                } else if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) { win = n - 1; break; }
                else if (errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK) { *why = errno; close(fd); fds[n - 1] = -1; }
                else alive++;
            }
            continue;
        }
        if (rq && stopped(rq)) break;
        if (el >= limit_ms) { if (!*why || alive) *why = -1; break; }
        if (!alive && n >= RACE_N) break;
        struct pollfd p[RACE_N];
        int idx[RACE_N], np = 0;
        for (int k = 0; k < n; k++) if (fds[k] >= 0) { p[np].fd = fds[k]; p[np].events = POLLOUT; p[np].revents = 0; idx[np++] = k; }
        int wait = TICK_MS;
        if (n < RACE_N && START_MS[n] - el < wait) wait = START_MS[n] - el > 0 ? START_MS[n] - el : 0;
        if (limit_ms - el < wait) wait = limit_ms - el > 0 ? limit_ms - el : 0;
        int r = poll(p, (nfds_t)np, wait);
        if (r < 0) { if (errno == EINTR) continue; *why = 1000 + errno; break; }
        for (int q = 0; q < np && r > 0 && win < 0; q++) {
            if (!p[q].revents) continue;
            int k = idx[q], e = 0;
            socklen_t elen = sizeof e;
            if (getsockopt(fds[k], SOL_SOCKET, SO_ERROR, &e, &elen) < 0) e = (p[q].revents & (POLLERR | POLLHUP)) ? -2 : 0;
            if (!e) { win = k; break; }
            *why = e;
            close(fds[k]); fds[k] = -1; alive--;
        }
        if (win >= 0) break;
    }
    for (int k = 0; k < n; k++) if (k != win && fds[k] >= 0) close(fds[k]);
    if (win < 0) return rq && stopped(rq) ? -2 : -1;
    if (win == 0) {                                         /* (1.2.4q: the first one answered: its time) */
        int el = (int)(now_ms64() - t0);
        if (el < 1500) g_rtt_ms = g_rtt_ms ? (g_rtt_ms * 3 + el + 2) / 4 : (el > 0 ? el : 1);
    }
    if (fls[win] >= 0) fcntl(fds[win], F_SETFL, fls[win]);   /* (blocking again: the rest of the code expects that) */
    *why = 0;
    return fds[win];
}

/* ---- 1.2.4r: name lookups with a time limit, that a stop ends, and the addresses found kept ----
 * getaddrinfo cannot be interrupted: on the device an HLS reader sat in it while the Wi-Fi came and went, the
 * player did not stop in time, and its decoder stayed taken. It runs on a thread of its own now; the request
 * waits for it at most DNS_WAIT_MS (and stops at once when asked to); what it found is kept for DNS_FRESH_MS
 * (no lookup then) and, when a lookup fails or takes too long, up to DNS_STALE_MS (an earlier address). */
#define DNS_WAIT_MS 5000
#define DNS_FRESH_MS (10 * 60 * 1000)
#define DNS_STALE_MS (24 * 60 * 60 * 1000)
#define DNS_N 16
#define DNS_ADDRS 4
typedef struct { char host[128]; struct in_addr a[DNS_ADDRS]; int n; uint64_t at; } DnsEnt;
static DnsEnt g_dns[DNS_N];
static volatile int g_dns_lock;
static void dns_lock(void) { while (__sync_lock_test_and_set(&g_dns_lock, 1)) sleep_us(1000); }
static void dns_unlock(void) { __sync_lock_release(&g_dns_lock); }
/* 1 = found (fresh_only: not older than DNS_FRESH_MS) */
static int dns_cached(const char *host, int fresh_only, struct in_addr *a, int *n)
{
    int ok = 0;
    dns_lock();
    for (int k = 0; k < DNS_N; k++)
        if (g_dns[k].n && !strcmp(g_dns[k].host, host)) {
            uint64_t age = now_ms64() - g_dns[k].at;
            if (age < (fresh_only ? (uint64_t)DNS_FRESH_MS : (uint64_t)DNS_STALE_MS)) {
                *n = g_dns[k].n;
                memcpy(a, g_dns[k].a, sizeof g_dns[k].a);
                ok = 1;
            }
            break;
        }
    dns_unlock();
    return ok;
}
static void dns_keep(const char *host, const struct in_addr *a, int n)
{
    if (strlen(host) >= sizeof g_dns[0].host || n <= 0) return;
    dns_lock();
    int k = 0, old = 0;
    while (k < DNS_N && g_dns[k].n && strcmp(g_dns[k].host, host)) { if (g_dns[k].at < g_dns[old].at) old = k; k++; }
    if (k == DNS_N) k = old;
    snprintf(g_dns[k].host, sizeof g_dns[k].host, "%s", host);
    memcpy(g_dns[k].a, a, sizeof g_dns[k].a);
    g_dns[k].n = n;
    g_dns[k].at = now_ms64();
    dns_unlock();
}
typedef struct { char host[256]; struct in_addr a[DNS_ADDRS]; volatile int n, done; volatile int refs; } DnsJob;
static void dns_job_put(DnsJob *j) { if (__sync_sub_and_fetch(&j->refs, 1) == 0) free(j); }
static void dns_job_run(DnsJob *j)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int n = 0;
    if (getaddrinfo(j->host, NULL, &hints, &res) == 0 && res) {
        for (struct addrinfo *x = res; x && n < DNS_ADDRS; x = x->ai_next)
            if (x->ai_family == AF_INET && x->ai_addr) j->a[n++] = ((struct sockaddr_in *)x->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    if (n) dns_keep(j->host, j->a, n);                      /* (also when the request stopped waiting) */
    j->n = n;
    __sync_synchronize();
    j->done = 1;
    dns_job_put(j);
}
#ifdef __vita__
static int dns_thread(SceSize args, void *argp)
{
    (void)args;
    dns_job_run(*(DnsJob **)argp);
    sceKernelExitDeleteThread(0);
    return 0;
}
static int dns_spawn(DnsJob *j)
{
    SceUID th = sceKernelCreateThread("dns", dns_thread, 0x10000100, 64 * 1024, 0, 0, NULL);
    if (th < 0) return -1;
    if (sceKernelStartThread(th, sizeof j, &j) < 0) { sceKernelDeleteThread(th); return -1; }
    return 0;
}
#else
#include <pthread.h>
static void *dns_thread(void *p) { dns_job_run(p); return NULL; }
static int dns_spawn(DnsJob *j)
{
    pthread_t th;
    if (pthread_create(&th, NULL, dns_thread, j) != 0) return -1;
    pthread_detach(th);
    return 0;
}
#endif
/* the host's addresses: n of them in a; 0 found, -1001 none, -2 stopped. *stale: an earlier one was used */
static int dns_lookup(const char *host, const CioRequest *rq, struct in_addr *a, int *n, int *stale)
{
    *stale = 0;
    unsigned q[4];
    char tail;
    if (sscanf(host, "%u.%u.%u.%u%c", &q[0], &q[1], &q[2], &q[3], &tail) == 4 && q[0] < 256 && q[1] < 256 && q[2] < 256 && q[3] < 256) {
        a[0].s_addr = htonl((q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3]);   /* (an address already) */
        *n = 1;
        return 0;
    }
    if (dns_cached(host, 1, a, n)) return 0;
    DnsJob *j = calloc(1, sizeof *j);
    if (!j) return -1001;
    snprintf(j->host, sizeof j->host, "%s", host);
    j->refs = 2;
    if (dns_spawn(j) < 0) {                                  /* (no thread: as before, here) */
        j->refs = 1;
        dns_job_run(j);
        j = NULL;
    } else {
        uint64_t t0 = now_ms64();
        while (!j->done && now_ms64() - t0 < DNS_WAIT_MS && !(rq && stopped(rq))) sleep_us(10000);
    }
    int found = 0;
    if (j && j->done && j->n) { *n = j->n; memcpy(a, j->a, sizeof j->a); found = 1; }
    int was_stopped = j && !j->done && rq && stopped(rq);
    if (j) dns_job_put(j);                                   /* (a lookup still running frees itself) */
    if (!j && dns_cached(host, 1, a, n)) found = 1;          /* (ran here: it kept what it found) */
    if (found) return 0;
    if (was_stopped) return -2;
    if (dns_cached(host, 0, a, n)) { *stale = 1; return 0; }
    return -1001;
}

static int tcp_connect(const char *host, int port, const CioRequest *rq, char *err, size_t errsz)
{
    struct in_addr ip[DNS_ADDRS];
    int nip = 0, stale = 0;
    int lr = dns_lookup(host, rq, ip, &nip, &stale);
    if (lr == -2) { snprintf(err, errsz, "Stopped"); return -2; }
    if (lr < 0 || !nip) { snprintf(err, errsz, "Could not resolve %s", host); return -1001; }
    if (stale) g_dns_stale++;
    /* (the addresses as a list, as getaddrinfo gave them before) */
    struct sockaddr_in sa[DNS_ADDRS];
    struct addrinfo ai[DNS_ADDRS], *res = &ai[0];
    memset(sa, 0, sizeof sa);
    memset(ai, 0, sizeof ai);
    for (int k = 0; k < nip; k++) {
        sa[k].sin_family = AF_INET;
        sa[k].sin_port = htons((uint16_t)port);
        sa[k].sin_addr = ip[k];
        ai[k].ai_family = AF_INET;
        ai[k].ai_socktype = SOCK_STREAM;
        ai[k].ai_addr = (struct sockaddr *)&sa[k];
        ai[k].ai_addrlen = sizeof sa[k];
        ai[k].ai_next = k + 1 < nip ? &ai[k + 1] : NULL;
    }
    int fd = -1, st = 0, why = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        /* A big receive buffer, set before connecting so TCP can offer a big window: with the small default
         * the speed stops near buffer / round trip (films over https stayed at about 300 KB/s while the
         * Vita's own HTTP reached 800 KB/s). (set_rcvbuf in connect_race) */
        uint64_t t0 = now_ms64();
        int tries = 0;
        fd = connect_race(a, rq, &why, &tries);
        g_connect_ms = (int)(now_ms64() - t0);
        g_connect_tries = tries;
        if (fd >= 0) { st = 0; break; }
        st = fd;
        fd = -1;
        if (st == -2) break;
    }
    if (st == -2) { snprintf(err, errsz, "Stopped"); return -2; }
    if (fd < 0) {
        if (why == -1) snprintf(err, errsz, "Could not connect to the server (no answer)");
        else if (why) snprintf(err, errsz, "Could not connect to the server (%d)", why);
        else snprintf(err, errsz, "Could not connect to the server");
        return -1002;
    }
    return fd;
}

/* appends to the request, never past its end (a long link plus long headers just makes k reach the end) */
static void rq_add(char *b, size_t cap, int *k, const char *fmt, ...)
{
    if (*k < 0 || (size_t)*k >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b + *k, cap - (size_t)*k, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    *k = (size_t)*k + (size_t)n >= cap - 1 ? (int)cap - 1 : *k + n;
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

/* 1.2.4f: ChaCha20-Poly1305 first among the offered suites: the Vita's ARM has no AES instructions, and in
 * software ChaCha20 is about twice as fast as AES-GCM (an HTTPS 1080p stream took 80% of a core). Servers that
 * follow the client's order take it; the others keep choosing as before. */
static int g_suites[160];
static char g_last_suite[64];
static const int *suites(void)
{
    if (g_suites[0]) return g_suites;
    const int *all = mbedtls_ssl_list_ciphersuites();
    int n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (const int *p = all; *p && n < (int)(sizeof g_suites / sizeof g_suites[0]) - 1; p++) {
            const char *nm = mbedtls_ssl_get_ciphersuite_name(*p);
            int cc = nm && strstr(nm, "CHACHA20");
            if (cc == !pass) g_suites[n++] = *p;
        }
    g_suites[n] = 0;
    return g_suites;
}
const char *cio_last_suite(void) { return g_last_suite; }

static int conn_open(Conn *c, const char *host, int port, int tls, char *err, size_t errsz)
{
    c->fd = tcp_connect(host, port, c->rq, err, errsz);
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
        mbedtls_ssl_conf_ciphersuites(&c->conf, suites());
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
    snprintf(g_last_suite, sizeof g_last_suite, "%s", mbedtls_ssl_get_ciphersuite(&c->ssl) ? mbedtls_ssl_get_ciphersuite(&c->ssl) : "");
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

/* 1.2.4p: kept connections */
static int has_close(const char *v)                         /* "close" in a Connection header, any case */
{
    for (; *v; v++) if (!strncasecmp(v, "close", 5)) return 1;
    return 0;
}
static void conn_drop(Conn *c) { if (c) { conn_close(c); free(c); } }
CioKeep *cio_keep_new(void) { return calloc(1, sizeof(CioKeep)); }
void cio_keep_close(CioKeep *k) { if (k) for (int i = 0; i < KEEP_SLOTS; i++) { conn_drop(k->s[i].c); k->s[i].c = NULL; } }
void cio_keep_free(CioKeep *k) { if (k) { cio_keep_close(k); free(k); } }
int cio_keep_reused(const CioKeep *k) { return k ? k->reused : 0; }
/* the kept connection to this server (taken out), NULL none; an old one is closed */
static Conn *keep_take(CioKeep *k, const char *host, int port, int tls)
{
    for (int i = 0; i < KEEP_SLOTS; i++) {
        if (!k->s[i].c || k->s[i].port != port || k->s[i].tls != tls || strcmp(k->s[i].host, host)) continue;
        Conn *c = k->s[i].c;
        k->s[i].c = NULL;
        if (now_ms64() - k->s[i].last_ms < KEEP_IDLE_MS) return c;
        conn_drop(c);
    }
    return NULL;
}
static void keep_put(CioKeep *k, Conn *c, const char *host, int port, int tls)
{
    int i = 0, o = 0;
    for (; i < KEEP_SLOTS; i++) {
        if (!k->s[i].c) break;
        if (k->s[i].last_ms < k->s[o].last_ms) o = i;
    }
    if (i == KEEP_SLOTS) { i = o; conn_drop(k->s[i].c); }
    k->s[i].c = c;
    snprintf(k->s[i].host, sizeof k->s[i].host, "%s", host);
    k->s[i].port = port;
    k->s[i].tls = tls;
    k->s[i].last_ms = now_ms64();
}

/* ---- 1.2.4r: one connection per server, shared by the app's threads (CioRequest.shared) ----
 * A request to a server takes its connection; another request to the same server meanwhile waits for it (at most
 * POOL_WAIT_MS) rather than open a second one: on the device a provider answered 403 to the film details (a second
 * connection) while the lists' kept one stood open. Closed after POOL_IDLE_MS unused (cio_pool_idle; reading a
 * film's details takes a while: the next one finds it open), and all at once by cio_pool_close (a video starts).
 * A server may close it sooner: the request then notices (nothing comes) and makes a new one. */
#define POOL_N 6
#define POOL_IDLE_MS 30000
#define POOL_WAIT_MS 2500
typedef struct { char host[256]; int port, tls, busy, drop; Conn *c; uint64_t last_ms; } PoolEnt;
static PoolEnt g_pool[POOL_N];
static volatile int g_pool_lock;
static volatile int g_pool_keep = 1;     /* 0: shared, but nothing kept open after a request (a video plays) */
void cio_pool_keep(int on) { g_pool_keep = on ? 1 : 0; }
static void pool_lock(void) { while (__sync_lock_test_and_set(&g_pool_lock, 1)) sleep_us(1000); }
static void pool_unlock(void) { __sync_lock_release(&g_pool_lock); }
/* the server's entry, taken (busy): *c its open connection (NULL: make one). -1: none (busy too long: one of its own) */
static int pool_take(const char *host, int port, int tls, const CioRequest *rq, int no_reuse, Conn **c)
{
    *c = NULL;
    uint64_t t0 = now_ms64();
    for (;;) {
        pool_lock();
        int k = 0, free_k = -1, old_k = -1;
        for (; k < POOL_N; k++) {
            if (g_pool[k].host[0] && g_pool[k].port == port && g_pool[k].tls == tls && !strcmp(g_pool[k].host, host)) break;
            if (!g_pool[k].host[0] && free_k < 0) free_k = k;
            if (g_pool[k].host[0] && !g_pool[k].busy && (old_k < 0 || g_pool[k].last_ms < g_pool[old_k].last_ms)) old_k = k;
        }
        if (k < POOL_N && !g_pool[k].busy) {
            g_pool[k].busy = 1;
            Conn *o = g_pool[k].c;
            g_pool[k].c = NULL;
            pool_unlock();
            if (o && !no_reuse && now_ms64() - g_pool[k].last_ms < POOL_IDLE_MS) *c = o;
            else conn_drop(o);
            return k;
        }
        if (k == POOL_N) {                                   /* (a new server: a free entry, or the oldest unused one) */
            int n = free_k >= 0 ? free_k : old_k;
            if (n < 0) { pool_unlock(); return -1; }
            Conn *o = g_pool[n].c;
            memset(&g_pool[n], 0, sizeof g_pool[n]);
            snprintf(g_pool[n].host, sizeof g_pool[n].host, "%s", host);
            g_pool[n].port = port;
            g_pool[n].tls = tls;
            g_pool[n].busy = 1;
            pool_unlock();
            conn_drop(o);
            return n;
        }
        pool_unlock();                                       /* (its connection is in use: wait for it) */
        if (stopped(rq) || now_ms64() - t0 > POOL_WAIT_MS) return -1;
        sleep_us(20000);
    }
}
/* the entry given back, with the connection to keep (NULL: none) */
static void pool_give(int k, Conn *c)
{
    if (k < 0) { conn_drop(c); return; }
    pool_lock();
    if (g_pool[k].drop || !g_pool_keep) { g_pool[k].drop = 0; g_pool[k].busy = 0; pool_unlock(); conn_drop(c); return; }
    Conn *o = g_pool[k].c;
    g_pool[k].c = c;
    if (c) g_pool[k].last_ms = now_ms64();
    g_pool[k].busy = 0;
    pool_unlock();
    conn_drop(o);
}
void cio_pool_idle(void)
{
    Conn *gone[POOL_N];
    int n = 0;
    uint64_t now = now_ms64();
    pool_lock();
    for (int k = 0; k < POOL_N; k++)
        if (!g_pool[k].busy && g_pool[k].c && now - g_pool[k].last_ms >= POOL_IDLE_MS) { gone[n++] = g_pool[k].c; g_pool[k].c = NULL; }
    pool_unlock();
    for (int k = 0; k < n; k++) conn_drop(gone[k]);
}
void cio_pool_close(void)
{
    Conn *gone[POOL_N];
    int n = 0;
    pool_lock();
    for (int k = 0; k < POOL_N; k++) {
        if (g_pool[k].busy) g_pool[k].drop = 1;               /* (in use: closed when given back) */
        else if (g_pool[k].c) { gone[n++] = g_pool[k].c; g_pool[k].c = NULL; }
    }
    pool_unlock();
    for (int k = 0; k < n; k++) conn_drop(gone[k]);
}
int cio_pool_open(void)
{
    int n = 0;
    pool_lock();
    for (int k = 0; k < POOL_N; k++) n += g_pool[k].c != NULL || g_pool[k].busy;
    pool_unlock();
    return n;
}

int cio_get(const CioRequest *rq, int *status, char *err, size_t errsz)
{
    char url[HLS_URL_MAX], host[256], req[4096], auth[400], enc[560], val[HLS_URL_MAX];
    const char *path;
    int tls, port, ret = -1;
    char e2[1];
    if (!err) { err = e2; errsz = sizeof e2; }
    err[0] = 0;
    *status = 0;
    snprintf(url, sizeof url, "%s", rq->url);
    CioKeep *kp = rq->shared ? NULL : rq->keep;
    if (kp) kp->reused = 0;
    if (rq->reused) *rq->reused = 0;
    int no_reuse = 0, pk = -1, tried403 = 0;
#define DROP_CP() do { conn_drop(cp); cp = NULL; if (pk >= 0) { pool_give(pk, NULL); pk = -1; } } while (0)
    uint8_t *hb = malloc(16384 + 1), *buf = malloc(16384);
    if (!hb || !buf) { free(hb); free(buf); snprintf(err, errsz, "Out of memory"); return -1; }
    for (int redirect = 0; redirect < 8; redirect++) {
        if (parse_url(url, &tls, host, sizeof host, &port, &path) < 0) { snprintf(err, errsz, "Bad address"); ret = -1; break; }
        Conn *cp = NULL;
        int reused = 0, r = 0;
        if (rq->shared) {                                    /* 1.2.4r: the server's one connection */
            pk = pool_take(host, port, tls, rq, no_reuse, &cp);
            if (cp) reused = 1;
            if (pk < 0 && stopped(rq)) { ret = -2; snprintf(err, errsz, "Stopped"); break; }
        } else if (kp) {                                     /* 1.2.4p: the kept one, if it is to this server and fresh */
            cp = keep_take(kp, host, port, tls);
            if (cp && no_reuse) { conn_drop(cp); cp = NULL; }
            if (cp) reused = 1;
        }
        if (cp) { cp->rq = rq; cp->idle_ms = 0; }
        else {
            cp = calloc(1, sizeof *cp);
            if (!cp) { snprintf(err, errsz, "Out of memory"); ret = -1; break; }
            cp->fd = -1;
            cp->rq = rq;
            r = conn_open(cp, host, port, tls, err, errsz);
            if (r == -1005 && !stopped(rq)) {
                /* 1.2.4m: a handshake that failed (not timed out) is tried once more on a new connection: a CDN's HLS
                 * 1080p answered "-0x6C00 internal error" once on the device, and the player took it for a variant
                 * without a picture */
                conn_close(cp);
                memset(cp, 0, sizeof *cp);
                cp->fd = -1;
                cp->rq = rq;
                r = conn_open(cp, host, port, tls, err, errsz);
                if (r == -1005) { size_t l = strlen(err); snprintf(err + l, errsz - l, " (twice)"); }
                else if (r >= 0) err[0] = 0;
            }
            if (r < 0) { DROP_CP(); ret = r; break; }
        }
        int def = (tls && port == 443) || (!tls && port == 80);
        int k = 0;
        rq_add(req, sizeof req, &k, "GET %s HTTP/1.1\r\nHost: %s", path, host);
        if (!def) rq_add(req, sizeof req, &k, ":%d", port);
        rq_add(req, sizeof req, &k, "\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: %s\r\n",
                      rq->ua && rq->ua[0] ? rq->ua : NET_UA, kp || pk >= 0 ? "keep-alive" : "close");
        if (rq->referer && rq->referer[0]) rq_add(req, sizeof req, &k, "Referer: %s\r\n", rq->referer);
        if (rq->user && rq->user[0] && redirect == 0) {
            snprintf(auth, sizeof auth, "%s:%s", rq->user, rq->pass ? rq->pass : "");
            b64(auth, enc);
            rq_add(req, sizeof req, &k, "Authorization: Basic %s\r\n", enc);
        }
        if (rq->range) {
            if (rq->range_to) rq_add(req, sizeof req, &k, "Range: bytes=%llu-%llu\r\n",
                                            (unsigned long long)rq->range_from, (unsigned long long)rq->range_to);
            else rq_add(req, sizeof req, &k, "Range: bytes=%llu-\r\n", (unsigned long long)rq->range_from);
        }
        rq_add(req, sizeof req, &k, "\r\n");
        if (k >= (int)sizeof req - 1) { DROP_CP(); snprintf(err, errsz, "Request too long"); ret = -1008; break; }
        if (conn_write(cp, req, (size_t)k) < 0) {
            DROP_CP();
            if (reused) { no_reuse = 1; redirect--; continue; }   /* (the server closed the kept one: a new one) */
            snprintf(err, errsz, "Could not send the request"); ret = -1007; break;
        }
        /* headers */
        size_t hl = 0;
        char *eoh = NULL;
        while (!eoh && hl < 16384) {
            int n = conn_read(cp, hb + hl, 16384 - hl);
            if (n <= 0) { r = n; break; }
            hl += (size_t)n;
            hb[hl] = 0;
            eoh = strstr((char *)hb, "\r\n\r\n");
        }
        if (!eoh) {
            DROP_CP();
            if (r == -2) { ret = -2; break; }
            if (reused && hl == 0) { no_reuse = 1; redirect--; continue; }   /* (closed meanwhile: a new one) */
            if (r == -1006) snprintf(err, errsz, "The server did not answer");
            else if (hl == 0) snprintf(err, errsz, "The server closed the connection without an answer (%d)", r);
            else snprintf(err, errsz, "Bad answer from the server (%u bytes)", (unsigned)hl);
            ret = r == -1006 ? -1006 : -1008;
            break;
        }
        if (kp) kp->reused |= reused;
        if (rq->reused) *rq->reused = reused;
        *eoh = 0;
        int code = 0;
        if (sscanf((char *)hb, "HTTP/%*d.%*d %d", &code) != 1 &&
            sscanf((char *)hb, "ICY %d", &code) != 1) {      /* (0.6.0: radio servers answer "ICY 200 OK") */
            char head[24];
            size_t k = 0;
            for (; k < sizeof head - 1 && hb[k]; k++) head[k] = hb[k] >= 32 && hb[k] < 127 ? (char)hb[k] : '.';
            head[k] = 0;
            DROP_CP();
            snprintf(err, errsz, "Bad answer from the server: %s", head);
            ret = -1008;
            break;
        }
        *status = code;
        if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) && header((char *)hb, "Location", val, sizeof val)) {
            char next[HLS_URL_MAX];
            hls_resolve(url, val, -1, next, sizeof next);
            snprintf(url, sizeof url, "%s", next);
            DROP_CP();
            continue;
        }
        if (rq->final_url && rq->final_cap) snprintf(rq->final_url, rq->final_cap, "%s", url);
        if (code == 403 && rq->shared && !tried403 && !stopped(rq)) {   /* 1.2.4r: once more on a new connection */
            DROP_CP();
            tried403 = 1;
            sleep_us(300000);
            no_reuse = 1;
            redirect--;
            continue;
        }
        if (code >= 400) { DROP_CP(); ret = 0; break; }   /* the caller looks at the status */
        Body b;
        memset(&b, 0, sizeof b);
        b.c = cp;
        b.pend = (uint8_t *)eoh + 4;
        b.npend = hl - (size_t)(b.pend - hb);
        b.left = -1;
        if (header((char *)hb, "Transfer-Encoding", val, sizeof val) && strstr(val, "chunked")) { b.chunked = 1; b.left = -1; }
        else if (header((char *)hb, "Content-Length", val, sizeof val)) b.left = atoll(val);
        if (b.chunked) b.left = -1;
        /* 1.2.4p: kept afterwards only with a known end (length or chunks), HTTP/1.1 and no "Connection: close" */
        int keepable = (kp || pk >= 0) && (b.chunked || b.left >= 0) && !strncmp((char *)hb, "HTTP/1.1", 8) &&
                       !(header((char *)hb, "Connection", val, sizeof val) && has_close(val));
        if (rq->total) {
            uint64_t tot = 0;
            if (header((char *)hb, "Content-Range", val, sizeof val)) {
                const char *sl = strchr(val, '/');
                if (sl && sl[1] != '*') tot = strtoull(sl + 1, NULL, 10);
            } else if (code == 200 && b.left > 0) tot = (uint64_t)b.left;
            *rq->total = tot;
        }
        ret = 0;
        int ended = 0;
        for (;;) {
            int n = body_read(&b, buf, 16384);
            if (n == 0) { ended = b.done || (!b.chunked && b.left == 0); break; }
            if (n < 0) {
                if (n == -2) ret = -2;
                else { ret = n; snprintf(err, errsz, n == -1006 ? "No data for too long" : "Connection lost"); }
                break;
            }
            if (rq->data && rq->data(rq->ctx, buf, (size_t)n)) break;
        }
        if (keepable && ended && b.chunked) {               /* (the trailer: lines up to an empty one) */
            int ch, len = 0, ok = 0;
            while ((ch = raw_byte(&b)) >= 0) {
                if (ch == '\n') { if (len == 0) { ok = 1; break; } len = 0; }
                else if (ch != '\r') len++;
            }
            if (!ok) ended = 0;
        }
        if (keepable && ended && ret == 0 && b.npend == 0 && pk >= 0) { pool_give(pk, cp); pk = -1; cp = NULL; }
        else if (keepable && ended && ret == 0 && b.npend == 0) {
            keep_put(kp, cp, host, port, tls);
        } else DROP_CP();
        break;
    }
    if (pk >= 0) pool_give(pk, NULL);                        /* (never left taken) */
#undef DROP_CP
    if (ret == -1 && !err[0]) snprintf(err, errsz, "Too many redirects");
    free(hb);
    free(buf);
    return ret;
}

#else   /* built without TLS */

int cio_available(void) { return 0; }
int cio_last_rcvbuf(void) { return 0; }
int cio_last_connect_ms(void) { return 0; }
int cio_last_connect_tries(void) { return 0; }
const char *cio_rcvbuf_note(void) { return ""; }
const char *cio_last_suite(void) { return ""; }
CioKeep *cio_keep_new(void) { return NULL; }
void cio_keep_free(CioKeep *k) { (void)k; }
int cio_keep_reused(const CioKeep *k) { (void)k; return 0; }
void cio_keep_close(CioKeep *k) { (void)k; }
int cio_dns_stale(void) { return 0; }
void cio_pool_idle(void) {}
void cio_pool_close(void) {}
void cio_pool_keep(int on) { (void)on; }
int cio_pool_open(void) { return 0; }
void cio_init(void) {}
int cio_get(const CioRequest *rq, int *status, char *err, size_t errsz)
{
    (void)rq;
    *status = 0;
    if (err) snprintf(err, errsz, "HTTPS support is not built in");
    return -1;
}

#endif
