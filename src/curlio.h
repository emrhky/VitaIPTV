#ifndef CURLIO_H
#define CURLIO_H
/* HTTPS with the app's own TLS (mbedTLS in third_party/, built in with HAVE_TLS). The Vita's own TLS
 * (sceHttp/sceSsl) fails on most sites today (handshake error 0x80431075), so https:// addresses go
 * through here. Without HAVE_TLS cio_available() is 0 and the app stays on sceHttp. */
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
} CioRequest;

int  cio_available(void);
void cio_init(void);                    /* once, at start-up (curl_global_init) */
/* Returns 0 when the body ended (or data() stopped it), or a negative error. status = HTTP status
 * (0 if none arrived); err gets a short English description of a failure. */
int  cio_get(const CioRequest *rq, int *status, char *err, size_t errsz);

#endif
