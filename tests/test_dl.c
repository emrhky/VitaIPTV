/* 1.3: src/dl.c on the PC against tests/run_dl_test.py's server (DL_BASE): downloads, going on after a lost
 * connection, a server without ranges, errors, too little space, pause / resume, delete. */
#include "dl.h"
#include <assert.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <time.h>
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

static volatile int net_on = 1;                     /* 1.5.2: the network the downloads see */
static int net_up(void) { return net_on; }

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

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
    dl_folder_name('E', 2, "Organize İşler: Karun Hazinesi (2026) TR - S01E01 - 1. Bölüm", nm, sizeof nm);   /* (cut at 40: no dash at the end) */
    printf("folder name: %s\n", nm);
    assert(nm[strlen(nm) - 1] != '-' && nm[strlen(nm) - 1] != '_');
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
        snprintf(p, sizeof p, "%s/video.srt", a.dir);       /* 1.3.1: the subtitle file beside it on the server */
        assert(fsize(p) > 10);
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
    {   /* 1.4.1: the reason is kept in info.txt (the list says it after a restart); 404 is not asked again */
        DlItem r;
        assert(dl_info_read(&r, d.dir) == 0 && strstr(r.err, "does not give") && strstr(r.err, "404"));
        /* 1.5.1: an empty answer with no size is not a finished film: asked again twice, then it fails */
        {
            DlItem e = mk(base, "/empty/film.mkv", "Bos", 'F');
            double te = now_s();
            assert(dl_add(&e) == 0);
            int se = wait_end(e.dir, 30);
            dl_state(e.dir, &ds);
            printf("4 empty answer: state %d (%s) in %.1f s\n", se, ds.err, now_s() - te);
            assert(se == DL_FAILED && strstr(ds.err, "empty") && now_s() - te > 9.0);
            DlItem ei;
            assert(dl_info_read(&ei, e.dir) == 0 && !ei.done);
            /* a "finished" one with nothing (written so by 1.5.0) is not finished when the list is read */
            ei.done = 1; ei.err[0] = 0;
            assert(dl_info_write(&ei) == 0);
            DlItem lst[64];
            int nl = dl_scan(lst, 64);
            int seen = 0;
            for (int i = 0; i < nl; i++) if (!strcmp(lst[i].dir, e.dir)) { seen = 1; assert(!lst[i].done); }
            assert(seen);
            assert(dl_delete(e.dir) == 0);
        }
        /* 405 with no stream before: the server does not give it (failed at once, not asked three times) */
        DlItem g = mk(base, "/e405/film.mkv", "Vermez", 'F');
        double t0 = now_s();
        assert(dl_add(&g) == 0);
        st = wait_end(g.dir, 20);
        dl_state(g.dir, &ds);
        printf("4 405: state %d (%s) in %.1f s\n", st, ds.err, now_s() - t0);
        assert(st == DL_FAILED && strstr(ds.err, "405") && now_s() - t0 < 4.0);
        /* just after a stream: 405 is "the account is still counted": asked again (still running after 6 s) */
        dl_note_stream_end();
        assert(dl_resume(g.dir) == 0);
        usleep(6000000);
        int s2 = dl_state(g.dir, &ds);
        printf("4 405 after a stream: state %d after 6 s\n", s2);
        assert(s2 == DL_ACTIVE);
        assert(dl_delete(g.dir) == 0);
        /* 1.2.2: the account's connections all in use (its check said so): its server's 405 waits for a free one */
        dl_note_account_full("http://other.example:8080", 1);   /* (another server's account: no effect here) */
        DlItem g2 = mk(base, "/e405/film2.mkv", "Bekler", 'F');
        assert(dl_add(&g2) == 0);
        dl_note_account_full(base, 1);
        usleep(3000000);
        int s3 = dl_state(g2.dir, &ds);
        printf("4 405, account full: state %d, waiting %d after 3 s\n", s3, ds.netwait);
        assert(s3 == DL_ACTIVE && ds.netwait == 2);
        assert(dl_delete(g2.dir) == 0);
        dl_note_account_full(base, 0);
    }
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
    {
        char p[700]; snprintf(p, sizeof p, "%s/video.srt", e.dir);   /* (film2.srt is not there: no file) */
        assert(fsize(p) < 0);
    }

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
    /* 6. 1.5.2: the Vita asleep / no network: the drops are not counted, it waits and goes on afterwards */
    dl_set_net_check(net_up);
    dl_set_asleep(1);
    DlItem h = mk(base, "/drop/film2.mkv", "Uyku", 'F');
    assert(dl_add(&h) == 0);
    snprintf(vid, sizeof vid, "%s/%s", h.dir, h.video);
    {
        for (int k = 0; k < 200 && !(dl_state(h.dir, &ds) == DL_ACTIVE && ds.netwait); k++) usleep(20000);
        long long at = fsize(vid);
        usleep(4000000);                                     /* (ten drops' time and more: still waiting) */
        int s6 = dl_state(h.dir, &ds);
        printf("6 asleep: state %d, waiting %d, %lld -> %lld bytes\n", s6, ds.netwait, at, fsize(vid));
        assert(s6 == DL_ACTIVE && ds.netwait && fsize(vid) == at && at > 0 && at < whole);
        dl_set_asleep(0);                                    /* awake, but the Wi-Fi not yet */
        net_on = 0;
        usleep(1500000);
        assert(dl_state(h.dir, &ds) == DL_ACTIVE && ds.netwait && fsize(vid) == at);
        net_on = 1;
        st = wait_end(h.dir, 60);
        printf("6 network back: state %d, %lld bytes\n", st, fsize(vid));
        assert(st == DL_IDLE && same_file(vid, src));
    }
    dl_set_net_check(NULL);
    /* 7. 1.2.4: the order of the waiting ones; info.txt's "watched" */
    {
        DlItem q[4];
        static const char *const qp[4] = { "/slow/q0/film3.mkv", "/slow/q1/film3.mkv", "/slow/q2/film3.mkv", "/slow/q3/film3.mkv" };
        for (int k = 0; k < 4; k++) { char t[16]; snprintf(t, sizeof t, "Sira %d", k); q[k] = mk(base, qp[k], t, 'F'); assert(dl_add(&q[k]) == 0); }
        usleep(300000);
        assert(dl_state(q[0].dir, &ds) == DL_ACTIVE && dl_queue_pos(q[0].dir) == 0 && dl_queue_count() == 3);
        assert(dl_queue_pos(q[1].dir) == 1 && dl_queue_pos(q[2].dir) == 2 && dl_queue_pos(q[3].dir) == 3);
        assert(dl_queue_move(q[3].dir, 1) == 1);                                  /* the last one next */
        assert(dl_queue_pos(q[3].dir) == 1 && dl_queue_pos(q[1].dir) == 2 && dl_queue_pos(q[2].dir) == 3);
        assert(dl_queue_move(q[3].dir, 99) == 3);                                 /* and to the end again */
        assert(dl_queue_pos(q[1].dir) == 1 && dl_queue_pos(q[2].dir) == 2 && dl_queue_pos(q[3].dir) == 3);
        assert(dl_queue_move(q[1].dir, 2) == 2 && dl_queue_pos(q[2].dir) == 1);   /* one step down */
        assert(dl_queue_move(q[0].dir, 1) == -1);                                 /* (the active one is not in it) */
        printf("7 order: %d %d %d\n", dl_queue_pos(q[1].dir), dl_queue_pos(q[2].dir), dl_queue_pos(q[3].dir));
        dl_pause(q[0].dir);                                                       /* the next one is the one moved first */
        for (int k = 0; k < 100 && dl_state(q[2].dir, &ds) != DL_ACTIVE; k++) usleep(20000);
        assert(dl_state(q[2].dir, &ds) == DL_ACTIVE && dl_queue_pos(q[1].dir) == 1 && dl_queue_count() == 2);
        for (int k = 0; k < 4; k++) assert(dl_delete(q[k].dir) == 0);
        DlItem r;
        assert(dl_info_read(&r, h.dir) == 0 && r.done && !r.watched);
        assert(dl_set_watched(h.dir, 1) == 0 && dl_info_read(&r, h.dir) == 0 && r.watched && r.done);
        DlItem lst[16];
        int n = dl_scan(lst, 16), seen = 0;
        for (int i = 0; i < n; i++) if (!strcmp(lst[i].dir, h.dir)) { seen = 1; assert(lst[i].watched); }
        assert(seen);
        assert(dl_set_watched(h.dir, 0) == 0 && dl_info_read(&r, h.dir) == 0 && !r.watched);
        printf("7 watched: ok\n");
    }
    dl_shutdown();
    printf("all download tests passed\n");
    return 0;
}
