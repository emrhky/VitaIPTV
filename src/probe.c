#include "probe.h"
#include "tsdemux.h"
#include "httpio.h"                     /* plog() */
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
            else if (i->width > 1280 || i->height > 720) verdict = "MAYBE: above 720p, hardware limit unknown";
            else verdict = "YES: H.264 8-bit up to 720p";
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

/* Reads data and feeds the demuxer. Returns 0 if data was read, -1 with res->error set. */
static int read_http(ProbeCtx *c, TsDemux *d, uint8_t *buf, int *status_out, unsigned long long t0)
{
    ProbeResult *r = &c->res;
    int tpl = -1, conn = -1, req = -1, rc = -1, status = 0, n, first = 1;
    uint32_t total = 0;

    tpl = sceHttpCreateTemplate("VitaIPTV/1.0", SCE_HTTP_VERSION_1_1, 1);
    if (tpl < 0) { fail(r, "HTTP init failed"); goto done; }
    sceHttpSetResolveTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetConnectTimeOut(tpl, 10 * 1000 * 1000);
    sceHttpSetRecvTimeOut(tpl, 3 * 1000 * 1000);
    sceHttpSetAutoRedirect(tpl, 1);
    conn = sceHttpCreateConnectionWithURL(tpl, c->url, 1);
    if (conn < 0) { fail(r, "Bad address"); goto done; }
    req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, c->url, 0);
    if (req < 0) { fail(r, "Request failed"); goto done; }
    n = sceHttpSendRequest(req, NULL, 0);
    if (n < 0) { char m[64]; snprintf(m, sizeof m, "Connection failed (0x%08X)", (unsigned)n); fail(r, m); goto done; }
    sceHttpGetStatusCode(req, &status);
    *status_out = status;
    plog("probe: HTTP status %d", status);
    if (status >= 400) { char m[64]; snprintf(m, sizeof m, "Server answered HTTP %d", status); fail(r, m); goto done; }

    while (!c->cancel) {
        n = sceHttpReadData(req, buf, CHUNK);
        if (n < 0) { if (total == 0) { fail(r, "Read error"); goto done; } break; }
        if (n == 0) break;
        if (first) {
            first = 0;
            plog("probe: first bytes %02X %02X %02X %02X %02X %02X %02X %02X", buf[0], buf[1], buf[2], buf[3],
                 n > 4 ? buf[4] : 0, n > 5 ? buf[5] : 0, n > 6 ? buf[6] : 0, n > 7 ? buf[7] : 0);
            if (n >= 7 && !memcmp(buf, "#EXTM3U", 7)) { fail(r, "HLS playlist (.m3u8): not supported yet"); goto done; }
            if (buf[0] == '<') { fail(r, "Server sent a web page, not video"); goto done; }
        }
        ts_feed(d, buf, (size_t)n);
        total += (uint32_t)n;
        r->bytes = total;
        if (total >= PROBE_MAX || sceKernelGetProcessTimeWide() - t0 >= PROBE_US) break;
    }
    rc = 0;
done:
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tpl >= 0) sceHttpDeleteTemplate(tpl);
    return rc;
}

static int read_local(ProbeCtx *c, TsDemux *d, uint8_t *buf, unsigned long long t0)
{
    (void)t0;
    FILE *f = fopen(c->url, "rb");
    if (!f) { fail(&c->res, "Cannot open file"); return -1; }
    uint32_t total = 0;
    size_t n;
    while (!c->cancel && (n = fread(buf, 1, CHUNK, f)) > 0) {
        ts_feed(d, buf, n);
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
    TsDemux *d = ts_create(&sink);
    uint8_t *buf = malloc(CHUNK);
    unsigned long long t0 = sceKernelGetProcessTimeWide();
    int status = 0, ok = 0;

    plog("probe: start");
    if (!d || !buf) {
        fail(r, "Out of memory");
    } else {
        int http = !strncasecmp(c->url, "http://", 7) || !strncasecmp(c->url, "https://", 8);
        ok = (http ? read_http(c, d, buf, &status, t0) : read_local(c, d, buf, t0)) == 0;
        if (ok && !c->cancel) {
            ts_flush(d);
            const TsInfo *info = ts_info(d);
            if (r->bytes == 0) { fail(r, "No data received"); ok = 0; }
            else if (info->program < 0) { fail(r, "No MPEG-TS data found (not a TS stream?)"); ok = 0; }
            else summarize(r, info, status, sceKernelGetProcessTimeWide() - t0);
        }
    }
    if (d) ts_destroy(d);
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
    SceUID th = sceKernelCreateThread("iptv_probe", probe_main, 0x10000100, 0x20000, 0, 0, NULL);
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
