#ifndef DL_H
#define DL_H
/* 1.3: downloads (films and episodes) for watching without the internet.
 *
 * Each one is a folder under the root (ux0:data/VitaIPTV/downloads/<kind>_<key>_<name>/):
 *   video.<ext>   the file as the server sends it (MKV, MP4, TS...; over 4 GB is fine on exFAT)
 *   poster.jpg    the poster the server gives (if any)
 *   info.txt      name, year, plot, rating, length, series / season / episode, the playlist, the address,
 *                 the size, finished or not, the key of the sound / subtitle choice (avpref.h)
 * One download at a time, in a thread of its own, while the app runs; a stopped one goes on where it stopped
 * (HTTP Range). Before it starts (and every 32 MB) the free space is checked: the rest of the file plus
 * DL_RESERVE must fit.
 * Portable C: on the PC the same code runs with POSIX files (tests/test_dl.c). */
#include <stddef.h>
#include <stdint.h>

#define DL_MAX       256                    /* downloads listed */
#define DL_URL_MAX   1280
#define DL_DIR_MAX   256
#define DL_RESERVE   (200ull * 1024 * 1024)  /* left free on the memory card */

typedef struct {
    char dir[DL_DIR_MAX];                          /* its folder (the key for everything here) */
    char kind;                              /* 'F' film, 'E' episode */
    char title[128];                        /* the film's / episode's name */
    char series[128];                       /* an episode: its series */
    int  season, episode;                   /* (0 = not known) */
    char year[8], genre[64], rating[8];
    int  dur_min;
    char plot[480];
    char source[64];                        /* the playlist it came from */
    char url[DL_URL_MAX];                   /* where it is downloaded from (may hold |User-Agent=...) */
    char poster_url[512];
    char video[24];                         /* the file's name in dir ("video.mkv") */
    uint64_t size;                          /* the whole file (0 = not known yet) */
    uint64_t have;                          /* on the memory card now (dl_scan: the file's size) */
    int done;                               /* finished */
    uint32_t avkey;                         /* the sound / subtitle choice's key (avpref.h) */
    int av_series;                          /* (an episode's: its series') */
    long long added;                        /* when (seconds) */
} DlItem;

enum { DL_IDLE = 0, DL_QUEUED, DL_ACTIVE, DL_FAILED, DL_DONE };
typedef struct {
    int state;
    uint64_t have, total;
    unsigned bps;                           /* bytes a second (active) */
    char err[96];                           /* DL_FAILED: why (English, for T()) */
    int err_arg_mb[2];                      /* (space: needed, free; MB) */
} DlState;

/* ---- the info file and names (no thread) ---- */
int  dl_info_write(const DlItem *it);       /* <dir>/info.txt; 0 = ok */
int  dl_info_read(DlItem *it, const char *dir);   /* 0 = ok (it->dir set) */
/* the folder's name for a download: "F_1a2b3c4d_Film_Name" (Turkish letters as plain ones, at most 40) */
void dl_folder_name(char kind, uint32_t key, const char *title, char *out, size_t cap);
/* "video.mkv": the address' extension when it is a video one, else .mkv */
void dl_video_name(const char *url, char *out, size_t cap);
/* "1,4 GB", "820 MB" (comma: tr) */
void dl_size_text(uint64_t bytes, int comma, char *out, size_t cap);

/* ---- the downloads ---- */
void dl_init(const char *root);             /* once (the folder is made when the first one starts) */
const char *dl_root(void);
/* every download under the root (newest first), up to max; have = the file's size now */
int  dl_scan(DlItem *out, int max);
/* makes the folder and info.txt for it (it->url, title... set; dir, video, added are filled) and queues it.
 * 0 = queued, 1 = it is there already (queued again if unfinished), -1 = cannot write */
int  dl_add(DlItem *it);
int  dl_resume(const char *dir);            /* queue it again (unfinished); 0 = ok */
void dl_pause(const char *dir);             /* stops it (the file stays: it goes on from there later) */
void dl_pause_all(void);
int  dl_delete(const char *dir);            /* stops it, then removes the folder; 0 = ok */
int  dl_state(const char *dir, DlState *s); /* returns s->state */
int  dl_active(void);                       /* something is being downloaded or waits for it */
int  dl_queued_count(void);
/* the free space of the device the root is on (bytes), -1 = cannot be read */
long long dl_free_bytes(void);
void dl_shutdown(void);                     /* at exit: stops (what is there stays) */

#endif
