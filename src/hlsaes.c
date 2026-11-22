/* 1.2.2: AES-128 HLS segments (see hlsaes.h) */
#include "hlsaes.h"

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_TLS
#include <mbedtls/aes.h>

#define CHUNK 16384

struct HlsAes {
    mbedtls_aes_context ctx;
    uint8_t iv[16];
    uint8_t part[16];                   /* bytes of a block not complete yet */
    int npart;
    uint8_t last[16];                   /* the newest whole block, decrypted at the end (padding) */
    int have_last;
    uint8_t work[CHUNK];
    uint8_t plain[CHUNK];
};

int hlsaes_available(void) { return 1; }

HlsAes *hlsaes_create(void)
{
    HlsAes *a = calloc(1, sizeof *a);
    if (a) mbedtls_aes_init(&a->ctx);
    return a;
}

void hlsaes_destroy(HlsAes *a)
{
    if (!a) return;
    mbedtls_aes_free(&a->ctx);
    free(a);
}

void hlsaes_start(HlsAes *a, const uint8_t key[16], const uint8_t iv[16])
{
    mbedtls_aes_setkey_dec(&a->ctx, key, 128);
    memcpy(a->iv, iv, 16);
    a->npart = 0;
    a->have_last = 0;
}

/* decrypts n (a multiple of 16) bytes of a->work */
static void run(HlsAes *a, size_t n, HlsAesOut out, void *ctx)
{
    if (!n) return;
    mbedtls_aes_crypt_cbc(&a->ctx, MBEDTLS_AES_DECRYPT, n, a->iv, a->work, a->plain);
    out(ctx, a->plain, n);
}

void hlsaes_feed(HlsAes *a, const uint8_t *in, size_t n, HlsAesOut out, void *ctx)
{
    size_t w = 0;
    while (n) {
        /* complete the open block */
        size_t take = 16 - (size_t)a->npart;
        if (take > n) take = n;
        memcpy(a->part + a->npart, in, take);
        a->npart += (int)take;
        in += take;
        n -= take;
        if (a->npart < 16) break;
        a->npart = 0;
        /* the block held back goes to work, this one is held back */
        if (a->have_last) {
            memcpy(a->work + w, a->last, 16);
            w += 16;
            if (w == CHUNK) { run(a, w, out, ctx); w = 0; }
        }
        memcpy(a->last, a->part, 16);
        a->have_last = 1;
        /* whole blocks straight from the input (all but the newest) */
        while (n >= 32 && a->npart == 0) {
            memcpy(a->work + w, a->last, 16);
            w += 16;
            if (w == CHUNK) { run(a, w, out, ctx); w = 0; }
            memcpy(a->last, in, 16);
            in += 16;
            n -= 16;
        }
    }
    run(a, w, out, ctx);
}

int hlsaes_end(HlsAes *a, HlsAesOut out, void *ctx)
{
    if (!a->have_last) return a->npart ? -1 : 0;
    memcpy(a->work, a->last, 16);
    mbedtls_aes_crypt_cbc(&a->ctx, MBEDTLS_AES_DECRYPT, 16, a->iv, a->work, a->plain);
    a->have_last = 0;
    int pad = a->plain[15], bad = a->npart != 0 || pad < 1 || pad > 16;
    for (int i = 16 - pad; !bad && i < 16; i++) if (a->plain[i] != pad) bad = 1;
    if (bad) { out(ctx, a->plain, 16); return -1; }    /* (given as it is: the picture shows if it was only the padding) */
    if (pad < 16) out(ctx, a->plain, (size_t)(16 - pad));
    return 0;
}

#else   /* without mbedTLS */

int     hlsaes_available(void) { return 0; }
HlsAes *hlsaes_create(void) { return NULL; }
void    hlsaes_destroy(HlsAes *a) { (void)a; }
void    hlsaes_start(HlsAes *a, const uint8_t key[16], const uint8_t iv[16]) { (void)a; (void)key; (void)iv; }
void    hlsaes_feed(HlsAes *a, const uint8_t *in, size_t n, HlsAesOut out, void *ctx)
{ (void)a; (void)in; (void)n; (void)out; (void)ctx; }
int     hlsaes_end(HlsAes *a, HlsAesOut out, void *ctx) { (void)a; (void)out; (void)ctx; return -1; }

#endif
