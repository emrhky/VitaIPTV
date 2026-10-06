#ifndef SCREENS_H
#define SCREENS_H
/* Whole screens drawn from plain data, so main.c only decides *what* to show. */
#include "ui.h"

#define SCR_ROWS 12                     /* list rows per page */

typedef struct {
    const char *name;
    const char *badge;                  /* small coloured label before the name (may be NULL) */
    unsigned badge_col;
    const char *right;                  /* dim text on the right (may be NULL) */
    int number;                         /* shown before the name if > 0 */
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
void scr_loading(const char *title, const char *line, unsigned t_ms);

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
} ScrPlayer;
void scr_player(const ScrPlayer *p);

void scr_radio(const char *name, const char *group, const char *audio_info, int level, unsigned t_ms,
               const UiHint *hints, int nhints, const char *note);

enum { SCR_F_TEXT, SCR_F_CHOICE, SCR_F_TOGGLE, SCR_F_BUTTON };
typedef struct { const char *label; const char *value; int kind; int enabled; } ScrField;
void scr_form(const char *title, const char *subtitle, const ScrField *f, int n, int sel,
              const UiHint *hints, int nhints, const char *status, int status_err);
/* small menu box over the current screen */
void scr_menu(const char *title, const char *const *items, int n, int sel);
/* yes/no box over the current screen */
void scr_confirm(const char *title, const char *line);
/* start-up screen: picture (may be NULL: drawn logo instead) and a status line */
void scr_splash(const vita2d_texture *img, const char *status, const char *version, unsigned t_ms);
void scr_about(const char *version);

void scr_lines(const char *title, const char *subtitle, const char *const *lines, int n, const char *error,
               int busy, unsigned t_ms, const UiHint *hints, int nhints);

#endif
