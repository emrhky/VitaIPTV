/*
 * The log (log.txt). 1.2.1: the lines are gathered in memory (32 KB) and written to the memory card together:
 * at most once a second, when the space is full, at once for an error line, and when the app ends or the Vita
 * goes to sleep (plog_flush). Before, every line opened, wrote and closed the file (a few ms on the memory card,
 * on the player's threads too) with no lock (lines of two threads could mix).
 * Moved here from player.c; the size limits are as before.
 */
#include "httpio.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#ifndef APPLOG_PATH
#define APPLOG_PATH "ux0:data/VitaIPTV/log.txt"
#define APPLOG_PREV "ux0:data/VitaIPTV/log_prev.txt"
#endif

/* 0.8.1: the log has a size limit: the app may run for days (the console sleeps, it is not closed). Past 1 MB
 * the regular statistics lines stop; past 3 MB nothing more is written until the app starts again. */
#define LOG_SOFT (1u << 20)
#define LOG_HARD (3u << 20)
#define LOG_BUF  32768
#define LOG_EVERY_US 1000000u

static char g_buf[2][LOG_BUF];
static int g_cur;                                /* the buffer lines go to */
static size_t g_len;                             /* (in it) */
static unsigned g_log_bytes;                     /* the file's size (as it will be with what waits) */
static int g_soft_noted, g_hard_noted;
static SceUID g_mx = -1, g_wmx = -1;             /* the lines; the writing to the file (in order) */
static unsigned g_last_us;
/* 1.2.4n: Settings > Log file: everything (1), or only the errors and the version line (0, the default: most people
 * never read it; the memory card is then written to only when something went wrong) */
static volatile int g_all = 1;                   /* (1 until the settings are read: the start's lines) */
void plog_set_all(int all) { g_all = all ? 1 : 0; }

static int log_periodic(const char *fmt)
{
    static const char *const P[] = { "tsp: stats ", "tsp: rates ", "tsp: timing", "tsp: cpu", "tsp: decoder units",
                                     "tsp: HD decoder units", "tsp: HD decoder had" };
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) if (!strncmp(fmt, P[i], strlen(P[i]))) return 1;
    return 0;
}
/* a line that may be the last before a crash: written at once */
static int log_urgent(const char *fmt)
{
    /* 1.2.4o: not the statistics and the end lines: "errors %u" in them made each one an error line (with
     * "Errors only" the log got the statistics every 5 s, and each was written to the card at once) */
    if (log_periodic(fmt) || !strncmp(fmt, "tsp: video end", 14) || !strncmp(fmt, "tsp: audio end", 14)) return 0;
    return strstr(fmt, "ERROR") || strstr(fmt, "error") || strstr(fmt, "failed") || strstr(fmt, "FAILED") ||
           strstr(fmt, "could not") || !strncmp(fmt, "exit:", 5) || !strncmp(fmt, "power:", 6);
}

static void lock(SceUID m) { if (m >= 0) sceKernelLockMutex(m, 1, NULL); }
static void unlock(SceUID m) { if (m >= 0) sceKernelUnlockMutex(m, 1); }

/* what waits goes to the file (the lines' lock is held only to swap the buffers) */
void plog_flush(void)
{
    lock(g_wmx);
    lock(g_mx);
    int b = g_cur;
    size_t n = g_len;
    g_cur ^= 1;
    g_len = 0;
    g_last_us = sceKernelGetProcessTimeLow();
    unlock(g_mx);
    if (n) {
        FILE *f = fopen(APPLOG_PATH, "a");
        if (f) { fwrite(g_buf[b], 1, n, f); fclose(f); }
    }
    unlock(g_wmx);
}

/* (g_mx held) room for len more bytes; 0 = the line goes, -1 = no room even after writing (the lock is given
 * back and taken again around the writing) */
static int room(size_t len)
{
    if (g_len + len < LOG_BUF) return 0;
    unlock(g_mx);
    plog_flush();
    lock(g_mx);
    return g_len + len < LOG_BUF ? 0 : -1;
}

static void put(const char *s)
{
    size_t k = strlen(s);
    if (room(k) == 0) { memcpy(g_buf[g_cur] + g_len, s, k); g_len += k; g_log_bytes += (unsigned)k; }
}

void plog(const char *fmt, ...)
{
    if (!g_all && !log_urgent(fmt) && strncmp(fmt, "version ", 8) && strncmp(fmt, "log:", 4)) return;
    lock(g_mx);
    unsigned have = g_log_bytes;
    if (have >= LOG_HARD || (have >= LOG_SOFT && log_periodic(fmt))) { unlock(g_mx); return; }
    for (;;) {
        size_t space = LOG_BUF - g_len;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(g_buf[g_cur] + g_len, space, fmt, ap);
        va_end(ap);
        if (n < 0) break;
        if ((size_t)n + 1 < space) {                         /* it fitted, with its '\n' */
            g_len += (size_t)n;
            g_buf[g_cur][g_len++] = '\n';
            g_log_bytes += (unsigned)n + 1;
            break;
        }
        if (g_len > 0) {                                     /* full: write what waits, then again (other threads
                                                              * may fill it again meanwhile: as often as needed) */
            unlock(g_mx);
            plog_flush();
            lock(g_mx);
            continue;
        }
        g_len = LOG_BUF - 2;                                 /* (one line longer than the whole space: cut) */
        g_buf[g_cur][g_len++] = '\n';
        g_log_bytes += (unsigned)(LOG_BUF - 1);
        break;
    }
    if (!g_soft_noted && have < LOG_SOFT && g_log_bytes >= LOG_SOFT) { g_soft_noted = 1; put("log: 1 MB: the regular statistics lines stop here\n"); }
    if (!g_hard_noted && g_log_bytes >= LOG_HARD) { g_hard_noted = 1; put("log: 3 MB: nothing more is written until the app is started again\n"); }
    int now = log_urgent(fmt) || (unsigned)(sceKernelGetProcessTimeLow() - g_last_us) >= LOG_EVERY_US;
    unlock(g_mx);
    if (now) plog_flush();
}

/* every frame (main loop): what waited a second goes to the file */
void plog_tick(void)
{
    if (g_len && (unsigned)(sceKernelGetProcessTimeLow() - g_last_us) >= LOG_EVERY_US) plog_flush();
}

void player_log_reset(void)
{
    if (g_mx < 0) g_mx = sceKernelCreateMutex("applog", 0, 0, NULL);
    if (g_wmx < 0) g_wmx = sceKernelCreateMutex("applog_w", 0, 0, NULL);
    /* the previous run's log stays as log_prev.txt (after a freeze the app is restarted, which used to
     * wipe the log that showed what happened) */
    remove(APPLOG_PREV);
    rename(APPLOG_PATH, APPLOG_PREV);
    FILE *f = fopen(APPLOG_PATH, "w");
    if (f) { fputs("Vita IPTV log\n", f); fclose(f); }
    lock(g_mx);
    g_len = 0;
    g_log_bytes = 14;
    g_soft_noted = g_hard_noted = 0;
    g_last_us = sceKernelGetProcessTimeLow();
    unlock(g_mx);
}
