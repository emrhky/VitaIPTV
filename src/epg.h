#ifndef EPG_H
#define EPG_H
/* 0.4.5: Xtream "short EPG" (now / next) for a live channel: player_api.php?action=get_short_epg&stream_id=ID.
 * Titles come base64 encoded (some servers send them plain), times as Unix timestamps. */
#include <stddef.h>
#include <stdint.h>

typedef struct { char title[96]; int64_t start, end; } EpgProg;
typedef struct { int has_now, has_next; EpgProg now, next; } EpgInfo;

/* The programme on at `now` and the one after it. Returns how many listings were read (0: none). */
int  epg_parse_short(const char *json, size_t len, int64_t now, EpgInfo *out);
/* "Now: Title (20:00-21:30)  ·  Next: Title (21:30)" in local time (tz_min: minutes east of UTC). The
 * words come from the caller (translated). Returns the length, 0 when there is nothing to show. */
size_t epg_line(const EpgInfo *e, int tz_min, const char *now_word, const char *next_word, char *out, size_t cap);
/* the channel's stream id from an Xtream live link (.../live/USER/PASS/ID.ts); 0 if it is not one */
int  epg_stream_id(const char *url, char *out, size_t cap);
/* days since 1970-01-01 for a calendar date (proleptic Gregorian) */
int64_t epg_days(int y, int m, int d);
/* base64 to bytes; returns the length, -1 if it is not valid base64 */
int  epg_b64(const char *in, size_t n, char *out, size_t cap);

#endif
