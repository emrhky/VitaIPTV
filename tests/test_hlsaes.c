/* 1.2.2: AES-128 HLS decrypting (src/hlsaes.c) and the IV helpers (src/hls.c), against openssl's output.
 * argv: plain file, encrypted file (openssl enc -aes-128-cbc, key 10..1f, IV 0a1b...f9) */
#include "hlsaes.h"
#include "hls.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *p; size_t n, cap; } Buf;
static void put(void *ctx, const uint8_t *d, size_t n)
{
    Buf *b = ctx;
    assert(b->n + n <= b->cap);
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static uint8_t *load(const char *f, size_t *n)
{
    FILE *fp = fopen(f, "rb");
    assert(fp);
    fseek(fp, 0, SEEK_END);
    *n = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *p = malloc(*n + 1);
    assert(fread(p, 1, *n, fp) == *n);
    fclose(fp);
    return p;
}

int main(int argc, char **argv)
{
    uint8_t iv[16], want[16] = { 0x0a,0x1b,0x2c,0x3d,0x4e,0x5f,0x60,0x71,0x82,0x93,0xa4,0xb5,0xc6,0xd7,0xe8,0xf9 };
    assert(hls_parse_iv("0x0a1b2c3d4e5f60718293a4b5c6d7e8f9", iv) && !memcmp(iv, want, 16));
    assert(hls_parse_iv("0X1F", iv) && iv[15] == 0x1f && iv[14] == 0 && iv[0] == 0);
    assert(!hls_parse_iv("1234", iv) && !hls_parse_iv("0xzz", iv) && !hls_parse_iv("0x", iv));
    hls_seq_iv(0x0102030405LL, iv);
    assert(iv[15] == 5 && iv[11] == 1 && iv[10] == 0 && iv[0] == 0);
    {   /* the playlist: AES-128 segments with their key and IV, SAMPLE-AES refused, NONE ends it */
        const char *pl = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:7\n"
                         "#EXT-X-KEY:METHOD=AES-128,URI=\"k1.bin\"\n#EXTINF:2,\na.ts\n"
                         "#EXT-X-KEY:METHOD=AES-128,URI=\"https://x/k2\",IV=0x0A1B2C3D4E5F60718293A4B5C6D7E8F9\n#EXTINF:2,\nb.ts\n"
                         "#EXT-X-KEY:METHOD=NONE\n#EXTINF:2,\nc.ts\n#EXT-X-ENDLIST\n";
        HlsMedia m;
        assert(hls_parse_media(pl, &m) == 0 && m.nseg == 3 && m.aes && !m.encrypted);
        assert(m.seg[0].aes && !m.seg[0].has_iv && m.seg[0].key_uri_len == 6 && !strncmp(m.seg[0].key_uri, "k1.bin", 6) && m.seg[0].seq == 7);
        assert(m.seg[1].aes && m.seg[1].has_iv && !memcmp(m.seg[1].iv, want, 16) && !strncmp(m.seg[1].key_uri, "https://x/k2", 12));
        assert(!m.seg[2].aes && !m.seg[2].key_uri);
        hls_media_free(&m);
        assert(hls_parse_media("#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"k\"\n#EXTINF:2,\na.ts\n#EXT-X-ENDLIST\n", &m) == 0 && m.encrypted && !m.aes);
        hls_media_free(&m);
    }
    if (argc < 3) { puts("hlsaes: no files, decrypting not tested"); return 0; }
    assert(hlsaes_available());
    size_t pn, cn;
    uint8_t *plain = load(argv[1], &pn), *enc = load(argv[2], &cn);
    uint8_t key[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(16 + i);
    HlsAes *a = hlsaes_create();
    assert(a);
    Buf out = { malloc(cn + 16), 0, cn + 16 };
    static const size_t steps[] = { 1, 7, 15, 16, 17, 31, 32, 33, 1000, 16384, 70000 };
    for (size_t s = 0; s < sizeof steps / sizeof steps[0]; s++) {   /* fed in pieces of every size */
        out.n = 0;
        hlsaes_start(a, key, want);
        for (size_t at = 0; at < cn; at += steps[s]) hlsaes_feed(a, enc + at, cn - at < steps[s] ? cn - at : steps[s], put, &out);
        assert(hlsaes_end(a, put, &out) == 0);
        assert(out.n == pn && !memcmp(out.p, plain, pn));
    }
    key[0] ^= 1;                                            /* a wrong key: the padding says so */
    out.n = 0;
    hlsaes_start(a, key, want);
    hlsaes_feed(a, enc, cn, put, &out);
    assert(hlsaes_end(a, put, &out) < 0);
    hlsaes_destroy(a);
    free(out.p); free(plain); free(enc);
    printf("hlsaes: %zu bytes decrypted in 11 piece sizes, wrong key found\n", pn);
    puts("all hlsaes tests passed");
    return 0;
}
