/* tsinfo: analyze a TS file with the demuxer.
 *   tsinfo file.ts [-chunk N] [-o prefix]
 * -chunk N : feed N bytes at a time (0 = random sizes 1..3000)
 * -o prefix: write prefix.video (Annex-B) and prefix.audio (ADTS) */
#include "tsdemux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *fv, *fa;
static uint64_t vcalls, acalls, vbytes, abytes;
static int bad_pts_order;

static void on_video(void *c, const uint8_t *d, size_t n, int64_t pts, int64_t dts, int fl)
{
    (void)c; (void)pts; (void)dts; (void)fl;
    vcalls++; vbytes += n;
    if (fv) fwrite(d, 1, n, fv);
}
static void on_audio(void *c, const uint8_t *d, size_t n, int64_t pts)
{
    (void)c; (void)pts;
    acalls++; abytes += n;
    if (fa) fwrite(d, 1, n, fa);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: tsinfo file [-chunk N] [-o prefix]\n"); return 2; }
    long chunk = -1;
    const char *prefix = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-chunk") && i + 1 < argc) chunk = atol(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) prefix = argv[++i];
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 2; }
    if (prefix) {
        char p[512];
        snprintf(p, sizeof p, "%s.video", prefix); fv = fopen(p, "wb");
        snprintf(p, sizeof p, "%s.audio", prefix); fa = fopen(p, "wb");
    }
    TsSink sink = { on_video, on_audio, NULL };
    TsDemux *d = ts_create(&sink);
    uint8_t *buf = malloc(1 << 20);
    srand(1234);
    for (;;) {
        size_t want = chunk < 0 ? 65536 : chunk > 0 ? (size_t)chunk : (size_t)(1 + rand() % 3000);
        size_t n = fread(buf, 1, want, f);
        if (n == 0) break;
        ts_feed(d, buf, n);
    }
    ts_flush(d);
    const TsInfo *i = ts_info(d);
    printf("program=%d\n", i->program);
    for (int k = 0; k < i->nstreams; k++)
        printf("stream=pid:%d,type:0x%02x,codec:%s\n", i->streams[k].pid, i->streams[k].stream_type, ts_codec_name(i->streams[k].codec));
    printf("video_codec=%s\nwidth=%d\nheight=%d\nprofile=%d\nconstraint=%d\nlevel=%d\nchroma=%d\nbit_depth=%d\n"
           "frame_mbs_only=%d\nref_frames=%d\nfps=%.3f\nsps_len=%d\npps_len=%d\n",
           ts_codec_name(i->video_codec), i->width, i->height, i->profile, i->constraint, i->level,
           i->chroma_format, i->bit_depth, i->frame_mbs_only, i->ref_frames, ts_video_fps(i), i->sps_len, i->pps_len);
    printf("audio_codec=%s\naac_object=%d\naac_rate=%d\naac_channels=%d\n",
           ts_codec_name(i->audio_codec), i->aac_object, i->aac_rate, i->aac_channels);
    printf("packets=%llu\nbytes=%llu\nsync_losses=%u\ncc_errors=%u\ntei_errors=%u\nscrambled=%u\noverflows=%u\n",
           (unsigned long long)i->packets, (unsigned long long)i->bytes, i->sync_losses, i->cc_errors,
           i->tei_errors, i->scrambled_packets, i->pes_overflows);
    printf("video_aus=%u\nkeyframes=%u\ndamaged_aus=%u\naudio_frames=%u\n", i->video_aus, i->keyframes, i->damaged_aus, i->audio_frames);
    printf("first_video_pts=%lld\nfirst_audio_pts=%lld\n", (long long)i->first_video_pts, (long long)i->first_audio_pts);
    printf("sink_video_calls=%llu\nsink_audio_calls=%llu\nsink_video_bytes=%llu\nsink_audio_bytes=%llu\n",
           (unsigned long long)vcalls, (unsigned long long)acalls, (unsigned long long)vbytes, (unsigned long long)abytes);
    (void)bad_pts_order;
    ts_destroy(d);
    if (fv) fclose(fv);
    if (fa) fclose(fa);
    free(buf);
    fclose(f);
    return 0;
}
