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
    char audio_group[64];               /* 0.6.0: AUDIO="..." (its sound is in that group's playlists), "" = none */
    int audio_codec;                    /* 0.6.0: from CODECS: 1 AAC (mp4a), 2 AC-3, 3 E-AC-3, 0 not said */
} HlsVariant;

/* 0.6.0: an audio playlist of its own (#EXT-X-MEDIA:TYPE=AUDIO with a URI): the variant's picture comes
 * without sound (or with another one) and this sound is fetched beside it */
#define HLS_MAX_RENDITIONS 16
typedef struct {
    char uri[HLS_URL_MAX];              /* absolute */
    char group[64];                     /* GROUP-ID */
    char lang[16];                      /* LANGUAGE ("tr", "tur", "en-US", ...), "" if not given */
    char name[64];                      /* NAME */
    int is_default, autoselect;         /* DEFAULT=YES, AUTOSELECT=YES */
} HlsRendition;

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
/* 0.6.0: the audio playlists of a master playlist (only those with a URI: without one the sound is in the
 * variant itself); returns how many were stored */
int  hls_parse_audio(const char *text, const char *base_url, HlsRendition *r, int max);
/* 0.6.0: group has an audio rendition without a URI (its sound is the one inside the variant): 1 and its
 * LANGUAGE / NAME, else 0 */
int  hls_audio_muxed(const char *text, const char *group, char *lang, size_t lcap, char *name, size_t ncap);
/* The one to play from the renditions of group (all when group is ""): want_index >= 0 that one (if it is in
 * the group), else the language lang3 (ISO 639-2, "" = none), else DEFAULT=YES, else the first; -1 if none */
int  hls_pick_audio(const HlsRendition *r, int n, const char *group, int want_index, const char *lang3);
/* The variant to play: the best one up to max_h lines (720 or 1080) that the Vita can decode; -1 if none.
 * Without a size the highest bit rate under a cap is taken (2.6 Mbit/s for 720, 6 Mbit/s for 1080). */
int  hls_pick_variant(const HlsVariant *v, int n, int max_h);
/* The variant the player wants (0.3.0):
 *   want_bw > 0: the playable variant with exactly this bit rate (picked from a list), if there is one;
 *   want_h 0: automatic - about 540 lines (500-600, the Vita's screen) if there is one, else as hls_pick_variant(720);
 *   want_h -1: the smallest; want_h > 0: as hls_pick_variant(want_h) (the best up to that many lines). */
int  hls_pick_wanted(const HlsVariant *v, int n, int want_h, long want_bw);
/* A playable variant taller than h_low lines and at most max_h exists (e.g. 1080p next to 720p). */
int  hls_has_variant_above(const HlsVariant *v, int n, int h_low, int max_h);
/* Media playlist (text must stay alive while m is used). 0 = ok. */
int  hls_parse_media(const char *text, HlsMedia *m);
void hls_media_free(HlsMedia *m);
/* ref (absolute, //host, /path or relative) against the playlist address. */
void hls_resolve(const char *base, const char *ref, int ref_len, char *out, size_t cap);

#endif
