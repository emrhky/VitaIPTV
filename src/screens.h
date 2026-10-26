#ifndef SCREENS_H
#define SCREENS_H
/* Whole screens drawn from plain data, so main.c only decides *what* to show. */
#include "ui.h"
#include <stdint.h>

#define SCR_ROWS 11                     /* list rows per page (0.5.0: 11 taller rows for the bigger letters) */
#define SCR_SRC_ROWS 7                  /* playlist cards per page */

typedef struct {
    const char *name;
    const char *badge;                  /* small coloured label before the name (may be NULL) */
    unsigned badge_col;
    const char *right;                  /* dim text on the right (may be NULL) */
    const char *sub;                    /* second, dim line under the name (playlist cards, may be NULL) */
    int number;                         /* shown before the name if > 0 */
    int fav;                            /* 0.5.0: a star before the name (favourite) */
    int moving;                         /* 0.4.5: the row being moved (reordering the favourites) */
    int check;                          /* 0.7.0: a tick before the name (a watched episode) */
} ScrRow;
typedef void (*ScrRowFn)(int i, ScrRow *row, void *ctx);

typedef struct {
    const char *title, *subtitle, *right;
    int count, sel, *scroll;
    ScrRowFn row;
    void *ctx;
    const char *empty;                  /* text when the list is empty */
    const UiHint *hints;
    int nhints;
    const char *status;
    int status_err;
} ScrList;

void scr_list(const ScrList *l);

/* 0.8.0: a film's / series' short card (the preview beside the list, the details screen) */
typedef struct {
    const vita2d_texture *poster;       /* NULL: a frame (with a spinner while loading) */
    int loading;
    const char *title;
    const char *line;                   /* "2023  ·  2 h 8 min" */
    const char *genre, *director, *cast;
    float rating;                       /* out of 10, < 0 = not given */
    const char *plot;
} ScrVodCard;

/* Two lists side by side: categories on the left, channels on the right. */
typedef struct {
    const char *title, *subtitle, *right;
    int lcount, lsel, *lscroll;
    ScrRowFn lrow;
    int rcount, rsel, *rscroll;
    ScrRowFn rrow;
    void *ctx;
    int focus;                          /* 0 = left, 1 = right */
    int lmark;                          /* draw the left selection (off while searching) */
    const char *rempty;
    const UiHint *hints;
    int nhints;
    const char *status;
    int status_err;
    const ScrVodCard *preview;          /* 0.8.0: films / series, the list chosen: the categories step aside, this card beside */
} ScrDual;
void scr_dual(const ScrDual *d);
void scr_loading(const char *title, const char *line, unsigned t_ms);
/* the waiting background (a picture from the VPK, or drawn) behind loading screens */
void scr_set_backdrop(const vita2d_texture *img);
void scr_backdrop(void);

typedef struct {
    const char *name, *group, *info;    /* channel name, group, top-right info (resolution...) */
    int osd;                            /* show top/bottom bars */
    const UiHint *hints;
    int nhints;
    const char *stats1, *stats2;        /* detail lines above the bottom bar (may be NULL) */
    const char *center_title, *center_line;   /* message box (NULL = none) */
    unsigned center_col;
    int spinner;
    unsigned t_ms;
    int backdrop;                       /* no picture yet: draw the waiting background */
    /* films: time line (pos/dur in ms; seek_ms >= 0 = a jump being chosen) and the pause sign */
    int film, pos_ms, dur_ms, seek_ms, paused;
    const char *stats3;                 /* third line (table mode) */
    int stats_table;                    /* stats1-3 are rows of cells "label\vvalue" separated by tabs, in fixed columns */
    const char *stats_note;             /* table mode: a plain line above the table (may be NULL) */
    int fav;                            /* 0.5.0: a star before the name (favourite) */
    const char *clock;                  /* 0.7.0: the time of day, in the middle of the top bar (may be NULL) */
    const char *corner;                 /* 0.7.0: a note in the upper right corner over the picture ("next episode in 5 s") */
    const char *corner2;                /* ... its second, dim line (the buttons) */
    const char *subtitle;               /* 0.7.0: subtitle text over the picture (lines separated by \n; may be NULL) */
    const char *badge;                  /* 0.8.0: a label before the name ("ARCHIVE") */
    const char *time_l, *time_r;        /* 0.8.0: the time line's ends as clock times ("16:12", "17:00") instead of lengths */
    const char *time_note_l, *time_note_r;   /* ... and a line under it ("42 min watched", "Next: ...") */
} ScrPlayer;
void scr_player(const ScrPlayer *p);
/* 0.8.1: how subtitles look: size 0-3, colour 0-3, place 0-2, background 0-2 (see Settings) */
void scr_sub_style(int size, int color, int pos, int bg);

void scr_radio(const char *name, const char *group, const char *audio_info, int level, unsigned t_ms,
               const UiHint *hints, int nhints, const char *note, int fav);

enum { SCR_F_TEXT, SCR_F_CHOICE, SCR_F_TOGGLE, SCR_F_BUTTON, SCR_F_DANGER };   /* DANGER: a red button */
typedef struct { const char *label; const char *value; int kind; int enabled; } ScrField;
void scr_form(const char *title, const char *subtitle, const ScrField *f, int n, int sel,
              const UiHint *hints, int nhints, const char *status, int status_err);
/* small menu box over the current screen */
void scr_menu(const char *title, const char *const *items, int n, int sel);
/* 0.7.0: the player's Select menu: tabs on top (L / R), the tab's rows under them, a help line at the bottom;
 * the box keeps its size (rows scroll). dim[i] = a row that only says something (drawn dim), may be NULL. */
#define SCR_TAB_ROWS 7
void scr_tabmenu(const char *const *tabs, int ntabs, int tab, const char *const *items, const int *dim, int n, int sel,
                 const char *help);
/* yes/no box over the current screen */
void scr_confirm(const char *title, const char *line);
/* start-up screen: picture (may be NULL: drawn logo instead) and a status line */
void scr_splash(const vita2d_texture *img, const char *status, const char *version, unsigned t_ms);
void scr_about(const char *version);

void scr_lines(const char *title, const char *subtitle, const char *const *lines, int n, const char *error,
               int busy, unsigned t_ms, const UiHint *hints, int nhints);

/* ---- 0.8.0: the programme guide ---- */
enum { SCR_PROG_ARCHIVE = 1, SCR_PROG_SEL = 2, SCR_PROG_DIM = 4, SCR_PROG_LIVE = 8 };
#define SCR_GUIDE_PROGS 24
typedef struct { int64_t start, end; const char *title; int flags; } ScrProg;
typedef struct {
    const char *name;
    int number;                         /* > 0: shown before the name */
    int archive;                        /* the channel keeps programmes (catch-up) */
    int playing;                        /* the channel on in the player */
    int sel;                            /* the selected row */
    int loading;                        /* its programmes are being asked for */
    int noguide;                        /* 0.8.1: the channel has no guide at all (not only none at this time) */
    int nprogs;
    ScrProg progs[SCR_GUIDE_PROGS];
} ScrGuideRow;
typedef void (*ScrGuideRowFn)(int i, ScrGuideRow *r, void *ctx);
typedef struct {
    const char *title, *subtitle, *right;
    int64_t t0, now;                    /* the left edge and now (UTC seconds) */
    int span_min, tz_min;               /* minutes across; local time - UTC in minutes (for the labels) */
    const char *day;                    /* "Today", "Sun 25" ... left of the times */
    int count, first;                   /* rows: how many, the first one shown */
    ScrGuideRowFn row;
    void *ctx;
    const char *sel_title, *sel_line, *sel_desc, *sel_note;   /* the selected programme */
    int sel_live, sel_archive;
    float sel_progress;                 /* on now: how far (0..1) */
    const char *sel_left;               /* "28 min left" */
    const char *empty;                  /* nothing to show yet ("Loading the guide...") */
    const vita2d_texture *pic;          /* the player's picture (shown small at the top right), NULL = list mode */
    int pic_w, pic_h, pic_fields;
    const char *pic_label;
    int drawer, dcount, dsel, *dscroll; /* the category column over the guide */
    ScrRowFn drow;
    const UiHint *hints;
    int nhints;
    const char *popup_title, *popup_line;   /* "Watch from the start / Go live" box (popup_sel 0 / 1) */
    const char *popup_a, *popup_b;
    int popup, popup_sel;
} ScrGuide;
#define SCR_GUIDE_ROWS_LIST 7
#define SCR_GUIDE_ROWS_PLAYER 6
void scr_guide(const ScrGuide *g);

/* one channel's programmes, day by day (catch-up) */
typedef struct { const char *time, *title; int archive, live; float progress; } ScrCGRow;
typedef void (*ScrCGRowFn)(int i, ScrCGRow *r, void *ctx);
typedef struct {
    const char *title, *subtitle, *right;
    const char *const *days;
    int ndays, day;
    int count, sel, *scroll;
    ScrCGRowFn row;
    void *ctx;
    const char *d_title, *d_line1, *d_line2, *d_desc;   /* the selected one's details */
    int d_archive;
    const char *empty;
    const UiHint *hints;
    int nhints;
} ScrChanGuide;
#define SCR_CG_ROWS 9
void scr_chan_guide(const ScrChanGuide *c);

/* ---- 0.8.0: film / series details ---- */
typedef struct {
    const char *header, *header_sub;
    ScrVodCard card;
    const char *const *buttons;         /* the first one is the main one (blue) */
    int nbuttons, sel;
    float progress;                     /* the main button's bar (where it was left), < 0 = none */
    int fav;                            /* a star on the "Favourite" button */
    const char *note;
    int overlay;                        /* over the playing picture: no header, no buttons */
    const UiHint *hints;
    int nhints;
} ScrVodDetail;
void scr_vod_detail(const ScrVodDetail *d);

typedef struct { const char *name, *right; int check; float progress; } ScrEpRow;
typedef void (*ScrEpRowFn)(int i, ScrEpRow *r, void *ctx);
typedef struct {
    const char *header, *header_sub;
    ScrVodCard card;
    const char *const *seasons;
    int nseasons, season;
    int count, sel, *scroll;
    ScrEpRowFn row;
    void *ctx;
    const UiHint *hints;
    int nhints;
    const char *status;
} ScrSeries;
#define SCR_EP_ROWS 5
void scr_series(const ScrSeries *s);

#endif
