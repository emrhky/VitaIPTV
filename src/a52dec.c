#include "a52dec.h"
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_A52
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

struct A52Dec {
    AVCodecContext *ctx;
    AVCodecParserContext *parser;
    AVPacket *pkt;
    AVFrame *frame;
    int16_t pcm[A52_MAX_SAMPLES * 2];
    int64_t next_pts;                   /* 90 kHz, -1 = unknown */
    uint32_t frames, errors;
};

int a52_available(void) { return 1; }

A52Dec *a52_create(void)
{
    A52Dec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_EAC3);      /* (the E-AC-3 decoder takes AC-3 too) */
    if (!c) c = avcodec_find_decoder(AV_CODEC_ID_AC3);
    d->ctx = c ? avcodec_alloc_context3(c) : NULL;
    d->parser = av_parser_init(AV_CODEC_ID_AC3);                    /* (splits AC-3 and E-AC-3 frames) */
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->ctx || !d->parser || !d->pkt || !d->frame) { a52_destroy(d); return NULL; }
    d->ctx->thread_count = 1;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    av_opt_set_chlayout(d->ctx->priv_data, "downmix", &stereo, 0);  /* the decoder mixes 5.1 down itself */
    if (avcodec_open2(d->ctx, c, NULL) < 0) { a52_destroy(d); return NULL; }
    d->next_pts = -1;
    return d;
}

void a52_destroy(A52Dec *d)
{
    if (!d) return;
    if (d->parser) av_parser_close(d->parser);
    avcodec_free_context(&d->ctx);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    free(d);
}

static inline int16_t clip16(float v)
{
    int x = (int)(v * 32767.0f + (v >= 0 ? 0.5f : -0.5f));
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

/* the decoded frame as interleaved 16-bit stereo (or mono); more channels than two (the decoder did not mix
 * them down): left / right with the centre and the surrounds added at -3 dB */
static int to_pcm(A52Dec *d, int *out_ch)
{
    AVFrame *f = d->frame;
    int n = f->nb_samples, ch = f->ch_layout.nb_channels;
    if (n <= 0 || n > A52_MAX_SAMPLES || ch <= 0) return 0;
    int planar = f->format == AV_SAMPLE_FMT_FLTP, flt = planar || f->format == AV_SAMPLE_FMT_FLT;
    if (!flt) return 0;
    #define SAMPLE(c, i) (planar ? ((const float *)f->extended_data[c])[i] : ((const float *)f->data[0])[(i) * ch + (c)])
    if (ch == 1) {
        for (int i = 0; i < n; i++) d->pcm[i] = clip16(SAMPLE(0, i));
        *out_ch = 1;
        return n;
    }
    if (ch == 2) {
        for (int i = 0; i < n; i++) { d->pcm[2 * i] = clip16(SAMPLE(0, i)); d->pcm[2 * i + 1] = clip16(SAMPLE(1, i)); }
        *out_ch = 2;
        return n;
    }
    int fl = av_channel_layout_index_from_channel(&f->ch_layout, AV_CHAN_FRONT_LEFT);
    int fr = av_channel_layout_index_from_channel(&f->ch_layout, AV_CHAN_FRONT_RIGHT);
    int fc = av_channel_layout_index_from_channel(&f->ch_layout, AV_CHAN_FRONT_CENTER);
    int sl = av_channel_layout_index_from_channel(&f->ch_layout, AV_CHAN_SIDE_LEFT);
    int sr = av_channel_layout_index_from_channel(&f->ch_layout, AV_CHAN_SIDE_RIGHT);
    if (sl < 0) sl = av_channel_layout_index_from_channel(&f->ch_layout, AV_CHAN_BACK_LEFT);
    if (sr < 0) sr = av_channel_layout_index_from_channel(&f->ch_layout, AV_CHAN_BACK_RIGHT);
    if (fl < 0) fl = 0;
    if (fr < 0) fr = 1;
    const float k = 0.7071f, norm = 1.0f / (1.0f + k + k);
    for (int i = 0; i < n; i++) {
        float c = fc >= 0 ? SAMPLE(fc, i) * k : 0.0f;
        float l = SAMPLE(fl, i) + c + (sl >= 0 ? SAMPLE(sl, i) * k : 0.0f);
        float r = SAMPLE(fr, i) + c + (sr >= 0 ? SAMPLE(sr, i) * k : 0.0f);
        d->pcm[2 * i] = clip16(l * norm);
        d->pcm[2 * i + 1] = clip16(r * norm);
    }
    #undef SAMPLE
    *out_ch = 2;
    return n;
}

static void decode(A52Dec *d, const uint8_t *frame, int size, int64_t pts, A52Out out, void *ctx)
{
    int eac3 = size > 5 && (frame[5] >> 3) > 10;             /* bsid above 10: E-AC-3 */
    d->pkt->data = (uint8_t *)frame;
    d->pkt->size = size;
    if (avcodec_send_packet(d->ctx, d->pkt) < 0) { d->errors++; return; }
    while (avcodec_receive_frame(d->ctx, d->frame) == 0) {
        int ch = 0, n = to_pcm(d, &ch), rate = d->frame->sample_rate;
        if (n > 0 && rate > 0) {
            if (pts < 0) pts = d->next_pts;
            d->frames++;
            int src = d->ctx->ch_layout.nb_channels;
            out(ctx, d->pcm, n, rate, ch, src > 0 ? src : ch, eac3, pts);
            d->next_pts = pts >= 0 ? pts + (int64_t)n * 90000 / rate : -1;
            pts = -1;
        } else d->errors++;
        av_frame_unref(d->frame);
    }
}

void a52_feed(A52Dec *d, const uint8_t *data, size_t len, int64_t pts, A52Out out, void *ctx)
{
    int64_t in_pts = pts >= 0 ? pts : AV_NOPTS_VALUE;        /* the parser gives each frame the time of the packet it began in */
    while (len > 0) {
        uint8_t *fr = NULL;
        int fs = 0;
        int used = av_parser_parse2(d->parser, d->ctx, &fr, &fs, data, (int)len, in_pts, in_pts, 0);
        in_pts = AV_NOPTS_VALUE;
        if (used < 0) { d->errors++; return; }
        data += used;
        len -= (size_t)used;
        if (fs > 0) decode(d, fr, fs, d->parser->pts != AV_NOPTS_VALUE ? d->parser->pts : -1, out, ctx);
        if (used == 0 && fs == 0) break;
    }
}

void a52_stats(const A52Dec *d, uint32_t *frames, uint32_t *errors)
{
    *frames = d ? d->frames : 0;
    *errors = d ? d->errors : 0;
}

#else   /* built without FFmpeg */

int     a52_available(void) { return 0; }
A52Dec *a52_create(void) { return NULL; }
void    a52_destroy(A52Dec *d) { (void)d; }
void    a52_feed(A52Dec *d, const uint8_t *data, size_t len, int64_t pts, A52Out out, void *ctx)
{
    (void)d; (void)data; (void)len; (void)pts; (void)out; (void)ctx;
}
void    a52_stats(const A52Dec *d, uint32_t *frames, uint32_t *errors) { (void)d; *frames = *errors = 0; }

#endif
