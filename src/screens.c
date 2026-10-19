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
            ui_text_fit(918 - rw, y + 34, on ? RGBA8(200, 210, 240, 255) : UI_DIM, 1.0f, row.right, 230);
        }
        int maxw = 918 - rw - 16 - x;
        if (row.sub && row.sub[0]) {
            ui_text_fit(x, y + 27, UI_TEXT, 1.2f, row.name, maxw);
            if (on) ui_text_fit(x + 1, y + 27, UI_TEXT, 1.2f, row.name, maxw);
            ui_text_fit(x, y + 47, on ? RGBA8(190, 200, 230, 255) : UI_DIM, 0.9f, row.sub, maxw);
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
    y -= 40;
    vita2d_draw_rectangle(0, y, 960, 40, UI_SHADE);
    char a[16], b[16];
    int shown = p->seek_ms >= 0 ? p->seek_ms : p->pos_ms;
    fmt_clock(a, sizeof a, shown);
    if (p->dur_ms > 0) fmt_clock(b, sizeof b, p->dur_ms); else snprintf(b, sizeof b, "--:--");
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

void scr_player(const ScrPlayer *p)
{
    if (p->backdrop) scr_backdrop();
    if (p->center_title) ui_message(p->center_title, p->center_line, p->center_col, p->spinner, p->t_ms);
    if (p->paused && !p->center_title) {                    /* the pause sign */
        ui_round_rect(430, 222, 100, 100, 18, UI_PANEL);
        vita2d_draw_rectangle(456, 245, 16, 54, UI_TEXT);
        vita2d_draw_rectangle(488, 245, 16, 54, UI_TEXT);
    }
    if (!p->osd) return;
    vita2d_draw_rectangle(0, 0, 960, 62, UI_SHADE);
    vita2d_draw_rectangle(0, 62, 960, 2, UI_ACCENT);
    int iw = p->info ? ui_text_w(0.9f, p->info) : 0, nx = 20;
    if (p->fav) { ui_star(31, 23, 11, UI_FAV); nx = 50; }      /* 0.5.0: a favourite */
    ui_text_fit(nx, 30, UI_TEXT, 1.25f, p->name, 900 - iw - (nx - 20));
    ui_text_fit(nx + 1, 30, UI_TEXT, 1.25f, p->name, 900 - iw - (nx - 20));
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
        vita2d_draw_rectangle(bx, y + 5, bw, fh - 10, i == sel ? RGBA8(30, 52, 110, 255) : UI_PANEL);
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
            vita2d_draw_rectangle(x0, y + 1, 4, ROW_H - 2, focused ? UI_ACCENT : RGBA8(90, 110, 170, 255));
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
            ui_text_fit(x0 + w - 18 - rw, y + 25, is_sel ? RGBA8(200, 210, 240, 255) : UI_DIM, 1.0f, row.right, 170);
        }
        if (row.fav) { ui_star(x + 8, y + ROW_H / 2, 8, UI_FAV); x += 22; }   /* 0.5.0: a favourite */
        ui_text_fit(x, y + 26, (focused || is_sel) ? UI_TEXT : RGBA8(190, 194, 210, 255), LIST_FS, row.name, x0 + w - 28 - rw - x);
    }
    ui_scrollbar(x0 + w - 8, LIST_Y + 2, SCR_ROWS * ROW_H - 4, *scroll, SCR_ROWS, count);
}

void scr_dual(const ScrDual *d)
{
    ui_header(d->title, d->subtitle, d->right);
    const int LW = 300;
    vita2d_draw_rectangle(0, UI_HEADER_H + 2, LW, UI_FOOTER_Y - UI_HEADER_H - 2, RGBA8(18, 21, 31, 255));
    vita2d_draw_rectangle(LW, UI_HEADER_H + 2, 2, UI_FOOTER_Y - UI_HEADER_H - 2, d->focus == 0 ? UI_ACCENT : UI_LINE);
    list_area(8, LW - 4, d->lcount, d->lsel, d->lscroll, d->lrow, d->ctx, d->focus == 0, d->lmark, 0);
    if (d->rcount == 0 && d->rempty) ui_text_center(LW + (960 - LW) / 2, 270, UI_DIM, LIST_FS, d->rempty);
    list_area(LW + 12, 960 - LW - 14, d->rcount, d->rsel, d->rscroll, d->rrow, d->ctx, d->focus == 1, 1, 1);
    ui_footer(d->hints, d->nhints, d->status, d->status_err);
}
