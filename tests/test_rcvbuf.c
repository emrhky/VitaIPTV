/* 0.6.0: the receive buffer of src/curlio.c (set_rcvbuf) against a system that refuses sizes above a limit
 * (the Vita refused 1 MB and even 528 KB with ENOBUFS and took 512 KB). Built by tests/run_curlio_test.py. */
#include <sys/socket.h>
#include <errno.h>
#include <assert.h>
static int LIMIT, cur = 64512;
static int fake_set(int fd, int l, int o, const void *v, socklen_t n)
{
    (void)fd; (void)l; (void)o; (void)n;
    int x = *(const int *)v;
    if (x > LIMIT) { errno = 105; return -1; }
    cur = x;
    return 0;
}
static int fake_get(int fd, int l, int o, void *v, socklen_t *n)
{
    (void)fd; (void)l; (void)n;
    *(int *)v = o == SO_RCVBUF ? cur : 0;
    return 0;
}
#define setsockopt fake_set
#define getsockopt fake_get
#include "../src/curlio.c"
#include <stdio.h>
#include <string.h>

static void run(int limit, int want, const char *want_note)
{
    LIMIT = limit; cur = 64512;
    set_rcvbuf(3);
    const char *n = cio_rcvbuf_note();
    printf("limit %7d: buffer %d, note '%s'\n", limit, g_rcvbuf, n);
    assert(g_rcvbuf == want && (!want_note || !strcmp(n, want_note)));
}

int main(void)
{
    run(524288, 512 * 1024, "512 KB");                               /* the Vita: one try, said once */
    run(524288, 512 * 1024, NULL);
    assert(!cio_rcvbuf_note()[0]);                                  /* (not again) */
    run(300000, 256 * 1024, "512 KB refused (105), using 256 KB");   /* refused: smaller, and said */
    run(150000, 128 * 1024, "512 KB refused (105), 256 KB refused (105), using 128 KB");
    run(50000, 64512, "512 KB refused (105), 256 KB refused (105), 128 KB refused (105), using 63 KB");
    puts("rcvbuf test passed");
    return 0;
}
