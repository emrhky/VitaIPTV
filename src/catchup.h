#ifndef CATCHUP_H
#define CATCHUP_H
/*
 * 0.8.0: catch-up (an earlier programme of a live channel) addresses. Portable C.
 *
 *  CU_XC        Xtream: .../live/U/P/ID.ts -> .../timeshift/U/P/<minutes>/<YYYY-MM-DD:HH-MM>/ID.ts, the start in
 *               the server's local time (srv_off_s = its local time - UTC)
 *  CU_SHIFT     the link + ?utc=<start>&lutc=<now>
 *  CU_APPEND    the link + the template
 *  CU_DEFAULT   the template (a whole address), else like CU_APPEND
 *  CU_FLUSSONIC .../index.m3u8 -> .../index-<start>-<duration>.m3u8, .../mpegts -> .../timeshift_abs-<start>.ts
 *
 * Template words: {utc} ${start} {start} {utcend} ${end} {end} {lutc} ${now} {now} {timestamp} {duration}
 * ${duration} {duration:N} (in units of N seconds) {offset} ${offset} {Y} {m} {d} {H} {M} {S} (the start, UTC).
 * A link's "|User-Agent=..." part stays at the end.
 */
#include <stddef.h>
#include <stdint.h>

/* 0 = ok, -1 = this channel has no usable catch-up (or the address did not fit) */
int catchup_url(int mode, const char *live_url, const char *tmpl, int64_t start, int dur_s, int64_t now,
                int srv_off_s, char *out, size_t cap);

#endif
