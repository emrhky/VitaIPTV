/* 1.2.2: HLS segments encrypted with AES-128 (#EXT-X-KEY:METHOD=AES-128): the standard HLS encryption whose key
 * the playlist names openly (not DRM: SAMPLE-AES and the DRM systems stay unsupported). AES-128-CBC with PKCS#7
 * padding, decrypted as the segment comes in. Needs the built-in mbedTLS (HAVE_TLS); without it nothing works. */
#ifndef HLSAES_H
#define HLSAES_H

#include <stddef.h>
#include <stdint.h>

typedef struct HlsAes HlsAes;
typedef void (*HlsAesOut)(void *ctx, const uint8_t *data, size_t len);

int     hlsaes_available(void);
HlsAes *hlsaes_create(void);
void    hlsaes_destroy(HlsAes *a);
/* a segment starts: its key and IV */
void    hlsaes_start(HlsAes *a, const uint8_t key[16], const uint8_t iv[16]);
/* encrypted bytes in, plain bytes to out (the last block is held back until hlsaes_end: it has the padding) */
void    hlsaes_feed(HlsAes *a, const uint8_t *in, size_t n, HlsAesOut out, void *ctx);
/* the segment ended: the last block without its padding. Returns 0, or -1 when the padding is wrong (wrong key). */
int     hlsaes_end(HlsAes *a, HlsAesOut out, void *ctx);

#endif
