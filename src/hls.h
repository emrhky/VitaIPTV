#ifndef HLS_H
#define HLS_H
/* HLS (.m3u8) playlists: parsing only, no network (tsplayer.c downloads). */
#include <stddef.h>
#include <stdint.h>

#define HLS_URL_MAX 1024

typedef struct {
    char uri[HLS_URL_MAX];              /* absolute */
    long bandwidth;
    int width, height;                  /* 0 if not given */
    int hevc, audio_only;               /* from CODECS */
    int avc_level;                      /* H.264 level x10 from CODECS (avc1.PPCCLL), 0 if not given */
} HlsVariant;

typedef struct {
    int64_t seq;                        /* media sequence number */
    int duration_ms;
    const char *uri;                    /* points into the playlist text, not terminated */
    int uri_len;
    int discontinuity;                  /* a #EXT-X-DISCONTINUITY comes before it */
} HlsSegment;

typedef struct {
    int target_ms;                      /* #EXT-X-TARGETDURATION */
    int64_t first_seq;                  /* #EXT-X-MEDIA-SEQUENCE */
    int endlist;                        /* VOD / finished event */
    int encrypted;                      /* #EXT-X-KEY with a METHOD other than NONE */
    int fmp4;                           /* #EXT-X-MAP: fragmented MP4 segments */
    int nseg;
    HlsSegment *seg;                    /* malloc'd, free with hls_media_free */
} HlsMedia;

/* "#EXTM3U" at the start (a BOM and blank space allowed) */
int  hls_is_playlist(const char *text, size_t len);
/* the playlist lists variants (a master playlist) */
int  hls_is_master(const char *text);
/* Variants of a master playlist; returns how many were stored. */
int  hls_parse_master(const char *text, const char *base_url, HlsVariant *v, int max);
/* The variant to play: the best one up to max_h lines (720 or 1080) that the Vita can decode; -1 if none.
 * Without a size the highest bit rate under a cap is taken (2.6 Mbit/s for 720, 6 Mbit/s for 1080). */
int  hls_pick_variant(const HlsVariant *v, int n, int max_h);
/* A playable variant taller than h_low lines and at most max_h exists (e.g. 1080p next to 720p). */
int  hls_has_variant_above(const HlsVariant *v, int n, int h_low, int max_h);
/* Media playlist (text must stay alive while m is used). 0 = ok. */
int  hls_parse_media(const char *text, HlsMedia *m);
void hls_media_free(HlsMedia *m);
/* ref (absolute, //host, /path or relative) against the playlist address. */
void hls_resolve(const char *base, const char *ref, int ref_len, char *out, size_t cap);

#endif
