/* PC test of src/listcache.c (1.1 saved copies of playlists):
 *   gcc -Wall -Wextra -fsanitize=address,undefined -Isrc tests/test_listcache.c src/listcache.c -o t && ./t */
#include "listcache.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    const char *path = "/tmp/listcache_test.bin";
    char big[70000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (char)(i * 7 + (i >> 8));   /* any bytes, newlines and zeros too */
    CachePart parts[2] = { { "cats", "[{\"category_id\":\"1\"}]\n", 23 }, { "live", big, sizeof big } };
    assert(listcache_write(path, 1730000000, 10800, 1, parts, 2) == 0);
    ListCache c;
    assert(listcache_read(path, &c) == 0);
    assert(c.n == 2 && c.made == 1730000000 && c.srv_off == 10800 && c.srv_known == 1);
    size_t n; uint32_t h;
    const char *d = listcache_part(&c, "live", &n, &h);
    assert(d && n == sizeof big && !memcmp(d, big, n) && h == listcache_hash(big, sizeof big));
    d = listcache_part(&c, "cats", &n, NULL);
    assert(d && n == 23 && !memcmp(d, parts[0].data, 23));
    assert(!listcache_part(&c, "m3u", NULL, NULL));
    listcache_free(&c);

    /* cut short or changed: not used */
    FILE *f = fopen(path, "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *all = malloc((size_t)sz); assert(fread(all, 1, (size_t)sz, f) == (size_t)sz); fclose(f);
    f = fopen(path, "wb"); fwrite(all, 1, (size_t)sz - 100, f); fclose(f);
    assert(listcache_read(path, &c) == -1);
    all[sz / 2] ^= 1;
    f = fopen(path, "wb"); fwrite(all, 1, (size_t)sz, f); fclose(f);
    assert(listcache_read(path, &c) == -1);
    f = fopen(path, "wb"); fputs("something else\n", f); fclose(f);
    assert(listcache_read(path, &c) == -1);
    remove(path);
    assert(listcache_read(path, &c) == -1);                  /* none */
    free(all);

    /* an empty part, one part */
    CachePart one = { "m3u", "", 0 };
    assert(listcache_write(path, 5, 0, 0, &one, 1) == 0 && listcache_read(path, &c) == 0 && c.n == 1);
    assert(listcache_part(&c, "m3u", &n, NULL) && n == 0);
    listcache_free(&c);
    remove(path);
    puts("listcache: all tests passed");
    return 0;
}
