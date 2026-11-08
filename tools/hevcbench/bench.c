/* See bench.h. The file is cut into packets with FFmpeg's parser before the clock starts, so only the
 * decoding is timed (no file reading, no parsing, no drawing). */
#include "bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>

#define MAX_FILE (48 * 1024 * 1024)

typedef struct { uint8_t *data; int size; } Pkt;
struct BenchFile {
    enum AVCodecID id;
    Pkt *pkts;
    int n;
};

static enum AVCodecID codec_of(const char *path)
{
    const char *e = strrchr(path, '.');
    if (!e) return AV_CODEC_ID_NONE;
    if (!strcasecmp(e, ".hevc") || !strcasecmp(e, ".h265") || !strcasecmp(e, ".265")) return AV_CODEC_ID_HEVC;
    if (!strcasecmp(e, ".h264") || !strcasecmp(e, ".264")) return AV_CODEC_ID_H264;
    return AV_CODEC_ID_NONE;
}

BenchFile *bench_load(const char *path, char *err, int errcap)
{
    enum AVCodecID id = codec_of(path);
    if (id == AV_CODEC_ID_NONE) { snprintf(err, errcap, "not a .hevc/.h265/.h264 file"); return NULL; }
    FILE *fp = fopen(path, "rb");
    if (!fp) { snprintf(err, errcap, "cannot open"); return NULL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > MAX_FILE) { fclose(fp); snprintf(err, errcap, "size %ld (at most %d MB)", sz, MAX_FILE >> 20); return NULL; }
    uint8_t *buf = malloc((size_t)sz + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!buf) { fclose(fp); snprintf(err, errcap, "out of memory (%ld KB)", sz >> 10); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    memset(buf + got, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    BenchFile *f = calloc(1, sizeof *f);
    AVCodecParserContext *p = av_parser_init(id);
    const AVCodec *c = avcodec_find_decoder(id);
    AVCodecContext *pc = c ? avcodec_alloc_context3(c) : NULL;
    int cap = 0;
    if (!f || !p || !pc) { snprintf(err, errcap, "FFmpeg has no %s decoder/parser", avcodec_get_name(id)); goto fail; }
    f->id = id;
    for (size_t pos = 0;;) {
        uint8_t *out = NULL;
        int outn = 0;
        int used = av_parser_parse2(p, pc, &out, &outn, pos < got ? buf + pos : NULL, pos < got ? (int)(got - pos) : 0,
                                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) break;
        pos += (size_t)used;
        if (outn > 0) {
            if (f->n == cap) {
                cap = cap ? cap * 2 : 1024;
                Pkt *np = realloc(f->pkts, sizeof *np * (size_t)cap);
                if (!np) { snprintf(err, errcap, "out of memory (packets)"); goto fail; }
                f->pkts = np;
            }
            uint8_t *d = malloc((size_t)outn + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!d) { snprintf(err, errcap, "out of memory (packet)"); goto fail; }
            memcpy(d, out, (size_t)outn);
            memset(d + outn, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            f->pkts[f->n].data = d;
            f->pkts[f->n].size = outn;
            f->n++;
        } else if (pos >= got) break;                    /* (the parser gave its last packet) */
    }
    av_parser_close(p);
    avcodec_free_context(&pc);
    free(buf);
    if (!f->n) { snprintf(err, errcap, "no pictures found"); bench_free(f); return NULL; }
    return f;
fail:
    if (p) av_parser_close(p);
    avcodec_free_context(&pc);
    free(buf);
    bench_free(f);
    return NULL;
}

void bench_free(BenchFile *f)
{
    if (!f) return;
    for (int i = 0; i < f->n; i++) free(f->pkts[i].data);
    free(f->pkts);
    free(f);
}

int bench_packets(const BenchFile *f) { return f ? f->n : 0; }

static void got_frame(BenchResult *r, const AVFrame *fr, int64_t now, int64_t *last)
{
    if (!r->frames) {
        r->width = fr->width;
        r->height = fr->height;
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get((enum AVPixelFormat)fr->format);
        r->bit_depth = d ? d->comp[0].depth : 0;
    }
    /* (the first pictures of frame threading come in a burst after a delay: not counted as waits) */
    if (r->frames >= 8 && now - *last > r->worst_us) r->worst_us = now - *last;
    *last = now;
    r->frames++;
}

void bench_run(const BenchFile *f, const BenchCfg *cfg, BenchClock clk, BenchResult *r)
{
    memset(r, 0, sizeof *r);
    const AVCodec *c = avcodec_find_decoder(f->id);
    snprintf(r->codec, sizeof r->codec, "%s", f->id == AV_CODEC_ID_HEVC ? "H.265" : "H.264");
    AVCodecContext *ctx = c ? avcodec_alloc_context3(c) : NULL;
    AVFrame *fr = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    if (!ctx || !fr || !pkt) { snprintf(r->err, sizeof r->err, "out of memory"); goto done; }
    ctx->thread_count = cfg->threads > 0 ? cfg->threads : 1;
    ctx->thread_type = cfg->thread_type == BENCH_SLICE_THREADS ? FF_THREAD_SLICE : FF_THREAD_FRAME;
    if (cfg->fast) {
        ctx->skip_loop_filter = AVDISCARD_ALL;
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    }
    int rc = avcodec_open2(ctx, c, NULL);
    if (rc < 0) { snprintf(r->err, sizeof r->err, "avcodec_open2 %d", rc); goto done; }
    int64_t t0 = clk(), last = t0, now = t0;
    int sent = 0, stop = 0;
    while (!stop) {
        if (sent < f->n) {
            pkt->data = f->pkts[sent].data;
            pkt->size = f->pkts[sent].size;
            rc = avcodec_send_packet(ctx, pkt);
            if (rc == AVERROR(EAGAIN)) rc = 0;           /* (taken below, then sent again) */
            else { sent++; if (rc < 0 && rc != AVERROR_INVALIDDATA) { snprintf(r->err, sizeof r->err, "send_packet %d", rc); break; } }
        } else if (sent == f->n) {
            avcodec_send_packet(ctx, NULL);                /* the end: the pictures still inside come out */
            sent++;
        }
        for (;;) {
            rc = avcodec_receive_frame(ctx, fr);
            if (rc < 0) break;
            now = clk();
            got_frame(r, fr, now, &last);
            av_frame_unref(fr);
            if (r->frames >= cfg->max_frames || now - t0 >= (int64_t)cfg->max_ms * 1000) { stop = 1; break; }
        }
        if (rc == AVERROR_EOF) break;
        if (sent > f->n && rc < 0) break;                 /* (after the end: nothing more will come) */
    }
    r->us = now - t0;
    r->fps = r->us > 0 ? r->frames * 1e6 / (double)r->us : 0;
    if (ctx->framerate.num > 0 && ctx->framerate.den > 0) r->file_fps = av_q2d(ctx->framerate);
    r->ok = r->frames > 0;
    if (!r->ok && !r->err[0]) snprintf(r->err, sizeof r->err, "no picture decoded");
done:
    av_packet_free(&pkt);
    av_frame_free(&fr);
    avcodec_free_context(&ctx);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    const char *t = strrchr(p, ':');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : p;
}

void bench_all(const char *const *paths, int n, BenchClock clk, int max_threads, void (*out)(const char *line),
               void (*between)(void))
{
    char line[256];
    for (int i = 0; i < n; i++) {
        char err[96] = "";
        snprintf(line, sizeof line, "now: loading %s", base_name(paths[i]));
        out(line);
        if (between) between();
        BenchFile *f = bench_load(paths[i], err, sizeof err);
        if (!f) {
            snprintf(line, sizeof line, "%s: %s", base_name(paths[i]), err);
            out(line);
            continue;
        }
        /* the configurations: 1, 2, 3 (4) frame threads, and the most with the filters left out */
        BenchCfg cfgs[8];
        int nc = 0;
        for (int t = 1; t <= max_threads; t++) cfgs[nc++] = (BenchCfg){ t, BENCH_FRAME_THREADS, 0, 150, 25000 };
        /* (no slice threads: FFmpeg 7.1's H.265 wavefront threading locked up on the 10-bit test file on a PC) */
        cfgs[nc++] = (BenchCfg){ max_threads, BENCH_FRAME_THREADS, 1, 150, 25000 };
        int header = 0;
        for (int k = 0; k < nc; k++) {
            snprintf(line, sizeof line, "now: %s - %d %s thread%s%s", base_name(paths[i]), cfgs[k].threads,
                     cfgs[k].thread_type == BENCH_SLICE_THREADS ? "slice" : "frame", cfgs[k].threads > 1 ? "s" : "",
                     cfgs[k].fast ? ", fast" : "");
            out(line);
            if (between) between();
            BenchResult r;
            bench_run(f, &cfgs[k], clk, &r);
            if (!header && r.ok) {
                header = 1;
                snprintf(line, sizeof line, "%s: %s %dx%d %d-bit, %.0f fps stream, %d packets", base_name(paths[i]), r.codec,
                         r.width, r.height, r.bit_depth, r.file_fps, bench_packets(f));
                out(line);
            }
            if (!r.ok) snprintf(line, sizeof line, "  %d %s%s: FAILED (%s)", cfgs[k].threads,
                                cfgs[k].thread_type == BENCH_SLICE_THREADS ? "slice" : "frame", cfgs[k].fast ? " fast" : "", r.err);
            else {
                double need = r.file_fps > 0 ? r.file_fps : 25;
                snprintf(line, sizeof line, "  %d %s%-5s %6.1f fps  (%d pictures in %.2f s, slowest wait %d ms)  %s",
                         cfgs[k].threads, cfgs[k].thread_type == BENCH_SLICE_THREADS ? "slice" : "frame",
                         cfgs[k].fast ? " fast" : "", r.fps, r.frames, r.us / 1e6, (int)(r.worst_us / 1000),
                         r.fps >= need * 1.25 ? "REAL TIME" : r.fps >= need ? "just real time" : "too slow");
            }
            out(line);
        }
        bench_free(f);
    }
    out("done");
}
