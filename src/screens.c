#include "screens.h"
#include "lang.h"
#include "qr_github.h"
#include <stdio.h>
#include <string.h>

#define LIST_Y   70
#define ROW_H    36                       /* 0.5.0: 36 (was 33) for text at the size of the settings */
#define LIST_FS  1.2f                     /* 0.5.0: the size of the settings screen (FS in scr_form) */

/* Playlist cards: a big coloured type tag, the name and a dim second line. */
#define CARD_H 56

void scr_list(const ScrList *l)
{
    ui_header(l->title, l->subtitle, l->right);
    int sel = l->sel, *scroll = l->scroll;
    if (sel < *scroll) *scroll = sel;
    if (sel >= *scroll + SCR_SRC_ROWS) *scroll = sel - SCR_SRC_ROWS + 1;
    if (*scroll < 0) *scroll = 0;

    if (l->count == 0 && l->empty) ui_message(l->empty, NULL, UI_DIM, 0, 0);

    for (int r = 0; r < SCR_SRC_ROWS && *scroll + r < l->count; r++) {
        int i = *scroll + r, y = LIST_Y + r * CARD_H;
        int on = i == sel;
        ScrRow row;
        memset(&row, 0, sizeof row);
        l->row(i, &row, l->ctx);
        ui_round_rect(12, y + 3, 922, CARD_H - 6, 10, on ? UI_SEL : RGBA8(24, 28, 41, 255));
        if (on) vita2d_draw_rectangle(12, y + 13, 4, CARD_H - 26, UI_ACCENT);
        int x = 28;
        if (row.badge) x += ui_tag(x, y + 11, 112, CARD_H - 22, row.badge, row.badge_col) + 18;
        if (row.fav) { ui_star(x + 11, y + CARD_H / 2 - 1, 11, UI_FAV); x += 30; }   /* 0.5.0: a favourite list */
        int rw = 0;
        if (row.right && row.right[0]) {
            rw = ui_text_w(1.0f, row.right);
            if (rw > 230) rw = 230;
            ui_text_fit(918 - rw, y + 34, on ? ui_theme.sel_text : UI_DIM, 1.0f, row.right, 230);
        }
        int maxw = 918 - rw - 16 - x;
        if (row.sub && row.sub[0]) {
            ui_text_fit(x, y + 27, UI_TEXT, 1.2f, row.name, maxw);
            if (on) ui_text_fit(x + 1, y + 27, UI_TEXT, 1.2f, row.name, maxw);
            ui_text_fit(x, y + 47, on ? ui_theme.sel_text : UI_DIM, 0.9f, row.sub, maxw);
        } else {
            ui_text_fit(x, y + 36, UI_TEXT, 1.2f, row.name, maxw);
            if (on) ui_text_fit(x + 1, y + 36, UI_TEXT, 1.2f, row.name, maxw);
        }
    }
    ui_scrollbar(946, LIST_Y + 4, SCR_SRC_ROWS * CARD_H - 8, *scroll, SCR_SRC_ROWS, l->count);
    ui_footer(l->hints, l->nhints, l->status, l->status_err);
}

static void splash_background(void);
static const vita2d_texture *g_backdrop;

void scr_set_backdrop(const vita2d_texture *img) { g_backdrop = img; }

void scr_backdrop(void)
{
    if (g_backdrop) { vita2d_draw_texture(g_backdrop, 0, 0); return; }
    splash_background();
    ui_logo(480, 250, 150);
}

void scr_loading(const char *title, const char *line, unsigned t_ms)
{
    scr_backdrop();
    ui_header("Vita IPTV", NULL, NULL);
    ui_message(title, line, UI_TEXT, 1, t_ms);
}

static void fmt_clock(char *b, size_t n, int ms)
{
    int sec = ms < 0 ? 0 : ms / 1000;
    if (sec >= 3600) snprintf(b, n, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    else snprintf(b, n, "%d:%02d", sec / 60, sec % 60);
}

/* films: position, length and a bar; a jump being chosen shows where it goes */
static int time_line(const ScrPlayer *p, int y)
{
    int notes = (p->time_note_l && p->time_note_l[0]) || (p->time_note_r && p->time_note_r[0]);
    if (notes) {                                             /* 0.8.0: a line under the bar (catch-up) */
        y -= 28;
        vita2d_draw_rectangle(0, y, 960, 28, UI_SHADE);
        if (p->time_note_l) ui_text_fit(18, y + 19, UI_DIM, 0.9f, p->time_note_l, 440);
        if (p->time_note_r) { int w = ui_text_w(0.9f, p->time_note_r); if (w > 440) w = 440; ui_text_fit(942 - w, y + 19, UI_DIM, 0.9f, p->time_note_r, 440); }
    }
    y -= 40;
    vita2d_draw_rectangle(0, y, 960, 40, UI_SHADE);
    char a[16], b[16];
    int shown = p->seek_ms >= 0 ? p->seek_ms : p->pos_ms;
    fmt_clock(a, sizeof a, shown);
    if (p->dur_ms > 0) fmt_clock(b, sizeof b, p->dur_ms); else snprintf(b, sizeof b, "--:--");
    if (p->time_l && p->time_l[0] && p->seek_ms < 0) snprintf(a, sizeof a, "%s", p->time_l);   /* 0.8.0: clock times */
    if (p->time_r && p->time_r[0]) snprintf(b, sizeof b, "%s", p->time_r);
    ui_text(18, y + 27, p->seek_ms >= 0 ? UI_WARN : UI_TEXT, 1.0f, a);
    int bw = ui_text_w(1.0f, b);
    ui_text(942 - bw, y + 27, UI_DIM, 1.0f, b);
    int x0 = 120, x1 = 942 - bw - 24;
    if (x1 > x0 + 40) {
        vita2d_draw_rectangle(x0, y + 17, x1 - x0, 6, UI_LINE);
        if (p->dur_ms > 0) {
            float f = (float)p->pos_ms / (float)p->dur_ms;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            vita2d_draw_rectangle(x0, y + 17, (x1 - x0) * f, 6, UI_ACCENT);
            if (p->seek_ms >= 0) {
                float g = (float)p->seek_ms / (float)p->dur_ms;
                if (g < 0) g = 0;
                if (g > 1) g = 1;
                vita2d_draw_rectangle(x0 + (x1 - x0) * g - 2, y + 10, 4, 20, UI_WARN);
            }
        }
    }
    return y;
}

/* One row of the player's info table: cells "label\vvalue" separated by tabs, in four fixed columns. The
 * label starts at the column, the value ends at the column's right edge, so changing numbers stay put. */
static void stat_row(int y, const char *row)
{
    static const int x0[4] = { 18, 268, 498, 728 }, x1[4] = { 250, 480, 710, 942 };
    const char *c = row;
    for (int k = 0; k < 4 && c && *c; k++) {
        const char *tab = strchr(c, '\t');
        size_t n = tab ? (size_t)(tab - c) : strlen(c);
        char cell[128];
        if (n >= sizeof cell) n = sizeof cell - 1;
        memcpy(cell, c, n);
        cell[n] = 0;
        char *v = strchr(cell, '\v');
        if (v) *v++ = 0;
        int lw = ui_text_w(0.9f, cell);
        ui_text(x0[k], y, UI_DIM, 0.9f, cell);
        if (v && *v) {
            int room = x1[k] - x0[k] - lw - 10;             /* never over the label */
            int vw = ui_text_w(0.9f, v);
            if (vw <= room) ui_text(x1[k] - vw, y, UI_TEXT, 0.9f, v);
            else ui_text_fit(x0[k] + lw + 10, y, UI_TEXT, 0.9f, v, room);
        }
        c = tab ? tab + 1 : NULL;
    }
}

/* 0.5.0: a note above the bottom bar, on as many 26-pixel rows as it needs (up to 3); returns the new top */
static int note_band(int y, const char *s, unsigned col)
{
    UiWrap w;
    if (!s || !s[0] || !ui_wrap(&w, 0.9f, s, 924, 3)) return y;
    y -= 26 * w.n;
    vita2d_draw_rectangle(0, y, 960, 26 * w.n, UI_SHADE);
    for (int i = 0; i < w.n; i++) ui_text(18, y + 19 + i * 26, col, 0.9f, w.line[i]);
    return y;
}

/* 0.7.0: subtitle lines at the bottom of the picture (above the bars when they show), white with a dark outline.
 * 0.8.1: size, colour, place and background as chosen (scr_sub_style). */
static int g_sub_size = 1, g_sub_color, g_sub_pos, g_sub_bg;
void scr_sub_style(int size, int color, int pos, int bg)
{
    g_sub_size = size >= 0 && size <= 3 ? size : 1;
    g_sub_color = color >= 0 && color <= 3 ? color : 0;
    g_sub_pos = pos >= 0 && pos <= 2 ? pos : 0;
    g_sub_bg = bg >= 0 && bg <= 2 ? bg : 0;
}

static void subtitle_draw(const char *text, int osd)
{
    static const float SC[4] = { 1.05f, 1.3f, 1.55f, 1.8f };
    static const unsigned COL[4] = { RGBA8(255, 255, 255, 255), RGBA8(255, 228, 64, 255), RGBA8(150, 255, 150, 255),
                                     RGBA8(140, 205, 255, 255) };
    float sc = SC[g_sub_size];
    char lines[4][160];
    int n = 0;
    const char *s = text;
    while (*s && n < 4) {                                   /* the cue's own lines, each wrapped to the width */
        const char *e = strchr(s, '\n');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        char one[320];
        if (len >= sizeof one) len = sizeof one - 1;
        memcpy(one, s, len);
        one[len] = 0;
        UiWrap w;
        ui_wrap(&w, sc, one, 900, 2);
        for (int i = 0; i < w.n && n < 4; i++) snprintf(lines[n++], sizeof lines[0], "%s", w.line[i]);
        s = e ? e + 1 : s + len;
    }
    if (!n) return;
    int lh = (int)(sc * 26.0f + 0.5f), asc = (int)(sc * 21.0f + 0.5f);
    int y;                                                   /* the first line's baseline */
    if (g_sub_pos == 2) y = (osd ? 64 : 0) + 16 + asc;      /* top (under the bar when it shows) */
    else y = (osd ? UI_FOOTER_Y - 70 : 530) - (g_sub_pos == 1 ? 80 : 0) - (n - 1) * lh;
    if (g_sub_bg == 1) {                                     /* a dark box around all the lines */
        int mw = 0;
        for (int i = 0; i < n; i++) { int w = ui_text_w(sc, lines[i]); if (w > mw) mw = w; }
        vita2d_draw_rectangle(480 - mw / 2 - 16, y - asc - 6, mw + 32, n * lh + 12, RGBA8(0, 0, 0, 200));
    }
    for (int i = 0; i < n; i++, y += lh) {
        int w = ui_text_w(sc, lines[i]), x = 480 - w / 2;
        if (g_sub_bg == 0) vita2d_draw_rectangle(x - 10, y - asc, w + 20, lh, RGBA8(0, 0, 0, 120));   /* a light band: readable on white */
        for (int dy = -2; dy <= 2; dy += 2)
            for (int dx = -2; dx <= 2; dx += 2)
                if (dx || dy) ui_text(x + dx, y + dy, RGBA8(0, 0, 0, 255), sc, lines[i]);
        ui_text(x, y, COL[g_sub_color], sc, lines[i]);
    }
}

/* 0.7.0: a box in the upper right corner ("Next episode in 5 s", the buttons under it) */
static void corner_note(const char *line, const char *line2, int osd)
{
    int w1 = ui_text_w(1.15f, line), w2 = line2 ? ui_text_w(0.95f, line2) : 0;
    int w = (w1 > w2 ? w1 : w2) + 36, h = line2 ? 74 : 46, x = 940 - w, y = osd ? 78 : 16;
    ui_round_rect(x, y, w, h, 12, UI_PANEL);
    ui_text(x + 18, y + 31, UI_TEXT, 1.15f, line);
    if (line2) ui_text(x + 18, y + 60, UI_DIM, 0.95f, line2);
}

void scr_player(const ScrPlayer *p)
{
    if (p->backdrop) scr_backdrop();
    if (p->center_title) ui_message(p->center_title, p->center_line, p->center_col, p->spinner, p->t_ms);
    if (p->paused && !p->center_title) {                    /* the pause sign */
        ui_round_rect(430, 222, 100, 100, 18, UI_PANEL);
        vita2d_draw_rectangle(456, 245, 16, 54, UI_TEXT);
        vita2d_draw_rectangle(488, 245, 16, 54, UI_TEXT);
    }
    if (p->subtitle && p->subtitle[0]) subtitle_draw(p->subtitle, p->osd);   /* 0.7.0 */
    if (p->corner && p->corner[0]) corner_note(p->corner, p->corner2, p->osd);
    if (!p->osd) return;
    vita2d_draw_rectangle(0, 0, 960, 62, UI_SHADE);
    vita2d_draw_rectangle(0, 62, 960, 2, UI_ACCENT);
    int iw = p->info ? ui_text_w(0.9f, p->info) : 0, nx = 20;
    int name_w = 900 - iw - (nx - 20);
    if (p->clock && p->clock[0]) {                          /* 0.7.0: the time in the middle; the name stops before it */
        int cw = ui_text_w(1.25f, p->clock);
        ui_text(480 - cw / 2, 30, UI_TEXT, 1.25f, p->clock);
        ui_text(481 - cw / 2, 30, UI_TEXT, 1.25f, p->clock);
        name_w = 480 - cw / 2 - 18 - nx;
    }
    if (p->badge && p->badge[0]) {                           /* 0.8.0: "ARCHIVE" before the name */
        int bw = ui_badge(nx, 12, p->badge, RGBA8(40, 120, 200, 255));
        nx += bw + 10;
        name_w -= bw + 10;
    }
    if (p->fav) { ui_star(nx + 11, 23, 11, UI_FAV); nx += 30; name_w -= 30; }   /* 0.5.0: a favourite */
    ui_text_fit(nx, 30, UI_TEXT, 1.25f, p->name, name_w);
    ui_text_fit(nx + 1, 30, UI_TEXT, 1.25f, p->name, name_w);
    if (p->group && p->group[0]) ui_text_fit(20, 52, UI_DIM, 0.9f, p->group, 920);   /* (0.4.5: wider, the EPG line) */
    if (p->info && p->info[0]) ui_text(940 - iw, 30, UI_DIM, 0.9f, p->info);
    int y = UI_FOOTER_Y;
    if (p->film) y = time_line(p, y);
    if (p->stats_table) {
        const char *rows[3] = { p->stats3, p->stats2, p->stats1 };   /* drawn from the bottom up */
        for (int r = 0; r < 3; r++) {
            if (!rows[r] || !rows[r][0]) continue;
            y -= 26;
            vita2d_draw_rectangle(0, y, 960, 26, UI_SHADE);
            stat_row(y + 19, rows[r]);
        }
        y = note_band(y, p->stats_note, UI_DIM);
    } else {
        y = note_band(y, p->stats2, UI_DIM);
        y = note_band(y, p->stats1, UI_DIM);
    }
    ui_footer(p->hints, p->nhints, NULL, 0);
}

void scr_radio(const char *name, const char *group, const char *audio_info, int level, unsigned t_ms,
               const UiHint *hints, int nhints, const char *note, int fav)
{
    ui_header(T("Radio"), group, NULL);
    if (fav) {                                               /* 0.5.0: a favourite station: a star before the name */
        int nw = ui_text_w(1.7f, name);
        ui_star(480 - nw / 2 - 24, 157, 13, UI_FAV);
    }
    ui_badge(480 - ui_text_w(0.8f, T("ON AIR")) / 2 - 6, 96, T("ON AIR"), RGBA8(220, 60, 80, 255));
    ui_text_center(480, 168, UI_TEXT, 1.7f, name);
    ui_text_center(481, 168, UI_TEXT, 1.7f, name);
    ui_meter(160, 196, 640, 170, level, t_ms);
    vita2d_draw_rectangle(160, 368, 640, 2, UI_LINE);
    if (audio_info && audio_info[0]) ui_text_center(480, 404, UI_DIM, 1.0f, audio_info);
    if (note && note[0]) ui_text_wrap(60, 436, UI_DIM, 0.9f, note, 840, 24, 2, 1);   /* 0.5.0: two lines if long */
    ui_footer(hints, nhints, NULL, 0);
}

void scr_lines(const char *title, const char *subtitle, const char *const *lines, int n, const char *error,
               int busy, unsigned t_ms, const UiHint *hints, int nhints)
{
    char tb[96];
    ui_header(ui_title(title, tb, sizeof tb), subtitle, NULL);
    if (busy) { ui_message(T("Analyzing stream..."), subtitle, UI_TEXT, 1, t_ms); ui_footer(hints, nhints, NULL, 0); return; }
    ui_panel(20, 70, 920, 420, UI_PANEL);
    int y = 104;
    if (error && error[0]) y += 26 * ui_text_wrap(40, y, UI_ERR, 1.05f, error, 880, 26, 2, 0) + 12;   /* 0.5.0: wraps */
    for (int i = 0; i < n; i++) {
        unsigned col = UI_TEXT;
        if (!strncmp(lines[i], "Playable by hardware: YES", 25)) col = UI_OK;
        else if (!strncmp(lines[i], "Playable by hardware: NO", 24)) col = UI_ERR;
        else if (!strncmp(lines[i], "Playable by hardware: MAYBE", 27)) col = UI_WARN;
        ui_text_fit(40, y, col, 1.0f, lines[i], 880);
        y += n > 11 ? 27 : 30;                              /* many lines: closer, to stay in the panel */
    }
    ui_footer(hints, nhints, NULL, 0);
}

#define FORM_Y 76
#define FORM_H 46

/* 0.4.5: everything at 1.2 (as the playlist names), labels with capitals; the value column starts after the
 * longest label (290..470), so a long one ("Hide channels named 4K / UHD") does not run into its box. */
void scr_form(const char *title, const char *subtitle, const ScrField *f, int n, int sel,
              const UiHint *hints, int nhints, const char *status, int status_err)
{
    const float FS = 1.2f;
    char tb[192], lb[192];
    ui_header(ui_title(title, tb, sizeof tb), subtitle, NULL);
    int fh = n > 8 ? 8 * FORM_H / n : FORM_H;              /* more than 8 rows: squeeze them to fit */
    int ty = fh / 2 + 8;                                    /* text baseline inside a row */
    int lw = 0;
    for (int i = 0; i < n; i++) {
        if (f[i].kind == SCR_F_BUTTON || f[i].kind == SCR_F_DANGER) continue;
        int w = ui_text_w(FS, ui_title(f[i].label, lb, sizeof lb));
        if (w > lw) lw = w;
    }
    int bx = 36 + lw + 26;
    if (bx < 290) bx = 290;
    if (bx > 470) bx = 470;
    int bw = 930 - bx;
    for (int i = 0; i < n; i++) {
        int y = FORM_Y + i * fh, on = f[i].enabled;
        unsigned lc = on ? UI_DIM : RGBA8(80, 84, 100, 255), vc = on ? UI_TEXT : RGBA8(90, 94, 110, 255);
        const char *label = ui_title(f[i].label, lb, sizeof lb);
        if (f[i].kind == SCR_F_BUTTON || f[i].kind == SCR_F_DANGER) {
            int bbw = 320, bbx = 480 - bbw / 2, red = f[i].kind == SCR_F_DANGER;
            unsigned bg = i == sel ? (red ? RGBA8(200, 52, 58, 255) : UI_ACCENT) : (red ? RGBA8(78, 30, 38, 255) : UI_PANEL2);
            unsigned fg = i == sel ? RGBA8(255, 255, 255, 255) : red ? RGBA8(255, 130, 130, 255) : vc;
            vita2d_draw_rectangle(bbx, y + 4, bbw, fh - 8, bg);
            ui_text_center(480, y + ty, fg, FS, label);
            continue;
        }
        if (i == sel) {
            vita2d_draw_rectangle(10, y + 2, 940, fh - 4, UI_SEL);
            vita2d_draw_rectangle(10, y + 2, 4, fh - 4, UI_ACCENT);
        }
        ui_text_fit(36, y + ty, i == sel ? UI_TEXT : lc, FS, label, bx - 36 - 12);
        vita2d_draw_rectangle(bx, y + 5, bw, fh - 10, i == sel ? ui_theme.sel_dark : UI_PANEL);
        const char *v = f[i].value && f[i].value[0] ? f[i].value : (on ? T("(empty)") : "-");
        unsigned col = f[i].value && f[i].value[0] ? vc : RGBA8(100, 104, 122, 255);
        if (f[i].kind == SCR_F_CHOICE) {
            ui_text(bx + 12, y + ty, i == sel ? UI_ACCENT : lc, FS, "<");
            const char *tv = ui_title(v, tb, sizeof tb);
            if (ui_text_w(FS, tv) <= bw - 80) ui_text_center(bx + bw / 2, y + ty, col, FS, tv);
            else ui_text_fit(bx + 40, y + ty, col, FS, tv, bw - 80);
            ui_text(bx + bw - 26, y + ty, i == sel ? UI_ACCENT : lc, FS, ">");
        } else if (f[i].kind == SCR_F_TOGGLE) {                /* a pill with the word in it */
            int onv = !strcmp(v, T("On"));
            const char *tv = ui_title(v, tb, sizeof tb);
            int pw = ui_text_w(FS, tv) + 28;
            ui_round_rect(bx + 10, y + 7, pw, fh - 14, (fh - 14) / 2, onv ? RGBA8(40, 160, 90, 255) : RGBA8(110, 110, 130, 255));
            ui_text(bx + 24, y + ty, RGBA8(255, 255, 255, 255), FS, tv);
        } else {
            ui_text_fit(bx + 12, y + ty, col, FS, v, bw - 24);
        }
    }
    ui_footer(hints, nhints, status, status_err);
}

void scr_menu(const char *title, const char *const *items, int n, int sel)
{
    /* 0.5.0: long menus (sizes, the server, audio tracks) get lower rows; the box is as wide as the longest item */
    char tb[192];
    int rh = 44;
    if (64 + n * rh > 520) rh = (520 - 64) / (n > 0 ? n : 1);
    if (rh < 26) rh = 26;
    float sc = rh >= 36 ? LIST_FS : rh >= 31 ? 1.05f : 0.95f;   /* 0.5.0: the size of the settings while it fits */
    int w = 420;
    for (int i = 0; i < n; i++) { int iw = ui_text_w(sc, ui_title(items[i], tb, sizeof tb)) + 60; if (iw > w) w = iw; }
    if (ui_text_w(1.25f, title) + 60 > w) w = ui_text_w(1.25f, title) + 60;
    if (w > 900) w = 900;
    int h = 64 + n * rh, x = (960 - w) / 2, y = (544 - h) / 2 - 10;
    if (y < 4) y = 4;
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 120));
    ui_panel(x, y, w, h, RGBA8(24, 28, 42, 245));
    ui_text_fit(x + (w - (ui_text_w(1.25f, title) < w - 40 ? ui_text_w(1.25f, title) : w - 40)) / 2, y + 36, UI_TEXT, 1.25f, title, w - 40);
    vita2d_draw_rectangle(x + 20, y + 50, w - 40, 1, UI_LINE);
    for (int i = 0; i < n; i++) {
        int ry = y + 58 + i * rh;
        if (i == sel) vita2d_draw_rectangle(x + 12, ry, w - 24, rh - 6, UI_SEL);
        ui_text_center(480, ry + (rh - 6) / 2 + (int)(9 * sc), UI_TEXT, sc, ui_title(items[i], tb, sizeof tb));   /* (0.4.5: capitals) */
    }
}

void scr_tabmenu(const char *const *tabs, int ntabs, int tab, const char *const *items, const int *dim, int n, int sel,
                 const char *help)
{
    char tb[192];
    const int w = 720, rh = 42, x = (960 - w) / 2;
    const int h = 70 + SCR_TAB_ROWS * rh + 48, y = (544 - h) / 2 - 6;
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 120));
    ui_panel(x, y, w, h, RGBA8(24, 28, 42, 245));
    /* the tabs, as wide as the box allows */
    int tw = (w - 32) / (ntabs > 0 ? ntabs : 1);
    for (int i = 0; i < ntabs; i++) {
        int tx = x + 16 + i * tw;
        if (i == tab) ui_round_rect(tx + 3, y + 12, tw - 6, 40, 10, UI_ACCENT);
        else ui_round_rect(tx + 3, y + 12, tw - 6, 40, 10, RGBA8(44, 50, 72, 255));
        const char *t = ui_title(tabs[i], tb, sizeof tb);
        float sc = ui_text_w(1.1f, t) <= tw - 16 ? 1.1f : 0.95f;
        ui_text_center(tx + tw / 2, y + 41, i == tab ? RGBA8(255, 255, 255, 255) : UI_DIM, sc, t);
    }
    vita2d_draw_rectangle(x + 16, y + 62, w - 32, 1, UI_LINE);
    int top = 0;                                             /* the rows: the selected one always in view */
    if (sel >= SCR_TAB_ROWS) top = sel - SCR_TAB_ROWS + 1;
    for (int r = 0; r < SCR_TAB_ROWS && top + r < n; r++) {
        int i = top + r, ry = y + 70 + r * rh;
        if (i == sel) vita2d_draw_rectangle(x + 14, ry, w - 28, rh - 6, UI_SEL);
        ui_text_fit(x + 34, ry + (rh - 6) / 2 + 10, dim && dim[i] ? UI_DIM : UI_TEXT, LIST_FS, items[i], w - 68);
    }
    if (top > 0) ui_fill_triangle(x + w - 30, y + 80, x + w - 22, y + 70, x + w - 14, y + 80, UI_DIM);   /* more above / below */
    if (top + SCR_TAB_ROWS < n) {
        int by = y + 70 + SCR_TAB_ROWS * rh - 14;
        ui_fill_triangle(x + w - 30, by, x + w - 22, by + 10, x + w - 14, by, UI_DIM);
    }
    vita2d_draw_rectangle(x + 16, y + h - 44, w - 32, 1, UI_LINE);
    if (help && help[0]) ui_text_center(480, y + h - 16, UI_DIM, 0.95f, help);
}

void scr_confirm(const char *title, const char *line)
{
    /* 0.4.0: its own box, tall enough for the buttons (they were drawn below the message box) */
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 120));
    /* 0.5.0: the letters at the size of the settings */
    int w = 760, h = 196, x = (960 - w) / 2, y = (544 - h) / 2 - 10;
    ui_panel(x, y, w, h, RGBA8(24, 28, 42, 245));
    ui_text_center(480, y + 46, UI_WARN, 1.3f, title);
    if (line && line[0]) ui_text_wrap(x + 24, y + 84, UI_DIM, LIST_FS, line, w - 48, 28, 2, 1);
    int by = y + h - 44;                                     /* button centres, inside the box */
    int yes_w = 30 + 8 + ui_text_w(LIST_FS, T("Yes")), no_w = 30 + 8 + ui_text_w(LIST_FS, T("No"));
    int bx = 480 - (yes_w + 60 + no_w) / 2;
    bx += ui_button(bx, by, UI_BTN_CROSS) + 8;
    ui_text(bx, by + 8, UI_TEXT, LIST_FS, T("Yes"));
    bx += ui_text_w(LIST_FS, T("Yes")) + 60;
    bx += ui_button(bx, by, UI_BTN_CIRCLE) + 8;
    ui_text(bx, by + 8, UI_TEXT, LIST_FS, T("No"));
}

static void splash_background(void)
{
    for (int y = 0; y < 544; y += 4) {                      /* soft vertical glow */
        float d = (y - 230) / 300.0f;
        int g = (int)(40 * (1.0f - d * d));
        if (g < 0) g = 0;
        vita2d_draw_rectangle(0, y, 960, 4, RGBA8(14 + g / 3, 16 + g / 2, 24 + g, 255));
    }
}

void scr_splash(const vita2d_texture *img, const char *status, const char *version, unsigned t_ms)
{
    if (img) vita2d_draw_texture(img, 0, 0);
    else {
        splash_background();
        ui_logo(480, 200, 160);
        ui_text_center(480, 336, UI_TEXT, 2.0f, "Vita IPTV");
        ui_text_center(481, 336, UI_TEXT, 2.0f, "Vita IPTV");
        ui_text_center(480, 372, UI_DIM, 1.0f, T("Live TV and radio on your Vita"));
    }
    if (version) {                                          /* the picture has no version: it is written here */
        ui_text(900 - ui_text_w(0.9f, version), 520, UI_DIM, 0.9f, version);
    }
    if (status && status[0]) {
        ui_spinner(480, 440, t_ms);
        ui_text_center(480, 490, UI_DIM, 0.9f, status);
    }
}

/* 0.4.0 (the approved layout): everything centred, the GitHub address and its QR code, "emrhky" small
 * at the bottom right. The version comes from the build (APP_VERSION, "v0.4.0"). */
void scr_about(const char *version)
{
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 160));
    const int x = 110, y = 20, w = 740, h = 504;             /* 0.5.0: wider, the letters at the size of the settings */
    ui_panel(x, y, w, h, RGBA8(24, 28, 42, 248));
    ui_logo(480, y + 56, 72);
    ui_text_center(480, y + 132, UI_TEXT, 1.45f, "Vita IPTV");
    ui_text_center(481, y + 132, UI_TEXT, 1.45f, "Vita IPTV");
    ui_text_center(480, y + 160, UI_DIM, 1.05f, version);
    ui_text_center(480, y + 196, UI_TEXT, LIST_FS, T("IPTV player for the PS Vita: live TV, films, series, radio."));
    ui_text_center(480, y + 222, UI_DIM, 1.05f, T("M3U and Xtream playlists \xC2\xB7 HLS, MPEG-TS, MKV, MP4, FLV \xC2\xB7 HTTPS"));
    ui_text_center(480, y + 245, UI_DIM, 1.05f, T("H.264 hardware decoder up to 1080p."));
    vita2d_draw_rectangle(x + 40, y + 256, w - 80, 1, UI_LINE);
    ui_text_center(480, y + 286, UI_ACCENT, LIST_FS, "github.com/emrhky/VitaIPTV");
    ui_text_center(481, y + 286, UI_ACCENT, LIST_FS, "github.com/emrhky/VitaIPTV");
    {   /* the QR code of that page: 3 px a module, 4 modules of white around it */
        const int m = 3, q = 4, side = (QR_GITHUB_N + 2 * q) * m;
        int qx = 480 - side / 2, qy = y + 298;
        vita2d_draw_rectangle(qx, qy, side, side, RGBA8(255, 255, 255, 255));
        for (int r = 0; r < QR_GITHUB_N; r++)
            for (int c = 0; c < QR_GITHUB_N; c++)
                if (QR_GITHUB[r] >> (QR_GITHUB_N - 1 - c) & 1)
                    vita2d_draw_rectangle(qx + (q + c) * m, qy + (q + r) * m, m, m, RGBA8(0, 0, 0, 255));
    }
    ui_text_center(480, y + 442, UI_DIM, 1.0f, T("MIT License \xC2\xB7 third-party notices in the VPK"));
    int cw = 30 + 8 + ui_text_w(LIST_FS, T("Close")), bx = 480 - cw / 2, by = y + h - 26;
    bx += ui_button(bx, by, UI_BTN_CIRCLE) + 8;
    ui_text(bx, by + 8, UI_TEXT, LIST_FS, T("Close"));
    ui_text(x + w - 16 - ui_text_w(1.0f, "emrhky"), y + h - 16, UI_DIM, 1.0f, "emrhky");
}

/* one list inside a rectangle; returns nothing, draws rows + scrollbar */
static void list_area(int x0, int w, int count, int sel, int *scroll, ScrRowFn fn, void *ctx, int focused, int mark, int numbers)
{
    if (sel < *scroll) *scroll = sel;
    if (sel >= *scroll + SCR_ROWS) *scroll = sel - SCR_ROWS + 1;
    if (*scroll > count - SCR_ROWS) *scroll = count - SCR_ROWS;
    if (*scroll < 0) *scroll = 0;
    for (int r = 0; r < SCR_ROWS && *scroll + r < count; r++) {
        int i = *scroll + r, y = LIST_Y + r * ROW_H;
        ScrRow row;
        memset(&row, 0, sizeof row);
        fn(i, &row, ctx);
        int is_sel = mark && i == sel;
        if (row.moving) {                                   /* 0.4.5: being moved: amber, with arrows on the right */
            vita2d_draw_rectangle(x0, y + 1, w - 10, ROW_H - 2, RGBA8(120, 84, 20, 255));
            vita2d_draw_rectangle(x0, y + 1, 4, ROW_H - 2, UI_WARN);
            int ax = x0 + w - 34, cy = y + ROW_H / 2;
            ui_fill_triangle(ax, cy - 3, ax + 12, cy - 3, ax + 6, cy - 11, UI_WARN);
            ui_fill_triangle(ax, cy + 3, ax + 12, cy + 3, ax + 6, cy + 11, UI_WARN);
        } else if (is_sel) {
            vita2d_draw_rectangle(x0, y + 1, w - 10, ROW_H - 2, focused ? UI_SEL : UI_PANEL2);
            vita2d_draw_rectangle(x0, y + 1, 4, ROW_H - 2, focused ? UI_ACCENT : ui_theme.accent_dim);
        } else if (r % 2) {
            vita2d_draw_rectangle(x0, y + 1, w - 10, ROW_H - 2, RGBA8(20, 23, 34, 255));
        }
        int x = x0 + 14;
        if (numbers && row.number > 0) {
            char nb[16];
            snprintf(nb, sizeof nb, "%d", row.number);
            ui_text(x0 + 60 - ui_text_w(1.0f, nb), y + 25, is_sel ? UI_TEXT : UI_DIM, 1.0f, nb);
            x = x0 + 70;
        }
        int rw = 0;
        if (row.right && row.right[0]) {
            rw = ui_text_w(1.0f, row.right);
            if (rw > 170) rw = 170;
            ui_text_fit(x0 + w - 18 - rw, y + 25, is_sel ? ui_theme.sel_text : UI_DIM, 1.0f, row.right, 170);
        }
        if (row.fav) { ui_star(x + 8, y + ROW_H / 2, 8, UI_FAV); x += 22; }   /* 0.5.0: a favourite */
        if (row.check) { ui_check(x + 8, y + ROW_H / 2, 8, RGBA8(64, 226, 160, 255)); x += 24; }   /* 0.7.0: watched */
        ui_text_fit(x, y + 26, (focused || is_sel) ? UI_TEXT : RGBA8(190, 194, 210, 255), LIST_FS, row.name, x0 + w - 28 - rw - x);
    }
    ui_scrollbar(x0 + w - 8, LIST_Y + 2, SCR_ROWS * ROW_H - 4, *scroll, SCR_ROWS, count);
}

static void vod_card(int x0, const ScrVodCard *c);

void scr_dual(const ScrDual *d)
{
    ui_header(d->title, d->subtitle, d->right);
    if (d->preview && d->focus == 1) {                       /* 0.8.0: the list and a film's card beside it */
        vita2d_draw_rectangle(0, UI_HEADER_H + 2, 40, UI_FOOTER_Y - UI_HEADER_H - 2, RGBA8(18, 21, 31, 255));
        vita2d_draw_rectangle(40, UI_HEADER_H + 2, 2, UI_FOOTER_Y - UI_HEADER_H - 2, UI_LINE);
        ui_fill_triangle(26, 266, 14, 278, 26, 290, UI_DIM);  /* (the categories are there, to the left) */
        if (d->rcount == 0 && d->rempty) ui_text_center(306, 270, UI_DIM, LIST_FS, d->rempty);
        list_area(50, 520, d->rcount, d->rsel, d->rscroll, d->rrow, d->ctx, 1, 1, 0);
        vod_card(570, d->preview);
        ui_footer(d->hints, d->nhints, d->status, d->status_err);
        return;
    }
    const int LW = 300;
    vita2d_draw_rectangle(0, UI_HEADER_H + 2, LW, UI_FOOTER_Y - UI_HEADER_H - 2, RGBA8(18, 21, 31, 255));
    vita2d_draw_rectangle(LW, UI_HEADER_H + 2, 2, UI_FOOTER_Y - UI_HEADER_H - 2, d->focus == 0 ? UI_ACCENT : UI_LINE);
    list_area(8, LW - 4, d->lcount, d->lsel, d->lscroll, d->lrow, d->ctx, d->focus == 0, d->lmark, 0);
    if (d->rcount == 0 && d->rempty) ui_text_center(LW + (960 - LW) / 2, 270, UI_DIM, LIST_FS, d->rempty);
    list_area(LW + 12, 960 - LW - 14, d->rcount, d->rsel, d->rscroll, d->rrow, d->ctx, d->focus == 1, 1, 1);
    ui_footer(d->hints, d->nhints, d->status, d->status_err);
}

/* ===== 0.8.0: programme guide, catch-up, film / series details ===== */
#define G_ARCH   RGBA8(120, 200, 255, 255)
#define G_RED    RGBA8(255, 80, 90, 255)
#define G_BLOCK  RGBA8(34, 39, 58, 255)
#define G_PAST   RGBA8(26, 29, 42, 255)
#define G_LIVE   RGBA8(44, 52, 80, 255)

/* a "rewind clock": the channel / programme can be watched again */
static void arch_icon(float cx, float cy, float r, unsigned col)
{
    static const float C[] = { 0.766f, 0.643f, 0.5f, 0.866f, 0.174f, 0.985f, -0.174f, 0.985f, -0.5f, 0.866f, -0.766f, 0.643f,
                               -0.94f, 0.342f, -1.0f, 0.0f, -0.94f, -0.342f, -0.766f, -0.643f, -0.5f, -0.866f, -0.174f, -0.985f,
                               0.174f, -0.985f, 0.5f, -0.866f, 0.766f, -0.643f, 0.94f, -0.342f, 1.0f, 0.0f };
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i += 2) vita2d_draw_fill_circle(cx + r * C[i], cy - r * C[i + 1], 1.6f, col);
    float ex = cx + r * 0.766f, ey = cy - r * 0.643f;
    ui_fill_triangle(ex - 5, ey - 5, ex + 5, ey - 5, ex, ey + 4, col);
    vita2d_draw_rectangle(cx - 1, cy - r * 0.6f, 2.5f, r * 0.7f, col);
    vita2d_draw_rectangle(cx - 1, cy, r * 0.55f, 2.5f, col);
}

static void hhmm_local(int64_t t, int tz_min, char *b, size_t n)
{
    int64_t m = (t / 60 + tz_min) % 1440;
    if (m < 0) m += 1440;
    snprintf(b, n, "%02d:%02d", (int)(m / 60), (int)(m % 60));
}

static void bar(int x, int y, int w, int h, float f, unsigned bg, unsigned fg)
{
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    ui_round_rect(x, y, w, h, h / 2, bg);
    if (f > 0) { int fw = (int)(w * f); ui_round_rect(x, y, fw < h ? h : fw, h, h / 2, fg); }
}

/* the poster texture fitted into the box (a frame with a spinner while it loads, else a plain frame) */
static void poster_box(int x, int y, int w, int h, const vita2d_texture *t, int loading, unsigned t_ms)
{
    ui_round_rect(x - 3, y - 3, w + 6, h + 6, 8, UI_LINE);
    if (!t) {
        vita2d_draw_rectangle(x, y, w, h, UI_PANEL2);
        if (loading) ui_spinner(x + w / 2, y + h / 2, t_ms);
        else ui_logo(x + w / 2, y + h / 2, w / 3);
        return;
    }
    vita2d_draw_rectangle(x, y, w, h, RGBA8(10, 12, 18, 255));
    float tw = (float)vita2d_texture_get_width(t), th = (float)vita2d_texture_get_height(t);
    float sc = w / tw < h / th ? w / tw : h / th;
    vita2d_draw_texture_scale(t, x + (w - tw * sc) / 2, y + (h - th * sc) / 2, sc, sc);
}

static void stars(int x, int y, float v)
{
    for (int i = 0; i < 5; i++) ui_star(x + i * 22, y, 9, (i + 0.5f) * 2 <= v ? UI_FAV : UI_LINE);
}

static unsigned g_card_ms;

/* the short card beside a film list */
static void vod_card(int x0, const ScrVodCard *c)
{
    int w = 960 - x0;
    vita2d_draw_rectangle(x0, UI_HEADER_H + 2, w, UI_FOOTER_Y - UI_HEADER_H - 2, RGBA8(20, 23, 34, 255));
    const int pw = 150, ph = 225;
    poster_box(x0 + 20, 78, pw, ph, c->poster, c->loading, g_card_ms += 16);
    int x = x0 + 20 + pw + 16, tw = 960 - x - 12, y = 98;
    if (c->title) { ui_text_wrap(x, y, UI_TEXT, 1.1f, c->title, tw, 26, 2, 0); y += c->title && ui_text_w(1.1f, c->title) > tw ? 52 : 26; }
    if (c->line && c->line[0]) { ui_text_fit(x, y, UI_DIM, 0.9f, c->line, tw); y += 24; }
    if (c->rating >= 0) { stars(x + 8, y - 6, c->rating); y += 26; }
    if (c->genre && c->genre[0]) { ui_text_wrap(x, y, UI_DIM, 0.9f, c->genre, tw, 22, 2, 0); y += 24; }
    if (c->director && c->director[0]) ui_text_fit(x, y, UI_DIM, 0.9f, c->director, tw);
    if (c->plot && c->plot[0]) ui_text_wrap(x0 + 20, 78 + ph + 32, UI_TEXT, 0.9f, c->plot, w - 40, 22, 7, 0);
}

void scr_guide(const ScrGuide *g)
{
    int player = g->pic != NULL;
    int gx = player ? 200 : 236, gw = 960 - gx - 10;
    int rows = player ? SCR_GUIDE_ROWS_PLAYER : SCR_GUIDE_ROWS_LIST;
    int ruler_y = player ? 204 : 62, ry0 = ruler_y + 36, rh = 42;
    int span = g->span_min > 0 ? g->span_min : 150;
    int64_t t1 = g->t0 + (int64_t)span * 60;
    float ppm = (float)gw / span;
    if (player) {                                            /* the selected programme and the picture, at the top */
        vita2d_draw_rectangle(0, 0, 960, 200, UI_PANEL);
        vita2d_draw_rectangle(0, 200, 960, 2, UI_ACCENT);
        ui_round_rect(622, 14, 324, 176, 6, UI_LINE);
        vita2d_draw_rectangle(624, 16, 320, 172, RGBA8(0, 0, 0, 255));
        float sx = 320.0f / g->pic_w, sy = 172.0f / (g->pic_fields ? g->pic_h / 2 : g->pic_h);
        vita2d_draw_texture_part_scale(g->pic, 624, 16, 0, 0, g->pic_w, g->pic_fields ? g->pic_h / 2 : g->pic_h, sx, sy);
        if (g->pic_label) { vita2d_draw_rectangle(624, 160, 320, 28, RGBA8(0, 0, 0, 160)); ui_text_fit(634, 180, UI_TEXT, 0.9f, g->pic_label, 300); }
        if (g->subtitle || g->right) {
            char top[160];
            snprintf(top, sizeof top, "%s%s%s", g->right ? g->right : "", g->right && g->subtitle ? "  \xC2\xB7  " : "", g->subtitle ? g->subtitle : "");
            ui_text_fit(20, 34, UI_DIM, 0.95f, top, 580);
        }
        if (g->sel_title) { ui_text_fit(20, 72, UI_TEXT, 1.4f, g->sel_title, 590); ui_text_fit(21, 72, UI_TEXT, 1.4f, g->sel_title, 590); }
        if (g->sel_line) ui_text_fit(20, 100, UI_DIM, 1.0f, g->sel_line, 590);
        int y = 126;
        if (g->sel_live) { bar(20, y - 8, 260, 6, g->sel_progress, UI_LINE, UI_ACCENT); if (g->sel_left) ui_text(292, y, UI_DIM, 0.9f, g->sel_left); y += 22; }
        else if (g->sel_archive && g->sel_note) { arch_icon(28, y - 6, 7, G_ARCH); ui_text(44, y, G_ARCH, 0.92f, g->sel_note); y += 22; }
        if (g->sel_desc) ui_text_wrap(20, y + 6, UI_TEXT, 0.9f, g->sel_desc, 580, 22, (196 - y) / 22, 0);
    } else ui_header(g->title, g->subtitle, g->right);
    vita2d_draw_rectangle(0, ruler_y, 960, 34, UI_PANEL);
    if (g->day) ui_text_fit(18, ruler_y + 24, UI_DIM, 1.0f, g->day, gx - 30);
    for (int m = 0; m <= span; m += 30) {
        int x = gx + (int)(m * ppm);
        char b[8];
        hhmm_local(g->t0 + (int64_t)m * 60, g->tz_min, b, sizeof b);
        if (m < span) ui_text(x + 6, ruler_y + 24, UI_TEXT, 1.0f, b);
        vita2d_draw_rectangle(x, ruler_y + 8, 1, 22, UI_LINE);
    }
    for (int k = 0; k < rows && g->first + k < g->count; k++) {
        ScrGuideRow r;
        memset(&r, 0, sizeof r);
        g->row(g->first + k, &r, g->ctx);
        int y = ry0 + k * rh;
        vita2d_draw_rectangle(0, y, gx - 4, rh - 4, r.sel ? UI_PANEL2 : UI_PANEL);
        if (r.playing) vita2d_draw_rectangle(0, y, 4, rh - 4, UI_OK);
        int nx = player ? 12 : 44;
        if (!player && r.number > 0) { char nb[12]; snprintf(nb, sizeof nb, "%d", r.number); ui_text(38 - ui_text_w(0.9f, nb), y + 26, UI_DIM, 0.9f, nb); }
        ui_text_fit(nx, y + 27, UI_TEXT, player ? 1.0f : 1.05f, r.name, gx - nx - 16 - (r.archive ? 22 : 0));
        if (r.archive) arch_icon(gx - 20, y + 19, 7, G_ARCH);
        if (!r.nprogs) {
            vita2d_draw_rectangle(gx + 2, y, gw - 4, rh - 4, G_PAST);
            if (r.loading) ui_text(gx + 14, y + 26, UI_DIM, 0.95f, T("Loading..."));
            else ui_text(gx + 14, y + 26, UI_DIM, 0.95f, T(r.noguide ? "No programme guide" : "No programme information"));
        }
        for (int i = 0; i < r.nprogs; i++) {
            const ScrProg *p = &r.progs[i];
            int64_t a = p->start < g->t0 ? g->t0 : p->start, b = p->end > t1 ? t1 : p->end;
            if (b <= a) continue;
            int x = gx + (int)((a - g->t0) / 60.0f * ppm) + 2, w = (int)((b - a) / 60.0f * ppm) - 4;
            if (w < 4) continue;
            int sel = p->flags & SCR_PROG_SEL, past = p->flags & SCR_PROG_DIM;
            ui_round_rect(x, y, w, rh - 4, 6, sel ? UI_SEL : (p->flags & SCR_PROG_LIVE) ? G_LIVE : past ? G_PAST : G_BLOCK);
            if (sel) { vita2d_draw_rectangle(x, y, w, 2, UI_ACCENT); vita2d_draw_rectangle(x, y + rh - 6, w, 2, UI_ACCENT); }
            int tx = x + 10;
            if (p->start < g->t0) { ui_fill_triangle(x + 3, y + 19, x + 9, y + 13, x + 9, y + 25, UI_DIM); tx = x + 14; }
            if ((p->flags & SCR_PROG_ARCHIVE) && past && w > 44) { arch_icon(tx + 6, y + 19, 6, G_ARCH); tx += 20; }
            if (w - (tx - x) - 6 > 12) ui_text_fit(tx, y + 26, past && !sel ? UI_DIM : UI_TEXT, 1.0f, p->title, w - (tx - x) - 8);
        }
    }
    if (g->count == 0 && g->empty) ui_text_center(gx + gw / 2, ry0 + 3 * rh, UI_DIM, 1.1f, g->empty);
    if (g->now >= g->t0 && g->now < t1) {                    /* now */
        int nx = gx + (int)((g->now - g->t0) / 60.0f * ppm);
        vita2d_draw_rectangle(nx - 1, ruler_y + 4, 3, ry0 + rows * rh - ruler_y - 8, G_RED);
        ui_fill_triangle(nx - 6, ruler_y + 2, nx + 7, ruler_y + 2, nx + 1, ruler_y + 10, G_RED);
    }
    if (!player) {                                           /* the selected programme, under the grid */
        int y = ry0 + rows * rh + 2;
        vita2d_draw_rectangle(0, y, 960, UI_FOOTER_Y - y, RGBA8(20, 23, 34, 255));
        if (g->sel_title) {
            int tw = ui_text_w(1.2f, g->sel_title);
            if (tw > 520) tw = 520;
            ui_text_fit(18, y + 26, UI_TEXT, 1.2f, g->sel_title, 520);
            ui_text_fit(19, y + 26, UI_TEXT, 1.2f, g->sel_title, 520);
            if (g->sel_line) ui_text_fit(30 + tw, y + 26, UI_DIM, 0.95f, g->sel_line, 930 - 30 - tw - (g->sel_live ? 80 : 0));
            int dy = y + 52;
            if (g->sel_live) {
                ui_badge(870, y + 8, T("NOW"), G_RED);
                bar(18, y + 38, 300, 6, g->sel_progress, UI_LINE, UI_ACCENT);
                if (g->sel_left) ui_text(330, y + 45, UI_DIM, 0.9f, g->sel_left);
                dy = y + 70;
            } else if (g->sel_archive && g->sel_note) {
                arch_icon(26, y + 41, 7, G_ARCH);
                ui_text(42, y + 47, G_ARCH, 0.95f, g->sel_note);
                dy = y + 70;
            }
            if (g->sel_desc && g->sel_desc[0]) ui_text_wrap(18, dy, UI_DIM, 0.92f, g->sel_desc, 920, 22, (UI_FOOTER_Y - dy + 12) / 22, 0);
        }
    }
    ui_footer(g->hints, g->nhints, NULL, 0);
    if (g->drawer) {                                         /* the categories, over the left of the guide */
        vita2d_draw_rectangle(0, UI_HEADER_H + 2, 960, UI_FOOTER_Y - UI_HEADER_H - 2, RGBA8(0, 0, 0, 120));
        vita2d_draw_rectangle(0, UI_HEADER_H + 2, 300, UI_FOOTER_Y - UI_HEADER_H - 2, RGBA8(18, 21, 31, 252));
        vita2d_draw_rectangle(300, UI_HEADER_H + 2, 2, UI_FOOTER_Y - UI_HEADER_H - 2, UI_ACCENT);
        list_area(8, 292, g->dcount, g->dsel, g->dscroll, g->drow, g->ctx, 1, 1, 0);
    }
    if (g->popup) {
        vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 140));
        ui_panel(240, 170, 480, 190, RGBA8(24, 28, 42, 250));
        if (g->popup_title) ui_text_center(480, 212, UI_TEXT, 1.25f, g->popup_title);
        if (g->popup_line) ui_text_center(480, 242, UI_DIM, 0.95f, g->popup_line);
        const char *it[2] = { g->popup_a, g->popup_b };
        for (int i = 0; i < 2; i++) {
            if (!it[i]) continue;
            int y = 268 + i * 46;
            if (i == g->popup_sel) ui_round_rect(270, y, 420, 38, 8, UI_SEL);
            ui_text_center(480, y + 26, i == g->popup_sel ? UI_TEXT : UI_DIM, 1.1f, it[i]);
        }
    }
}

void scr_chan_guide(const ScrChanGuide *c)
{
    ui_header(c->title, c->subtitle, c->right);
    int pill = c->ndays > 0 ? (936 / c->ndays) - 8 : 96;
    if (pill > 96) pill = 96;
    for (int i = 0; i < c->ndays; i++) {
        int x = 14 + i * (pill + 8);
        ui_round_rect(x, 70, pill, 32, 16, i == c->day ? UI_ACCENT : UI_PANEL2);
        ui_text_center(x + pill / 2, 92, i == c->day ? UI_TEXT : UI_DIM, 0.92f, c->days[i]);
    }
    int sel = c->sel, *scroll = c->scroll;
    if (sel < *scroll) *scroll = sel;
    if (sel >= *scroll + SCR_CG_ROWS) *scroll = sel - SCR_CG_ROWS + 1;
    if (*scroll > c->count - SCR_CG_ROWS) *scroll = c->count - SCR_CG_ROWS;
    if (*scroll < 0) *scroll = 0;
    if (!c->count && c->empty) ui_text_center(300, 280, UI_DIM, 1.1f, c->empty);
    for (int k = 0; k < SCR_CG_ROWS && *scroll + k < c->count; k++) {
        int i = *scroll + k, y = 110 + k * 42;
        ScrCGRow r;
        memset(&r, 0, sizeof r);
        c->row(i, &r, c->ctx);
        int on = i == sel;
        if (on) { vita2d_draw_rectangle(0, y, 600, 40, UI_SEL); vita2d_draw_rectangle(0, y, 4, 40, UI_ACCENT); }
        else if (k % 2) vita2d_draw_rectangle(0, y, 600, 40, RGBA8(18, 21, 31, 255));
        unsigned col = r.archive && !r.live && !on ? UI_DIM : UI_TEXT;
        if (r.time) ui_text(18, y + 27, col, 1.05f, r.time);
        if (r.archive && !r.live) arch_icon(96, y + 20, 7, G_ARCH);
        ui_text_fit(116, y + 27, col, 1.1f, r.title, r.live ? 390 : 470);
        if (r.live) { ui_badge(520, y + 9, T("NOW"), G_RED); bar(116, y + 33, 380, 4, r.progress, UI_LINE, UI_ACCENT); }
    }
    ui_scrollbar(604, 112, SCR_CG_ROWS * 42 - 4, *scroll, SCR_CG_ROWS, c->count);
    ui_panel(612, 110, 336, 376, UI_PANEL);
    int y = 144;
    if (c->d_title) { ui_text_wrap(628, y, UI_TEXT, 1.2f, c->d_title, 304, 28, 2, 0); y += ui_text_w(1.2f, c->d_title) > 304 ? 56 : 28; }
    if (c->d_line1) { ui_text_fit(628, y, UI_DIM, 0.95f, c->d_line1, 304); y += 24; }
    if (c->d_line2) { ui_text_fit(628, y, UI_DIM, 0.95f, c->d_line2, 304); y += 24; }
    if (c->d_archive) { arch_icon(636, y + 4, 7, G_ARCH); ui_text_fit(652, y + 10, G_ARCH, 0.95f, T("In the archive: watch from the start"), 290); y += 32; }
    if (c->d_desc) ui_text_wrap(628, y + 10, UI_TEXT, 0.92f, c->d_desc, 306, 23, (470 - y) / 23, 0);
    ui_footer(c->hints, c->nhints, NULL, 0);
}

void scr_vod_detail(const ScrVodDetail *d)
{
    const ScrVodCard *c = &d->card;
    if (d->overlay) vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(8, 10, 16, 215));
    else ui_header(d->header, d->header_sub, NULL);
    int top = d->overlay ? 28 : 80;
    poster_box(30, top, 240, 360, c->poster, c->loading, g_card_ms += 16);
    int x = 300, tw = 640, y = top + 26;
    if (c->title) { ui_text_wrap(x, y, UI_TEXT, 1.55f, c->title, tw, 34, 2, 0); ui_text_wrap(x + 1, y, UI_TEXT, 1.55f, c->title, tw, 34, 2, 0);
                    y += ui_text_w(1.55f, c->title) > tw ? 64 : 30; }
    if (c->line && c->line[0]) { ui_text_fit(x, y, UI_DIM, 1.0f, c->line, tw); y += 30; }
    if (c->rating >= 0) {
        char rb[24];
        stars(x + 10, y - 6, c->rating);
        snprintf(rb, sizeof rb, "%.1f / 10", c->rating);
        for (char *q = rb; *q; q++) if (*q == '.' && lang_get() == LANG_TR) *q = ',';
        ui_text(x + 122, y, UI_TEXT, 1.0f, rb);
        y += 30;
    }
    if (c->genre && c->genre[0]) { ui_text_fit(x, y, UI_DIM, 0.95f, c->genre, tw); y += 30; }
    int bottom = d->overlay ? 496 : 372;
    if (c->plot && c->plot[0]) {
        int lines = (bottom - y - (c->director || c->cast ? 56 : 0)) / 25;
        if (lines > 0) y += 25 * ui_text_wrap(x, y + 4, UI_TEXT, 0.98f, c->plot, tw, 25, lines > 8 ? 8 : lines, 0) + 10;
    }
    if (c->director && c->director[0] && y < bottom) { ui_text(x, y + 4, UI_DIM, 0.95f, T("Director:")); ui_text_fit(x + 110, y + 4, UI_TEXT, 0.95f, c->director, tw - 110); y += 26; }
    if (c->cast && c->cast[0] && y < bottom + 20) { ui_text(x, y + 4, UI_DIM, 0.95f, T("Cast:")); ui_text_fit(x + 110, y + 4, UI_TEXT, 0.95f, c->cast, tw - 110); }
    if (!d->overlay && d->nbuttons > 0) {
        int bx = x, bw[3] = { 300, 150, 170 };
        for (int i = 0; i < d->nbuttons && i < 3; i++) {
            int w = bw[i];
            int on = i == d->sel;
            unsigned fill = i == 0 ? (on ? UI_ACCENT : ui_theme.button) : (on ? UI_SEL : UI_PANEL2);
            ui_round_rect(bx, 384, w, 44, 10, fill);
            if (on && i) { vita2d_draw_rectangle(bx + 10, 424, w - 20, 2, UI_ACCENT); }
            if (i == 0) {
                ui_fill_triangle(bx + 20, 394, bx + 20, 418, bx + 38, 406, UI_TEXT);
                ui_text_fit(bx + 48, d->progress >= 0 ? 410 : 414, UI_TEXT, 1.1f, d->buttons[i], w - 58);
                if (d->progress >= 0) bar(bx + 48, 418, w - 70, 3, d->progress, ui_theme.sel_dark, UI_TEXT);
            } else if (i == 2) {
                ui_star(bx + 26, 406, 10, d->fav ? UI_FAV : UI_DIM);
                ui_text_fit(bx + 44, 414, UI_TEXT, 1.1f, d->buttons[i], w - 54);
            } else ui_text_center(bx + w / 2, 414, UI_TEXT, 1.1f, d->buttons[i]);
            bx += w + 12;
        }
    }
    if (d->note && !d->overlay) ui_text_fit(x, 458, UI_DIM, 0.85f, d->note, tw);
    ui_footer(d->hints, d->nhints, NULL, 0);
}

void scr_series(const ScrSeries *s)
{
    const ScrVodCard *c = &s->card;
    ui_header(s->header, s->header_sub, NULL);
    poster_box(24, 76, 168, 252, c->poster, c->loading, g_card_ms += 16);
    int x = 210, tw = 730, y = 100;
    if (c->title) { ui_text_fit(x, y, UI_TEXT, 1.45f, c->title, tw); ui_text_fit(x + 1, y, UI_TEXT, 1.45f, c->title, tw); y += 28; }
    if (c->line && c->line[0]) { ui_text_fit(x, y, UI_DIM, 0.95f, c->line, tw); y += 24; }
    if (c->rating >= 0) {
        char rb[24];
        stars(x + 10, y - 6, c->rating);
        snprintf(rb, sizeof rb, "%.1f / 10", c->rating);
        for (char *q = rb; *q; q++) if (*q == '.' && lang_get() == LANG_TR) *q = ',';
        ui_text(x + 122, y, UI_TEXT, 0.95f, rb);
        y += 24;
    }
    if (c->plot && c->plot[0]) ui_text_wrap(x, y + 6, UI_DIM, 0.92f, c->plot, tw, 22, (236 - y) / 22, 0);
    int px = x;
    for (int i = 0; i < s->nseasons; i++) {                  /* the seasons (scrolling when there are many) */
        int first = s->season > 5 ? s->season - 5 : 0;
        if (i < first) continue;
        int w = ui_text_w(0.95f, s->seasons[i]) + 30;
        if (w < 104) w = 104;
        if (px + w > 950) { ui_fill_triangle(944, 252, 952, 260, 944, 268, UI_DIM); break; }
        ui_round_rect(px, 244, w, 32, 16, i == s->season ? UI_ACCENT : UI_PANEL2);
        ui_text_center(px + w / 2, 266, i == s->season ? UI_TEXT : UI_DIM, 0.95f, s->seasons[i]);
        px += w + 8;
    }
    int sel = s->sel, *scroll = s->scroll;
    if (sel < *scroll) *scroll = sel;
    if (sel >= *scroll + SCR_EP_ROWS) *scroll = sel - SCR_EP_ROWS + 1;
    if (*scroll > s->count - SCR_EP_ROWS) *scroll = s->count - SCR_EP_ROWS;
    if (*scroll < 0) *scroll = 0;
    for (int k = 0; k < SCR_EP_ROWS && *scroll + k < s->count; k++) {
        int i = *scroll + k, y2 = 286 + k * 40;
        ScrEpRow r;
        memset(&r, 0, sizeof r);
        s->row(i, &r, s->ctx);
        if (i == sel) { vita2d_draw_rectangle(x - 10, y2, 960 - x, 38, UI_SEL); vita2d_draw_rectangle(x - 10, y2, 4, 38, UI_ACCENT); }
        if (r.check) ui_check(x + 12, y2 + 19, 8, RGBA8(64, 226, 160, 255));
        int rw = r.right ? ui_text_w(0.95f, r.right) : 0;
        ui_text_fit(x + 34, y2 + 26, UI_TEXT, 1.05f, r.name, r.progress > 0 ? 400 : 690 - rw);
        if (r.right) ui_text(940 - rw, y2 + 26, UI_DIM, 0.95f, r.right);
        if (r.progress > 0) bar(x + 420, y2 + 16, 180, 5, r.progress, ui_theme.accent_dim, UI_TEXT);
    }
    ui_scrollbar(952, 288, SCR_EP_ROWS * 40 - 4, *scroll, SCR_EP_ROWS, s->count);
    ui_footer(s->hints, s->nhints, s->status, 0);
}
