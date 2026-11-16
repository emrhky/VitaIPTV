/* 1.3: src/dl.c on the PC against tests/run_dl_test.py's server (DL_BASE): downloads, going on after a lost
 * connection, a server without ranges, errors, too little space, pause / resume, delete. */
#include "dl.h"
#include <assert.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdarg.h>
void plog(const char *fmt, ...) { if (!getenv("VERBOSE")) return; va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar(10); }

static long long fsize(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long long)st.st_size : -1; }

static int same_file(const char *a, const char *b)
{
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    int ok = x && y;
    while (ok) {
        int c = fgetc(x), d = fgetc(y);
        if (c != d) ok = 0;
        if (c == EOF || d == EOF) break;
    }
    if (x) fclose(x);
    if (y) fclose(y);
    return ok;
}

/* waits until dir is not queued / active (max_s), returns its last state */
static int wait_end(const char *dir, double max_s)
{
    DlState s;
    for (int k = 0; k < (int)(max_s * 20); k++) {
        int st = dl_state(dir, &s);
        if (st != DL_ACTIVE && st != DL_QUEUED) return st;
        usleep(50000);
    }
    return dl_state(dir, &s);
}

static DlItem mk(const char *base, const char *path, const char *title, char kind)
{
    DlItem it;
    memset(&it, 0, sizeof it);
    snprintf(it.url, sizeof it.url, "%s%s", base, path);
    snprintf(it.title, sizeof it.title, "%s", title);
    it.kind = kind;
    return it;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *base = getenv("DL_BASE"), *src = argc > 1 ? argv[1] : NULL, *root = argc > 2 ? argv[2] : NULL;
    assert(base && src && root);
    char vid[600];

    /* names */
    char nm[96];
    dl_folder_name('F', 0x1a2b3c4d, "Kızılcık Şerbeti: Çok Özel Bölüm / 2 ?", nm, sizeof nm);
    printf("folder name: %s\n", nm);
    assert(!strcmp(nm, "F_1a2b3c4d_Kizilcik_Serbeti_Cok_Ozel_Bolum_2"));
    dl_folder_name('E', 1, "", nm, sizeof nm);
    assert(!strcmp(nm, "E_00000001"));
    dl_video_name("http://h/movie/u/p/12.MP4?token=1", nm, sizeof nm); assert(!strcmp(nm, "video.mp4"));
    dl_video_name("http://h/series/u/p/9.ts|User-Agent=x", nm, sizeof nm); assert(!strcmp(nm, "video.ts"));
    dl_video_name("http://h/x", nm, sizeof nm); assert(!strcmp(nm, "video.mkv"));
    dl_size_text(1503238553ull, 1, nm, sizeof nm); assert(!strcmp(nm, "1,4 GB"));
    dl_size_text(860ull << 20, 0, nm, sizeof nm); assert(!strcmp(nm, "860 MB"));

    dl_init(root);
    long long whole = fsize(src);
    assert(whole > 3 * 1024 * 1024);

    /* 1. a film: whole, with its poster and information */
    DlItem a = mk(base, "/film.mkv", "Örnek Film", 'F');
    snprintf(a.poster_url, sizeof a.poster_url, "%s/poster.jpg", base);
    snprintf(a.year, sizeof a.year, "2021");
    snprintf(a.plot, sizeof a.plot, "Bir satır\nikinci satır");
    a.avkey = 0xabcdef01;
    assert(dl_add(&a) == 0);
    int st = wait_end(a.dir, 30);
    snprintf(vid, sizeof vid, "%s/%s", a.dir, a.video);
    printf("1 whole: state %d, %lld of %lld bytes\n", st, fsize(vid), whole);
    assert(st == DL_IDLE && same_file(vid, src));
    {
        char p[700]; snprintf(p, sizeof p, "%s/poster.jpg", a.dir);
        assert(fsize(p) > 100);
        DlItem r;
        assert(dl_info_read(&r, a.dir) == 0 && r.done && r.size == (uint64_t)whole && !strcmp(r.title, "Örnek Film"));
        assert(!strcmp(r.plot, "Bir satır ikinci satır") && r.avkey == 0xabcdef01 && !strcmp(r.year, "2021"));
    }
    assert(dl_add(&a) == 1);                                 /* there already */

    /* 2. the connection drops every 1 MB: goes on where it stopped (ranges) */
    DlItem b = mk(base, "/drop/film.mkv", "Kesik", 'F');
    assert(dl_add(&b) == 0);
    st = wait_end(b.dir, 60);
    snprintf(vid, sizeof vid, "%s/%s", b.dir, b.video);
    printf("2 drops: state %d, %lld bytes\n", st, fsize(vid));
    assert(st == DL_IDLE && same_file(vid, src));

    /* 3. a server without ranges: stopped half way, then from the start again (no broken file) */
    DlItem c = mk(base, "/norange/slow/film.mkv", "Range yok", 'E');
    snprintf(c.series, sizeof c.series, "Dizi");
    c.season = 1; c.episode = 2;
    assert(dl_add(&c) == 0);
    snprintf(vid, sizeof vid, "%s/%s", c.dir, c.video);
    for (int k = 0; k < 200 && fsize(vid) < 1024 * 1024; k++) usleep(20000);
    dl_pause(c.dir);
    long long part = fsize(vid);
    DlState ds;
    printf("3 paused at %lld bytes (state %d)\n", part, dl_state(c.dir, &ds));
    assert(part > 0 && part < whole && dl_state(c.dir, &ds) == DL_IDLE);
    {
        DlItem lst[8];
        int n = dl_scan(lst, 8), found = 0;
        for (int i = 0; i < n; i++) if (!strcmp(lst[i].dir, c.dir)) { found = 1; assert(!lst[i].done && lst[i].have == (uint64_t)part && lst[i].kind == 'E' && lst[i].episode == 2); }
        assert(found && n == 3);
    }
    assert(dl_resume(c.dir) == 0);
    st = wait_end(c.dir, 60);
    printf("3 resumed: state %d, %lld bytes\n", st, fsize(vid));
    assert(st == DL_IDLE && same_file(vid, src));

    /* 4. errors: 404; too little space (the size is known after the first answer) */
    DlItem d = mk(base, "/nothing.mkv", "Yok", 'F');
    assert(dl_add(&d) == 0);
    st = wait_end(d.dir, 20);
    dl_state(d.dir, &ds);
    printf("4 404: state %d (%s)\n", st, ds.err);
    assert(st == DL_FAILED && strstr(ds.err, "404"));
    setenv("DL_TEST_FREE", "104857600", 1);                  /* 100 MB free: below the 200 MB kept free */
    DlItem e = mk(base, "/film2.mkv", "Sığmaz", 'F');
    assert(dl_add(&e) == 0);
    st = wait_end(e.dir, 20);
    dl_state(e.dir, &ds);
    printf("4 space: state %d (%s, %d MB needed, %d MB free)\n", st, ds.err, ds.err_arg_mb[0], ds.err_arg_mb[1]);
    assert(st == DL_FAILED && strstr(ds.err, "space") && ds.err_arg_mb[1] == 100);
    unsetenv("DL_TEST_FREE");
    assert(dl_resume(e.dir) == 0);                           /* room again: it goes */
    st = wait_end(e.dir, 30);
    snprintf(vid, sizeof vid, "%s/%s", e.dir, e.video);
    assert(st == DL_IDLE && same_file(vid, src));

    /* 5. delete: the folder goes (also while it downloads) */
    DlItem f = mk(base, "/slow/film3.mkv", "Silinecek", 'F');
    assert(dl_add(&f) == 0);
    usleep(300000);
    assert(dl_active());
    assert(dl_delete(f.dir) == 0 && fsize(f.dir) < 0 && !dl_active());
    assert(dl_delete(a.dir) == 0 && fsize(a.dir) < 0);
    assert(dl_delete("/etc") == -1);                         /* never outside the root */
    {
        DlItem lst[8];
        assert(dl_scan(lst, 8) == 4);                        /* b, c, d (failed, its folder stays), e */
    }
    dl_shutdown();
    printf("all download tests passed\n");
    return 0;
}
