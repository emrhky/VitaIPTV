/* 0.6.0: the receive buffer search of src/curlio.c (set_rcvbuf) against a system that refuses sizes above a
 * limit (the Vita refused 1 MB with ENOBUFS and took 512 KB). Built by tests/run_curlio_test.py:
 * gcc -DHAVE_TLS <mbedtls flags> -Isrc tests/test_rcvbuf.c src/hls.c <mbedtls libs> */
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

static void run(int limit, int want_use, const char *want_text)
{
    LIMIT = limit; cur = 64512; g_rcv_pick = 0;
    set_rcvbuf(3);
    printf("limit %7d: %s (buffer %d)\n", limit, g_rcv_note, g_rcvbuf);
    assert(g_rcvbuf == want_use && strstr(g_rcv_note, want_text));
    cur = 64512;
    set_rcvbuf(4);                                           /* later connections: the size found, at once */
    assert(g_rcvbuf == want_use);
}

int main(void)
{
    run(870000, 768 * 1024, "limit 848 KB, using 768 KB");          /* (at most 768 KB used) */
    run(600000, 576 * 1024, "limit 576 KB, using 576 KB");
    run(2000000, 768 * 1024, "limit at least 1024 KB");
    run(300000, 288 * 1024, "512 KB: refused");
    run(50000, 64512, "none bigger than the default");
    LIMIT = 870000; cur = 64512; g_rcv_pick = 0; set_rcvbuf(3);    /* later the system takes less: smaller ones */
    LIMIT = 400000; cur = 64512; set_rcvbuf(5);
    assert(g_rcvbuf == 384 * 1024);
    puts("rcvbuf test passed");
    return 0;
}
