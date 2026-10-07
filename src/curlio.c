#include "curlio.h"
#include "netua.h"
#include <stdio.h>
#include <string.h>

#ifdef HAVE_CURL
#include <curl/curl.h>

typedef struct {
    const CioRequest *rq;
    CURL *c;
    int status;
    int stopped;                        /* data() asked to stop: not an error */
} Xfer;

static size_t on_write(char *p, size_t sz, size_t n, void *ud)
{
    Xfer *x = ud;
    size_t len = sz * n;
    if (!x->status) {
        long code = 0;
        curl_easy_getinfo(x->c, CURLINFO_RESPONSE_CODE, &code);
        x->status = (int)code;
    }
    if (x->status >= 400) { x->stopped = 1; return 0; }  /* the error page is not the body we want */
    if (x->rq->data && x->rq->data(x->rq->ctx, (const uint8_t *)p, len)) { x->stopped = 1; return 0; }
    return len;
}

static int on_progress(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un)
{
    (void)dt; (void)dn; (void)ut; (void)un;
    Xfer *x = ud;
    return x->rq->stop && x->rq->stop(x->rq->ctx) ? 1 : 0;
}

int cio_available(void) { return 1; }

void cio_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

int cio_get(const CioRequest *rq, int *status, char *err, size_t errsz)
{
    Xfer x;
    memset(&x, 0, sizeof x);
    x.rq = rq;
    *status = 0;
    if (err && errsz) err[0] = 0;
    x.c = curl_easy_init();
    if (!x.c) { if (err) snprintf(err, errsz, "HTTPS init failed"); return -1; }
    char auth[512];
    curl_easy_setopt(x.c, CURLOPT_URL, rq->url);
    curl_easy_setopt(x.c, CURLOPT_USERAGENT, NET_UA);
    curl_easy_setopt(x.c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(x.c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(x.c, CURLOPT_SSL_VERIFYPEER, 0L);     /* the Vita has no current root certificates */
    curl_easy_setopt(x.c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(x.c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(x.c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(x.c, CURLOPT_CONNECTTIMEOUT, (long)(rq->connect_timeout_s ? rq->connect_timeout_s : 15));
    curl_easy_setopt(x.c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(x.c, CURLOPT_LOW_SPEED_TIME, (long)(rq->idle_timeout_s ? rq->idle_timeout_s : 15));
    curl_easy_setopt(x.c, CURLOPT_BUFFERSIZE, 16384L);
    curl_easy_setopt(x.c, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(x.c, CURLOPT_WRITEDATA, &x);
    curl_easy_setopt(x.c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(x.c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(x.c, CURLOPT_XFERINFODATA, &x);
    if (rq->user && rq->user[0]) {
        snprintf(auth, sizeof auth, "%s:%s", rq->user, rq->pass ? rq->pass : "");
        curl_easy_setopt(x.c, CURLOPT_USERPWD, auth);
        curl_easy_setopt(x.c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    }
    CURLcode r = curl_easy_perform(x.c);
    if (!x.status) {
        long code = 0;
        curl_easy_getinfo(x.c, CURLINFO_RESPONSE_CODE, &code);
        x.status = (int)code;
    }
    *status = x.status;
    int ret = 0;
    if (r != CURLE_OK && !(x.stopped && (r == CURLE_WRITE_ERROR)) ) {
        if (r == CURLE_ABORTED_BY_CALLBACK) ret = -2;       /* we were stopped */
        else {
            ret = -1000 - (int)r;
            if (err) snprintf(err, errsz, "%s", curl_easy_strerror(r));
        }
    }
    curl_easy_cleanup(x.c);
    return ret;
}

#else   /* built without curl */

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
