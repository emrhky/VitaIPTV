#ifndef HTTPIO_H
#define HTTPIO_H
#include <stdint.h>

/* The log (applog.c): a line to log.txt. 1.2.1: gathered in memory, written at most a second later (an error
 * line at once); plog_flush writes what waits (the app ends, the Vita sleeps), plog_tick every frame. */
void plog(const char *fmt, ...);
void plog_flush(void);
void plog_tick(void);
void player_log_reset(void);                 /* (the app starts: log.txt anew, the last one as log_prev.txt) */

/* File-reader callbacks for SceAvPlayerFileReplacement, reading over HTTP. */
void    *httpio_object(void);
int      httpio_open(void *p, const char *filename);
int      httpio_close(void *p);
int      httpio_read(void *p, uint8_t *buffer, uint64_t position, uint32_t length);
uint64_t httpio_size(void *p);

#endif
