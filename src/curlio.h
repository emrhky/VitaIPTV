#ifndef CURLIO_H
#define CURLIO_H
/* HTTPS with the app's own TLS (mbedTLS in third_party/, built in with HAVE_TLS). The Vita's own TLS
 * (sceHttp/sceSsl) fails on most sites today (handshake error 0x80431075), so https:// addresses go
 * through here. Without HAVE_TLS cio_available() is 0 and the app stays on sceHttp.
 * 0.6.0: plain http:// too (films, channels, lists): its own socket asks for a 1 MB receive buffer, the
 * system HTTP library keeps a small one, and with a long round trip the speed stops near buffer / time. */
#include <stddef.h>
#include <stdint.h>

/* called with every piece of the body; return non-zero to stop the transfer */
typedef int (*CioData)(void *ctx, const uint8_t *data, size_t len);
/* polled while waiting; return non-zero to abort (e.g. the player is stopping) */
typedef int (*CioStop)(void *ctx);

typedef struct {
    const char *url;
    const char *user, *pass;            /* HTTP basic authentication (may be NULL) */
    int connect_timeout_s;              /* 0 = 15 */
    int idle_timeout_s;                 /* no data for this long ends the transfer; 0 = 15 */
    CioData data;
    CioStop stop;
    void *ctx;
    int range;                          /* 1: ask only for bytes range_from..range_to (range_to 0 = to the end) */
    uint64_t range_from, range_to;
    uint64_t *total;                    /* out, may be NULL: the whole file's size when the server tells it */
    const char *ua, *referer;           /* User-Agent / Referer to send (NULL or "" = the app's own / none) */
    char *final_url;                    /* out, may be NULL: the address that answered, after redirects */
    size_t final_cap;
} CioRequest;

int  cio_available(void);
void cio_init(void);                    /* once, at start-up (curl_global_init) */
/* Returns 0 when the body ended (or data() stopped it), or a negative error. status = HTTP status
 * (0 if none arrived); err gets a short English description of a failure. */
int  cio_get(const CioRequest *rq, int *status, char *err, size_t errsz);
/* 0.6.0: the receive buffer the last connection got from the system, in bytes (0 = unknown) */
int  cio_last_rcvbuf(void);

#endif
