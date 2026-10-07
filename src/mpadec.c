#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3.h"
#include "mpadec.h"
#include <stdlib.h>
#include <string.h>

#define BUF_CAP   (32 * 1024)
#define KEEP      3000                  /* a frame and the next header must be there (minimp3 checks it) */
#define MARKS     32

struct MpaDec {
    mp3dec_t dec;
    uint8_t buf[BUF_CAP];
    size_t len;
    uint64_t base;                      /* stream offset of buf[0] */
    struct { uint64_t off; int64_t pts; } mark[MARKS];
    int nmark;
    int64_t next_pts;                   /* time of the next frame, -1 = unknown */
    uint32_t frames, skipped;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
};

MpaDec *mpa_create(void)
{
    MpaDec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    mp3dec_init(&d->dec);
    d->next_pts = -1;
    return d;
}

void mpa_destroy(MpaDec *d) { free(d); }

void mpa_stats(const MpaDec *d, uint32_t *frames, uint32_t *skipped)
{
    *frames = d ? d->frames : 0;
    *skipped = d ? d->skipped : 0;
}

/* The packet time belongs to the first frame starting at or after the packet's first byte. */
static int64_t frame_pts(MpaDec *d, uint64_t start)
{
    int64_t pts = -1;
    int k = 0;
    while (k < d->nmark && d->mark[k].off <= start) { if (d->mark[k].pts >= 0) pts = d->mark[k].pts; k++; }
    if (k) { memmove(d->mark, d->mark + k, sizeof d->mark[0] * (size_t)(d->nmark - k)); d->nmark -= k; }
    return pts;
}

static void consume(MpaDec *d, size_t n)
{
    if (n > d->len) n = d->len;
    memmove(d->buf, d->buf + n, d->len - n);
    d->len -= n;
    d->base += n;
}

static void run(MpaDec *d, size_t keep, MpaOut out, void *ctx)
{
    while (d->len > keep) {
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof info);
        int samples = mp3dec_decode_frame(&d->dec, d->buf, (int)d->len, d->pcm, &info);
        if (!info.hz) {                                     /* no complete frame: skip what is certainly junk */
            if (info.frame_bytes > 0) { d->skipped += (uint32_t)info.frame_bytes; consume(d, (size_t)info.frame_bytes); continue; }
            if (d->len >= BUF_CAP - 4096) { d->skipped += (uint32_t)(d->len - KEEP); consume(d, d->len - KEEP); }
            break;
        }
        int64_t p = frame_pts(d, d->base + (uint64_t)info.frame_offset);
        if (p >= 0) d->next_pts = p;
        int64_t pts = d->next_pts;
        int n = samples > 0 ? samples : (info.layer == 1 ? 384 : 1152);
        if (d->next_pts >= 0) d->next_pts += (int64_t)n * 90000 / info.hz;
        d->skipped += (uint32_t)info.frame_offset;
        consume(d, (size_t)info.frame_bytes);
        if (samples <= 0) continue;                         /* e.g. MP3 waiting for its bit reservoir */
        d->frames++;
        if (out) out(ctx, d->pcm, samples, info.hz, info.channels, info.layer, pts);
    }
}

void mpa_feed(MpaDec *d, const uint8_t *data, size_t len, int64_t pts, MpaOut out, void *ctx)
{
    if (!d || !len) return;
    if (d->nmark < MARKS) { d->mark[d->nmark].off = d->base + d->len; d->mark[d->nmark].pts = pts; d->nmark++; }
    while (len) {
        size_t room = BUF_CAP - d->len;
        if (!room) { run(d, KEEP, out, ctx); room = BUF_CAP - d->len; }
        if (!room) { d->skipped += (uint32_t)d->len; consume(d, d->len); room = BUF_CAP; }
        size_t n = len < room ? len : room;
        memcpy(d->buf + d->len, data, n);
        d->len += n;
        data += n;
        len -= n;
    }
    run(d, KEEP, out, ctx);
}

void mpa_flush(MpaDec *d, MpaOut out, void *ctx)
{
    if (d) run(d, 0, out, ctx);
}
