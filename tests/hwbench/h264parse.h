/* Vita IPTV hardware test - H.264 file reading (portable: also built on the PC for testing). */
#ifndef H264PARSE_H
#define H264PARSE_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t off, size;     /* the access unit inside H264File.data */
    uint8_t nonref;         /* every slice has nal_ref_idc 0: no other picture needs it */
    uint8_t idr;
    int32_t idx;            /* place in showing order inside its group of pictures (from the POC) */
} H264Au;

typedef struct {
    uint8_t *data;
    size_t size;
    H264Au *au;
    int n;
    int width, height;      /* shown size (after cropping) */
    int mbw, mbh;           /* coded size in 16x16 macroblocks */
    int level, refs, profile, interlaced;
    int fps;                /* from the file name ("..p50") or 25 */
} H264File;

enum { MODE_ALL, MODE_EVEN, MODE_REFS, MODES };
extern const char *const MODE_NAME[MODES];

/* Reads and splits the file. Returns 0 or a negative error (and writes why into err). */
int h264_load(H264File *f, const char *path, char *err, size_t errsz);
void h264_free(H264File *f);
/* Whether access unit i is decoded in this mode:
 *   all  - every picture
 *   even - the reference pictures and the other pictures at even places in showing order: every second
 *          picture is shown, evenly spaced (50 -> 25 fps), and some hidden reference pictures are decoded
 *   refs - only the reference pictures (what Vita IPTV 1.0 does at 1080p above 30 fps) */
int h264_keep(const H264File *f, int i, int mode);
/* Share of the access units decoded in this mode (0-1) */
double h264_share(const H264File *f, int mode);
#endif
