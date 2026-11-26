#ifndef HTTPIO_H
#define HTTPIO_H
#include <stdint.h>

/* The log (applog.c): a line to log.txt. 1.2.1: gathered in memory, written at most a second later (an error
 * line at once); plog_flush writes what waits (the app ends, the Vita sleeps), plog_tick every frame. */
void plog(const char *fmt, ...);
void plog_flush(void);
void plog_tick(void);
void player_log_reset(void);                 /* (the app starts: log.txt anew, the last one as log_prev.txt) */
void plog_set_all(int all);                  /* 1.2.4n: 1 every line, 0 the errors only (Settings > Log file) */

/* (1.2.4o: the HTTP reader for SceAvPlayer that this file was named after is gone with src/player.c: .mp4 /
 * .mov links play in our player) */

#endif
