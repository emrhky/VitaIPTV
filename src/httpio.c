/*
 * HTTP file reader for SceAvPlayer (its built-in reader rejects http:// URLs).
 *
 *  - Reads use HTTP Range requests, in blocks of HF_BLOCK bytes, because the
 *    player asks for tiny pieces (8 bytes) while parsing MP4 headers.
 *  - If the server ignores Range (answers 200), small files are downloaded
 *    once into memory and served from there.
 */
#include "nettls.h"
#include "httpio.h"
#include <psp2/types.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/http.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HF_BLOCK     (256 * 1024)
#ifndef HF_MAX_FULL
#define HF_MAX_FULL  (24 * 1024 * 1024)
#endif
#define HF_NO_RANGE  (-1000)

typedef struct {
    int tpl, conn;
    char url[512];
    uint64_t size;              /* 0 = unknown (live stream) */
    SceUID lock;
    int reads;
    uint8_t *blk;               /* cached block */
    uint32_t blk_cap, blk_len;
    uint64_t blk_pos;
    int no_range;               /* server ignores Range: whole file is cached */
} HttpFile;

static HttpFile g_hf = { .tpl = -1, .conn = -1 };

void *httpio_object(void) { return &g_hf; }

static void hf_free(HttpFile *f)
{
    if (f->conn >= 0) sceHttpDeleteConnection(f->conn);
    if (f->tpl >= 0) sceHttpDeleteTemplate(f->tpl);
    f->conn = f->tpl = -1;
    free(f->blk);
    f->blk = NULL;
    f->blk_cap = f->blk_len = 0;
}

int httpio_open(void *p, const char *filename)
{
    HttpFile *f = p;
    int req, r, status = 0;
    unsigned long long len = 0;

    hf_free(f);
    f->size = 0;
    f->reads = 0;
    f->no_range = 0;
    snprintf(f->url, sizeof f->url, "%s", filename);
    if (f->lock <= 0) f->lock = sceKernelCreateMutex("hf_lock", 0, 0, NULL);
    plog("hf_open");

    f->tpl = sceHttpCreateTemplate("VitaIPTV/1.0", SCE_HTTP_VERSION_1_1, 1);
    if (f->tpl < 0) { r = f->tpl; f->tpl = -1; plog("hf_open: template 0x%08X", (unsigned)r); return r; }
    sceHttpSetResolveTimeOut(f->tpl, 15 * 1000 * 1000);
    sceHttpSetConnectTimeOut(f->tpl, 15 * 1000 * 1000);
    sceHttpSetRecvTimeOut(f->tpl, 30 * 1000 * 1000);
    sceHttpSetAutoRedirect(f->tpl, 1);
    net_tls_relax(f->tpl);

    f->conn = sceHttpCreateConnectionWithURL(f->tpl, f->url, 1);
    if (f->conn < 0) { r = f->conn; f->conn = -1; plog("hf_open: connection 0x%08X", (unsigned)r); hf_free(f); return r; }

    req = sceHttpCreateRequestWithURL(f->conn, SCE_HTTP_METHOD_HEAD, f->url, 0);
    if (req < 0) { plog("hf_open: request 0x%08X", (unsigned)req); hf_free(f); return req; }
    r = sceHttpSendRequest(req, NULL, 0);
    if (r < 0) { plog("hf_open: send 0x%08X", (unsigned)r); sceHttpDeleteRequest(req); hf_free(f); return r; }
    sceHttpGetStatusCode(req, &status);
    r = sceHttpGetResponseContentLength(req, &len);
    if (r == 0) f->size = len;
    sceHttpDeleteRequest(req);
    plog("hf_open: status=%d size=%llu (length rc=0x%08X)", status, (unsigned long long)f->size, (unsigned)r);
    if (status >= 400) { hf_free(f); return -1; }
    return 0;
}

int httpio_close(void *p)
{
    plog("hf_close");
    hf_free((HttpFile *)p);
    return 0;
}

uint64_t httpio_size(void *p)
{
    return ((HttpFile *)p)->size;
}

/* Fetches [pos, pos+len) into the block cache. Returns bytes stored, HF_NO_RANGE, or < 0. */
static int hf_fetch(HttpFile *f, uint64_t pos, uint32_t len, int *status)
{
    int req, r, n, total = 0;
    char range[64];

    if (len > f->blk_cap) {
        uint8_t *nb = realloc(f->blk, len);
        if (!nb) return -2;
        f->blk = nb;
        f->blk_cap = len;
    }
    f->blk_len = 0;
    snprintf(range, sizeof range, "bytes=%llu-%llu",
             (unsigned long long)pos, (unsigned long long)(pos + len - 1));

    req = sceHttpCreateRequestWithURL(f->conn, SCE_HTTP_METHOD_GET, f->url, 0);
    if (req < 0) return req;
    sceHttpAddRequestHeader(req, "Range", range, SCE_HTTP_HEADER_OVERWRITE);
    r = sceHttpSendRequest(req, NULL, 0);
    if (r < 0) { sceHttpDeleteRequest(req); return r; }

    *status = 0;
    sceHttpGetStatusCode(req, status);
    if (*status == 200 && pos > 0) { sceHttpDeleteRequest(req); return HF_NO_RANGE; }
    if (*status != 206 && *status != 200) { sceHttpDeleteRequest(req); return -1; }

    while ((uint32_t)total < len) {
        n = sceHttpReadData(req, f->blk + total, len - (uint32_t)total);
        if (n < 0) { total = n; break; }
        if (n == 0) break;
        total += n;
    }
    sceHttpDeleteRequest(req);
    if (total < 0) return total;
    f->blk_pos = pos;
    f->blk_len = (uint32_t)total;
    return total;
}

int httpio_read(void *p, uint8_t *buffer, uint64_t position, uint32_t length)
{
    HttpFile *f = p;
    uint32_t want = length;
    int total = 0, status = 0, r;

    if (f->lock > 0) sceKernelLockMutex(f->lock, 1, NULL);

    if (f->size) {
        if (position >= f->size) goto done;                     /* end of file */
        if (position + want > f->size) want = (uint32_t)(f->size - position);
    }

    if (!(f->blk_len && position >= f->blk_pos && position + want <= f->blk_pos + f->blk_len)) {
        uint64_t fpos = position;
        uint32_t flen = want < HF_BLOCK ? HF_BLOCK : want;
        if (f->size && fpos + flen > f->size) flen = (uint32_t)(f->size - fpos);
        if (f->no_range) { fpos = 0; flen = (uint32_t)f->size; }

        r = hf_fetch(f, fpos, flen, &status);
        if (r == HF_NO_RANGE) {
            if (f->size && f->size <= HF_MAX_FULL) {
                plog("server ignores Range: caching the whole file");
                f->no_range = 1;
                r = hf_fetch(f, 0, (uint32_t)f->size, &status);
            } else {
                r = -1;
            }
        }
        if (r < 0) { total = r; goto done; }
    }

    if (position >= f->blk_pos && position < f->blk_pos + f->blk_len) {
        uint64_t avail = f->blk_pos + f->blk_len - position;
        uint32_t n = want < avail ? want : (uint32_t)avail;
        memcpy(buffer, f->blk + (position - f->blk_pos), n);
        total = (int)n;
    }

done:
    if (f->reads < 20 || total < 0) {
        f->reads++;
        plog("hf_read pos=%llu len=%u -> %d", (unsigned long long)position, (unsigned)length, total);
    }
    if (f->lock > 0) sceKernelUnlockMutex(f->lock, 1);
    return total;
}
