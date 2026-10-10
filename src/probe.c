#include "nettls.h"
#include "probe.h"
#include "tsdemux.h"
#include "mkvdemux.h"
#include "httpio.h"                     /* plog() */
#include "curlio.h"
#include "hls.h"
#include "vod.h"
#include <psp2/net/http.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PROBE_US    6000000ULL          /* how long to read */
#define PROBE_MAX   (6u * 1024 * 1024)  /* or how many bytes */
#define CHUNK       16384

typedef struct {
    TsDemux *ts;
    MkvDemux *mkv;
} Demux;

static void dmx_feed(Demux *d, const TsSink *sink, const uint8_t *b, size_t n, int first)
{
    if (first && mkv_probe(b, n)) d->mkv = mkv_create(sink);
    if (d->mkv) mkv_feed(d->mkv, b, n); else ts_feed(d->ts, b, n);
}

typedef struct {
    ProbeResult res;
    char url[512];
    volatile int cancel;
    int refs;                           /* UI + thread */
} ProbeCtx;

static ProbeCtx *g_cur;

static void ctx_release(ProbeCtx *c)
{
    if (__sync_sub_and_fetch(&c->refs, 1) == 0) free(c);
}

static void fail(ProbeResult *r, const char *msg)
{
    snprintf(r->error, sizeof r->error, "%s", msg);
    plog("probe: FAILED: %s", msg);
}

static const char *profile_name(int p)
{
    switch (p) {
    case 66: return "Baseline"; case 77: return "Main"; case 88: return "Extended";
    case 100: return "High"; case 110: return "High 10"; case 122: return "High 4:2:2";
    case 244: return "High 4:4:4"; default: return "?";
    }
}

static void add_line(ProbeResult *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add_line(ProbeResult *r, const char *fmt, ...)
{
    if (r->nlines >= PROBE_LINES) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->lines[r->nlines], sizeof r->lines[0], fmt, ap);
    va_end(ap);
    plog("probe: %s", r->lines[r->nlines]);
    r->nlines++;
}

static void summarize(ProbeResult *r, const TsInfo *i, int http_status, unsigned long long us)
{
    double secs = us / 1e6;
    if (http_status) add_line(r, "HTTP %d, %u KB in %.1f s (%.0f kbit/s)", http_status,
                              (unsigned)(r->bytes / 1024), secs, secs > 0 ? r->bytes * 8.0 / secs / 1000.0 : 0.0);
    else add_line(r, "%u KB read in %.1f s", (unsigned)(r->bytes / 1024), secs);

    char sbuf[100] = "";
    for (int k = 0; k < i->nstreams && strlen(sbuf) < 70; k++) {
        char one[32];
        snprintf(one, sizeof one, "%s0x%02X=%s", k ? ", " : "", i->streams[k].stream_type, ts_codec_name(i->streams[k].codec));
        strncat(sbuf, one, sizeof sbuf - strlen(sbuf) - 1);
    }
    add_line(r, "Streams: %s", sbuf);

    const char *verdict;
    if (i->video_pid < 0) {
        add_line(r, "Video: none");
        verdict = "No video stream found";
    } else if (i->video_codec == TS_CODEC_H264) {
        if (i->width == 0) {
            add_line(r, "Video: H.264 (video parameters not seen in this sample)");
            verdict = "Not enough data yet";
        } else {
            add_line(r, "Video: H.264 %s L%d.%d  %dx%d%s  %.2f fps  %d-bit",
                     profile_name(i->profile), i->level / 10, i->level % 10, i->width, i->height,
                     i->frame_mbs_only ? "" : " interlaced", ts_video_fps(i), i->bit_depth);
            add_line(r, "Keyframes: %u (+%u other I-pictures) of %u pictures, reference frames: %d", i->keyframes, i->intra_aus, i->video_aus, i->ref_frames);
            if (i->bit_depth != 8 || i->chroma_format != 1) verdict = "NO: Vita cannot decode this H.264 variant";
            else if (i->width > 1920 || i->height > 1088) verdict = "NO: above 1080p";
            else if ((i->width > 1280 || i->height > 720) && !i->frame_mbs_only) verdict = "MAYBE: 1080i (interlaced) may stutter";
            else verdict = i->width > 1280 || i->height > 720 ? "YES: H.264 8-bit 1080p" : "YES: H.264 8-bit up to 720p";
        }
    } else if (i->video_codec == TS_CODEC_HEVC) {
        add_line(r, "Video: HEVC (H.265), %u pictures", i->video_aus);
        verdict = "NO: Vita hardware cannot decode HEVC";
    } else {
        add_line(r, "Video: %s, %u pictures", ts_codec_name(i->video_codec), i->video_aus);
        verdict = "NO: this video format is not supported yet";
    }

    if (i->audio_pid < 0) add_line(r, "Audio: none");
    else if (i->audio_codec == TS_CODEC_AAC)
        add_line(r, "Audio: AAC%s %d Hz, %d ch, %u frames",
                 i->aac_object == 2 ? "-LC" : i->aac_object == 1 ? " Main" : "", i->aac_rate, i->aac_channels, i->audio_frames);
    else add_line(r, "Audio: %s (not supported yet), %u frames", ts_codec_name(i->audio_codec), i->audio_frames);

    add_line(r, "Packet problems: lost %u, damaged pictures %u, sync lost %u",
             i->cc_errors, i->damaged_aus, i->sync_losses);
    if (i->scrambled_packets) add_line(r, "SCRAMBLED: %u packets are encrypted", i->scrambled_packets);
    add_line(r, "Playable by hardware: %s", verdict);
}

/* ---- one GET: http through sceHttp, https through the built-in TLS; optionally a byte range ---- */
typedef int (*PData)(void *ctx, const uint8_t *d, size_t n);       /* non-zero: stop */
typedef struct { ProbeCtx *c; PData fn; void *ctx; } PgCtx;
static int pg_data(void *x, const uint8_t *d, size_t n) { PgCtx *g = x; return g->c->cancel || g->fn(g->ctx, d, n); }
static int pg_stop(void *x) { return ((PgCtx *)x)->c->cancel; }

/* Returns 0 when the body ended or fn stopped it, < 0 on a network error (err says why). */
static int pget(ProbeCtx *c, const char *url, long long from, long long to, PData fn, void *ctx, int *status, char *err, size_t errsz)
{
    *status = 0;
    err[0] = 0;
    if (cio_available() && !strncasecmp(url, "https:", 6)) {
        PgCtx g = { c, fn, ctx };
        CioRequest rq = { url, NULL, NULL, 10, 5, pg_data, pg_stop, &g, from >= 0, from >= 0 ? (uint64_t)from : 0,
                          to >= 0 ? (uint64_t)to : 0, NULL };
        int r = cio_get(&rq, status, err, errsz);
        return r == -2 ? 0 : r;
    }
    int tpl = -1, conn = -1, req = -1, rc = -1, n;
    uint8_t *buf = malloc(CHUNK);
    if (!buf) { snprintf(err, errsz, "Out of memory"); return -1; }
    tpl = sceHttpCreateTemplate(NET_UA, SCE_HTTP_VERSION_1_1, 1);
    if (tpl < 0) { snprintf(err, errsz, "HTTP init failed"); goto done; }
    sceHttpSetResolveTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetConnectTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetRecvTimeOut(tpl, 5 * 1000 * 1000);
    sceHttpSetAutoRedirect(tpl, 1);
    net_tls_relax(tpl);
    conn = sceHttpCreateConnectionWithURL(tpl, url, 1);
    if (conn < 0) { snprintf(err, errsz, "Bad address"); goto done; }
    req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, url, 0);
    if (req < 0) { snprintf(err, errsz, "Request failed"); goto done; }
    if (from >= 0) {
        char range[64];
        if (to >= 0) snprintf(range, sizeof range, "bytes=%lld-%lld", from, to);
        else snprintf(range, sizeof range, "bytes=%lld-", from);
        sceHttpAddRequestHeader(req, "Range", range, SCE_HTTP_HEADER_ADD);
    }
    n = sceHttpSendRequest(req, NULL, 0);
    if (n < 0) { snprintf(err, errsz, "Connection failed (0x%08X)", (unsigned)n); goto done; }
    sceHttpGetStatusCode(req, status);
    rc = 0;
    if (*status >= 400) goto done;
    while (!c->cancel) {
        n = sceHttpReadData(req, buf, CHUNK);
        if (n < 0) { snprintf(err, errsz, "Read error (0x%08X)", (unsigned)n); rc = n; break; }
        if (n == 0 || fn(ctx, buf, (size_t)n)) break;
    }
done:
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tpl >= 0) sceHttpDeleteTemplate(tpl);
    free(buf);
    return rc;
}

/* ---- a stream (TS or MKV) into the demuxer; what the first bytes are decides the rest ---- */
enum { SNIFF_STREAM, SNIFF_PLAYLIST, SNIFF_MP4, SNIFF_WEB };
typedef struct { ProbeCtx *c; Demux *d; uint32_t got; unsigned long long t0; int sniff, first; } StCtx;

static int st_data(void *x, const uint8_t *b, size_t n)
{
    StCtx *s = x;
    if (s->first) {
        s->first = 0;
        plog("probe: first bytes %02X %02X %02X %02X %02X %02X %02X %02X", b[0], n > 1 ? b[1] : 0, n > 2 ? b[2] : 0,
             n > 3 ? b[3] : 0, n > 4 ? b[4] : 0, n > 5 ? b[5] : 0, n > 6 ? b[6] : 0, n > 7 ? b[7] : 0);
        if (s->c->res.bytes == 0) {                         /* only the first thing fetched can be a playlist */
            if (hls_is_playlist((const char *)b, n)) { s->sniff = SNIFF_PLAYLIST; return 1; }
            if (vod_container(b, n) == VOD_MP4) { s->sniff = SNIFF_MP4; return 1; }
            if (b[0] == '<') { s->sniff = SNIFF_WEB; return 1; }
        }
    }
    TsSink sk = { NULL, NULL, NULL };
    dmx_feed(s->d, &sk, b, n, s->c->res.bytes == 0);
    s->got += (uint32_t)n;
    s->c->res.bytes += (uint32_t)n;
    return s->c->res.bytes >= PROBE_MAX || sceKernelGetProcessTimeWide() - s->t0 >= PROBE_US;
}

static int probe_stream(ProbeCtx *c, const char *url, Demux *d, int *status, unsigned long long t0, int *sniff)
{
    StCtx s = { c, d, 0, t0, SNIFF_STREAM, 1 };
    char err[100];
    int r = pget(c, url, -1, -1, st_data, &s, status, err, sizeof err);
    *sniff = s.sniff;
    plog("probe: HTTP status %d, %u KB", *status, (unsigned)(s.got / 1024));
    if (*status >= 400) { char m[64]; snprintf(m, sizeof m, "Server answered HTTP %d", *status); fail(&c->res, m); return -1; }
    if (s.sniff == SNIFF_WEB) { fail(&c->res, "Server sent a web page, not video"); return -1; }
    if (r < 0 && s.got == 0 && s.sniff == SNIFF_STREAM) { fail(&c->res, err[0] ? err : "Connection failed"); return -1; }
    return 0;
}

/* ---- whole small files (playlists) and byte ranges (an MP4's index) ---- */
typedef struct { uint8_t *out; size_t cap, n; } Buf;
static int buf_data(void *x, const uint8_t *d, size_t n)
{
    Buf *b = x;
    size_t k = b->cap - b->n < n ? b->cap - b->n : n;
    memcpy(b->out + b->n, d, k);
    b->n += k;
    return b->n >= b->cap;
}

static int fetch_text(ProbeCtx *c, const char *url, char *out, size_t cap, int *status)
{
    Buf b = { (uint8_t *)out, cap - 1, 0 };
    char err[100];
    int r = pget(c, url, -1, -1, buf_data, &b, status, err, sizeof err);
    out[b.n] = 0;
    if (*status >= 400) { char m[64]; snprintf(m, sizeof m, "Server answered HTTP %d", *status); fail(&c->res, m); return -1; }
    if (r < 0 && !b.n) { fail(&c->res, err[0] ? err : "Connection failed"); return -1; }
    return (int)b.n;
}

typedef struct { ProbeCtx *c; const char *url; } RangeCtx;
static int range_read(void *x, uint64_t off, uint8_t *out, size_t len)
{
    RangeCtx *rc = x;
    Buf b = { out, len, 0 };
    int status;
    char err[100];
    int r = pget(rc->c, rc->url, (long long)off, (long long)(off + len - 1), buf_data, &b, &status, err, sizeof err);
    if (status >= 400 || (r < 0 && !b.n)) return -1;
    if (status == 200 && off > 0) return -1;                 /* the server ignores ranges */
    rc->c->res.bytes += (uint32_t)b.n;
    return (int)b.n;
}

/* ---- HLS: the variant the player would choose, then a few of its segments ---- */
static int probe_hls(ProbeCtx *c, const char *url, Demux *d, int *status, unsigned long long t0)
{
    char *text = malloc(512 * 1024), cur[HLS_URL_MAX], seg[HLS_URL_MAX];
    HlsVariant *v = malloc(sizeof *v * 16);
    int rc = -1;
    if (!text || !v) { fail(&c->res, "Out of memory"); goto out; }
    snprintf(cur, sizeof cur, "%s", url);
    if (fetch_text(c, cur, text, 512 * 1024, status) < 0) goto out;
    if (hls_is_master(text)) {
        int n = hls_parse_master(text, cur, v, 16);
        int k = hls_pick_variant(v, n, 720);
        if (k < 0) { fail(&c->res, "HLS playlist without a usable stream"); goto out; }
        add_line(&c->res, "HLS: %d variants, analysing %dx%d %ld kbit/s", n, v[k].width, v[k].height, v[k].bandwidth / 1000);
        snprintf(cur, sizeof cur, "%s", v[k].uri);
        if (fetch_text(c, cur, text, 512 * 1024, status) < 0) goto out;
    }
    HlsMedia m;
    if (hls_parse_media(text, &m) < 0 || !m.nseg) { hls_media_free(&m); fail(&c->res, "HLS playlist without segments"); goto out; }
    if (m.encrypted) { hls_media_free(&m); fail(&c->res, "Encrypted HLS stream (AES): not supported"); goto out; }
    if (m.fmp4) { hls_media_free(&m); fail(&c->res, "HLS with MP4 segments: not supported"); goto out; }
    int first = m.endlist || m.nseg <= 3 ? 0 : m.nseg - 3;     /* live: near the newest, like the player */
    for (int i = first; i < m.nseg && !c->cancel; i++) {
        hls_resolve(cur, m.seg[i].uri, m.seg[i].uri_len, seg, sizeof seg);
        int sn;
        if (probe_stream(c, seg, d, status, t0, &sn) < 0) break;
        rc = 0;
        if (c->res.bytes >= PROBE_MAX || sceKernelGetProcessTimeWide() - t0 >= PROBE_US) break;
    }
    hls_media_free(&m);
out:
    free(text);
    free(v);
    return rc;
}

/* ---- MP4 files (films): the index, then the start of the film ---- */
static Mp4Demux *probe_mp4(ProbeCtx *c, const char *url)
{
    RangeCtx rc = { c, url };
    uint8_t *moov = NULL;
    size_t ml = 0;
    int r = mp4_read_moov(range_read, &rc, 0, &moov, &ml);
    if (r != 0) { fail(&c->res, r == -2 ? "MP4 index too big" : "MP4: index not readable (the server may not send parts of files)"); return NULL; }
    TsSink sk = { NULL, NULL, NULL };
    char err[96];
    Mp4Demux *m = mp4_create(moov, ml, &sk, err, sizeof err);
    free(moov);
    if (!m) { fail(&c->res, err); return NULL; }
    uint64_t off = mp4_seek(m, 0, NULL);                    /* a little of the film, to count pictures */
    uint8_t *b = malloc(CHUNK);
    for (int k = 0; b && k < 64 && !c->cancel; k++) {
        int n = range_read(&rc, off, b, CHUNK);
        if (n <= 0) break;
        mp4_feed(m, off, b, (size_t)n);
        off += (uint64_t)n;
    }
    free(b);
    return m;
}

static int read_local(ProbeCtx *c, Demux *d, uint8_t *buf, unsigned long long t0)
{
    (void)t0;
    FILE *f = fopen(c->url, "rb");
    if (!f) { fail(&c->res, "Cannot open file"); return -1; }
    uint32_t total = 0;
    size_t n;
    while (!c->cancel && (n = fread(buf, 1, CHUNK, f)) > 0) {
        { TsSink sk = { NULL, NULL, NULL }; dmx_feed(d, &sk, buf, n, total == 0); }
        total += (uint32_t)n;
        c->res.bytes = total;
        if (total >= PROBE_MAX) break;
    }
    fclose(f);
    return 0;
}

static int probe_main(SceSize args, void *argp)
{
    (void)args; (void)argp;
    ProbeCtx *c = *(ProbeCtx **)argp;
    ProbeResult *r = &c->res;
    TsSink sink = { NULL, NULL, NULL };
    Demux dm = { ts_create(&sink), NULL }, *d = &dm;
    uint8_t *buf = malloc(CHUNK);
    unsigned long long t0 = sceKernelGetProcessTimeWide();
    int status = 0, ok = 0;

    plog("probe: start");
    if (!d->ts || !buf) {
        fail(r, "Out of memory");
    } else {
        int http = !strncasecmp(c->url, "http://", 7) || !strncasecmp(c->url, "https://", 8);
        Mp4Demux *mp4 = NULL;
        if (http) {
            int sniff = SNIFF_STREAM;
            ok = probe_stream(c, c->url, d, &status, t0, &sniff) == 0;
            if (ok && sniff == SNIFF_PLAYLIST) ok = probe_hls(c, c->url, d, &status, t0) == 0;
            else if (ok && sniff == SNIFF_MP4) { mp4 = probe_mp4(c, c->url); ok = mp4 != NULL; if (ok) plog("probe: MP4 file"); }
        } else {
            ok = read_local(c, d, buf, t0) == 0;
        }
        if (ok && !c->cancel) {
            if (d->mkv) mkv_flush(d->mkv); else if (!mp4) ts_flush(d->ts);
            const TsInfo *info = mp4 ? mp4_info(mp4) : d->mkv ? mkv_info(d->mkv) : ts_info(d->ts);
            if (d->mkv) plog("probe: Matroska (MKV) container");
            if (r->bytes == 0) { fail(r, "No data received"); ok = 0; }
            else if (info->program < 0) { fail(r, "No video stream recognised (not TS, MKV or MP4?)"); ok = 0; }
            else summarize(r, info, status, sceKernelGetProcessTimeWide() - t0);
        }
        mp4_destroy(mp4);
    }
    if (d->ts) ts_destroy(d->ts);
    mkv_destroy(d->mkv);
    free(buf);
    __sync_synchronize();
    r->state = ok && !c->cancel ? PROBE_DONE : PROBE_FAILED;
    plog("probe: end");
    ctx_release(c);
    sceKernelExitDeleteThread(0);
    return 0;
}

int probe_start(const char *url)
{
    probe_cancel();
    ProbeCtx *c = calloc(1, sizeof *c);
    if (!c) return -1;
    snprintf(c->url, sizeof c->url, "%s", url);
    c->res.state = PROBE_RUNNING;
    c->refs = 2;
    g_cur = c;
    SceUID th = sceKernelCreateThread("iptv_probe", probe_main, 0x10000100, 0x40000, 0, 0, NULL);
    if (th < 0) { g_cur = NULL; free(c); return th; }
    sceKernelStartThread(th, sizeof c, &c);
    return 0;
}

void probe_cancel(void)
{
    if (!g_cur) return;
    g_cur->cancel = 1;
    ctx_release(g_cur);
    g_cur = NULL;
}

const ProbeResult *probe_result(void)
{
    return g_cur ? &g_cur->res : NULL;
}
