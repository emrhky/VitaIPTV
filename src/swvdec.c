/* 1.2.2b: MPEG-2 / MPEG-4 Part 2 / DivX 3 video in software (see swvdec.h) */
#include "swvdec.h"
#include "tsdemux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *swv_name(int codec)
{
    return codec == TS_CODEC_MPEG2V ? "MPEG-2" : codec == TS_CODEC_MPEG4V ? "MPEG-4 (DivX / Xvid)" :
           codec == TS_CODEC_DIVX3 ? "DivX 3" : "?";
}

#ifdef HAVE_A52
#include <libavcodec/avcodec.h>


struct SwVdec {
    AVCodecContext *ctx;
    const AVCodec *codec;
    int codec_kind;
    AVCodecParserContext *parser;
    AVPacket *pkt;
    AVFrame *frame;
    uint32_t errors;
    int skip;
};

static enum AVCodecID codec_id(int codec)
{
    return codec == TS_CODEC_MPEG2V ? AV_CODEC_ID_MPEG2VIDEO : codec == TS_CODEC_MPEG4V ? AV_CODEC_ID_MPEG4 :
           codec == TS_CODEC_DIVX3 ? AV_CODEC_ID_MSMPEG4V3 : AV_CODEC_ID_NONE;
}

int swv_available(int codec)
{
    enum AVCodecID id = codec_id(codec);
    return id != AV_CODEC_ID_NONE && avcodec_find_decoder(id) != NULL;
}

SwVdec *swv_create(int codec, const uint8_t *extra, size_t elen, int w, int h, int parse, char *err, size_t errsz)
{
    const AVCodec *c = avcodec_find_decoder(codec_id(codec));
    if (!c) { snprintf(err, errsz, "no %s decoder in this build", swv_name(codec)); return NULL; }
    SwVdec *d = calloc(1, sizeof *d);
    if (!d) { snprintf(err, errsz, "out of memory"); return NULL; }
    d->ctx = avcodec_alloc_context3(c);
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (parse && codec != TS_CODEC_DIVX3) d->parser = av_parser_init(c->id);
    if (!d->ctx || !d->pkt || !d->frame || (parse && codec != TS_CODEC_DIVX3 && !d->parser)) {
        snprintf(err, errsz, "out of memory");
        swv_destroy(d);
        return NULL;
    }
    d->ctx->thread_count = 1;
    d->ctx->pkt_timebase = (AVRational){ 1, 90000 };
    d->ctx->width = w;
    d->ctx->height = h;
    d->ctx->err_recognition = 0;
    d->ctx->flags2 |= AV_CODEC_FLAG2_FAST;                  /* (MPEG-2: no exact IDCT rounding; the same picture to the eye) */
    if (extra && elen && elen < 4096) {
        d->ctx->extradata = av_mallocz(elen + AV_INPUT_BUFFER_PADDING_SIZE);
        if (d->ctx->extradata) { memcpy(d->ctx->extradata, extra, elen); d->ctx->extradata_size = (int)elen; }
    }
    d->codec = c;
    d->codec_kind = codec;
    if (avcodec_open2(d->ctx, c, NULL) < 0) { snprintf(err, errsz, "the %s decoder did not open", swv_name(codec)); swv_destroy(d); return NULL; }
    return d;
}

int swv_threads(SwVdec *d, int n)
{
    /* (an FFmpeg built without threads takes the count and decodes on one: nothing breaks) */
    if (!d || n <= 1) return 1;
    int slice = d->codec_kind == TS_CODEC_MPEG2V;
    if (slice ? !(d->codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) : !(d->codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)) return 1;
    /* threads are set before opening: open it again with them */
    AVCodecContext *nc = avcodec_alloc_context3(d->codec);
    if (!nc) return 1;
    nc->thread_count = n;
    nc->thread_type = slice ? FF_THREAD_SLICE : FF_THREAD_FRAME;
    nc->pkt_timebase = d->ctx->pkt_timebase;
    nc->width = d->ctx->width;
    nc->height = d->ctx->height;
    nc->flags2 = d->ctx->flags2;
    if (d->ctx->extradata_size > 0) {
        nc->extradata = av_mallocz((size_t)d->ctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (nc->extradata) { memcpy(nc->extradata, d->ctx->extradata, (size_t)d->ctx->extradata_size); nc->extradata_size = d->ctx->extradata_size; }
    }
    if (avcodec_open2(nc, d->codec, NULL) < 0) { avcodec_free_context(&nc); return 1; }
    avcodec_free_context(&d->ctx);
    d->ctx = nc;
    if (d->skip) d->ctx->skip_frame = d->skip >= 2 ? AVDISCARD_NONKEY : AVDISCARD_NONREF;
    return nc->active_thread_type ? nc->thread_count : 1;
}

void swv_destroy(SwVdec *d)
{
    if (!d) return;
    if (d->parser) av_parser_close(d->parser);
    avcodec_free_context(&d->ctx);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    free(d);
}

static int receive(SwVdec *d, SwvOut out, void *ctx)
{
    int n = 0;
    while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
        AVFrame *f = d->frame;
        if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) { d->errors++; av_frame_unref(f); continue; }
        if (f->decode_error_flags) d->errors++;
        SwvFrame o;
        o.y = f->data[0]; o.u = f->data[1]; o.v = f->data[2];
        o.ys = f->linesize[0]; o.us = f->linesize[1]; o.vs = f->linesize[2];
        o.w = f->width; o.h = f->height;
        int64_t ts = f->best_effort_timestamp;
        o.pts = ts == AV_NOPTS_VALUE ? -1 : ts & ((1LL << 33) - 1);
#if LIBAVUTIL_VERSION_MAJOR >= 58 && defined(AV_FRAME_FLAG_INTERLACED)
        o.interlaced = (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;
        o.top_first = (f->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;
        o.key = (f->flags & AV_FRAME_FLAG_KEY) != 0;
#else
        o.interlaced = f->interlaced_frame;
        o.top_first = f->top_field_first;
        o.key = f->key_frame;
#endif
        out(ctx, &o);
        n++;
        av_frame_unref(f);
    }
    return n;
}

static int send(SwVdec *d, const uint8_t *data, int len, int64_t pts, int64_t dts, SwvOut out, void *ctx)
{
    AVPacket *p = d->pkt;
    p->data = (uint8_t *)data;
    p->size = len;
    p->pts = pts >= 0 ? pts : AV_NOPTS_VALUE;
    p->dts = dts >= 0 ? dts : AV_NOPTS_VALUE;
    int n = 0;
    for (int k = 0; k < 4; k++) {                           /* (full: take the pictures out, then again) */
        int r = avcodec_send_packet(d->ctx, data ? p : NULL);
        if (r == AVERROR(EAGAIN)) { n += receive(d, out, ctx); continue; }
        if (r < 0 && r != AVERROR_EOF) d->errors++;
        break;
    }
    return n + receive(d, out, ctx);
}

int swv_decode(SwVdec *d, const uint8_t *data, size_t len, int64_t pts, int64_t dts, SwvOut out, void *ctx)
{
    if (!d || !len) return 0;
    if (!d->parser) return send(d, data, (int)len, pts, dts, out, ctx);
    int n = 0;
    while (len > 0) {
        uint8_t *o = NULL;
        int on = 0;
        int used = av_parser_parse2(d->parser, d->ctx, &o, &on, data, (int)len,
                                    pts >= 0 ? pts : AV_NOPTS_VALUE, dts >= 0 ? dts : AV_NOPTS_VALUE, 0);
        if (used < 0) { d->errors++; break; }
        data += used;
        len -= (size_t)used;
        pts = dts = -1;                                     /* (the parser keeps them for the picture they start) */
        if (on > 0) {
            int64_t pp = d->parser->pts, pd = d->parser->dts;
            n += send(d, o, on, pp == AV_NOPTS_VALUE ? -1 : pp, pd == AV_NOPTS_VALUE ? -1 : pd, out, ctx);
        }
        if (!used && !on) break;
    }
    return n;
}

void swv_flush(SwVdec *d, SwvOut out, void *ctx)
{
    if (!d) return;
    if (d->parser) {                                        /* the last picture the parser holds */
        uint8_t *o = NULL;
        int on = 0;
        av_parser_parse2(d->parser, d->ctx, &o, &on, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (on > 0) send(d, o, on, d->parser->pts == AV_NOPTS_VALUE ? -1 : d->parser->pts, -1, out, ctx);
    }
    send(d, NULL, 0, -1, -1, out, ctx);
}

void swv_skip(SwVdec *d, int level)
{
    if (!d || d->skip == level) return;
    d->skip = level;
    d->ctx->skip_frame = level >= 2 ? AVDISCARD_NONKEY : level == 1 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
}

double swv_fps(const SwVdec *d)
{
    if (!d) return 0;
    AVRational r = d->ctx->framerate;
    if (r.num > 0 && r.den > 0) return (double)r.num / r.den;
    return 0;
}

uint32_t swv_errors(const SwVdec *d) { return d ? d->errors : 0; }

#else   /* built without FFmpeg */

int     swv_available(int codec) { (void)codec; return 0; }
SwVdec *swv_create(int codec, const uint8_t *extra, size_t elen, int w, int h, int parse, char *err, size_t errsz)
{
    (void)codec; (void)extra; (void)elen; (void)w; (void)h; (void)parse;
    snprintf(err, errsz, "built without FFmpeg");
    return NULL;
}
void    swv_destroy(SwVdec *d) { (void)d; }
int     swv_threads(SwVdec *d, int n) { (void)d; (void)n; return 1; }
int     swv_decode(SwVdec *d, const uint8_t *data, size_t len, int64_t pts, int64_t dts, SwvOut out, void *ctx)
{ (void)d; (void)data; (void)len; (void)pts; (void)dts; (void)out; (void)ctx; return 0; }
void    swv_flush(SwVdec *d, SwvOut out, void *ctx) { (void)d; (void)out; (void)ctx; }
void    swv_skip(SwVdec *d, int level) { (void)d; (void)level; }
double  swv_fps(const SwVdec *d) { (void)d; return 0; }
uint32_t swv_errors(const SwVdec *d) { (void)d; return 0; }

#endif
