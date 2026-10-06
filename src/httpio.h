#ifndef HTTPIO_H
#define HTTPIO_H
#include <stdint.h>

/* Log function provided by player.c (appends a line to log.txt). */
void plog(const char *fmt, ...);

/* File-reader callbacks for SceAvPlayerFileReplacement, reading over HTTP. */
void    *httpio_object(void);
int      httpio_open(void *p, const char *filename);
int      httpio_close(void *p);
int      httpio_read(void *p, uint8_t *buffer, uint64_t position, uint32_t length);
uint64_t httpio_size(void *p);

#endif
