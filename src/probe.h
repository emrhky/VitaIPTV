#ifndef PROBE_H
#define PROBE_H
#include <stdint.h>

/* Reads a few seconds of a TS stream (HTTP URL or local file) in a background
 * thread and reports what is inside: codecs, resolution, errors. */

typedef enum { PROBE_IDLE = 0, PROBE_RUNNING, PROBE_DONE, PROBE_FAILED } ProbeState;

#define PROBE_LINES 14

typedef struct {
    volatile int state;                 /* ProbeState */
    volatile uint32_t bytes;            /* progress */
    char error[100];
    int nlines;
    char lines[PROBE_LINES][100];
} ProbeResult;

int                 probe_start(const char *url);   /* 0 = started */
void                probe_cancel(void);              /* returns immediately */
const ProbeResult  *probe_result(void);              /* NULL if nothing started */

#endif
