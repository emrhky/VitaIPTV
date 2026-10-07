#include "screens.h"
#include "lang.h"
#include <stdio.h>
#include <string.h>

#define LIST_Y   70
#define ROW_H    33

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

void scr_player(const ScrPlayer *p)
{
    if (p->backdrop) scr_backdrop();
    if (p->center_title) ui_message(p->center_title, p->center_line, p->center_col, p->spinner, p->t_ms);
    if (!p->osd) return;
    vita2d_draw_rectangle(0, 0, 960, 62, UI_SHADE);
    vita2d_draw_rectangle(0, 62, 960, 2, UI_ACCENT);
    int iw = p->info ? ui_text_w(0.9f, p->info) : 0;
    ui_text_fit(20, 30, UI_TEXT, 1.25f, p->name, 900 - iw);
    ui_text_fit(21, 30, UI_TEXT, 1.25f, p->name, 900 - iw);
    if (p->group && p->group[0]) ui_text_fit(20, 52, UI_DIM, 0.9f, p->group, 700);
    if (p->info && p->info[0]) ui_text(940 - iw, 30, UI_DIM, 0.9f, p->info);
    int y = UI_FOOTER_Y;
    if (p->stats2 && p->stats2[0]) { y -= 26; vita2d_draw_rectangle(0, y, 960, 26, UI_SHADE); ui_text_fit(18, y + 19, UI_DIM, 0.9f, p->stats2, 924); }
    if (p->stats1 && p->stats1[0]) { y -= 26; vita2d_draw_rectangle(0, y, 960, 26, UI_SHADE); ui_text_fit(18, y + 19, UI_DIM, 0.9f, p->stats1, 924); }
    ui_footer(p->hints, p->nhints, NULL, 0);
}

void scr_radio(const char *name, const char *group, const char *audio_info, int level, unsigned t_ms,
               const UiHint *hints, int nhints, const char *note)
{
    ui_header(T("Radio"), group, NULL);
    ui_badge(480 - ui_text_w(0.8f, T("ON AIR")) / 2 - 6, 96, T("ON AIR"), RGBA8(220, 60, 80, 255));
    ui_text_center(480, 168, UI_TEXT, 1.7f, name);
    ui_text_center(481, 168, UI_TEXT, 1.7f, name);
    ui_meter(160, 196, 640, 170, level, t_ms);
    vita2d_draw_rectangle(160, 368, 640, 2, UI_LINE);
    if (audio_info && audio_info[0]) ui_text_center(480, 404, UI_DIM, 1.0f, audio_info);
    if (note && note[0]) ui_text_center(480, 440, UI_DIM, 0.9f, note);
    ui_footer(hints, nhints, NULL, 0);
}

void scr_lines(const char *title, const char *subtitle, const char *const *lines, int n, const char *error,
               int busy, unsigned t_ms, const UiHint *hints, int nhints)
{
    ui_header(title, subtitle, NULL);
    if (busy) { ui_message(T("Analyzing stream..."), subtitle, UI_TEXT, 1, t_ms); ui_footer(hints, nhints, NULL, 0); return; }
    ui_panel(20, 70, 920, 420, UI_PANEL);
    int y = 104;
    if (error && error[0]) { ui_text_fit(40, y, UI_ERR, 1.05f, error, 880); y += 38; }
    for (int i = 0; i < n; i++) {
        unsigned col = UI_TEXT;
        if (!strncmp(lines[i], "Playable by hardware: YES", 25)) col = UI_OK;
        else if (!strncmp(lines[i], "Playable by hardware: NO", 24)) col = UI_ERR;
        else if (!strncmp(lines[i], "Playable by hardware: MAYBE", 27)) col = UI_WARN;
        ui_text_fit(40, y, col, 1.0f, lines[i], 880);
        y += 30;
    }
    ui_footer(hints, nhints, NULL, 0);
}

#define FORM_Y 76
#define FORM_H 46

void scr_form(const char *title, const char *subtitle, const ScrField *f, int n, int sel,
              const UiHint *hints, int nhints, const char *status, int status_err)
{
    ui_header(title, subtitle, NULL);
    for (int i = 0; i < n; i++) {
        int y = FORM_Y + i * FORM_H, on = f[i].enabled;
        unsigned lc = on ? UI_DIM : RGBA8(80, 84, 100, 255), vc = on ? UI_TEXT : RGBA8(90, 94, 110, 255);
        if (f[i].kind == SCR_F_BUTTON) {
            int bw = 300, bx = 480 - bw / 2;
            vita2d_draw_rectangle(bx, y + 6, bw, FORM_H - 12, i == sel ? UI_ACCENT : UI_PANEL2);
            ui_text_center(480, y + 30, i == sel ? RGBA8(255, 255, 255, 255) : vc, 1.05f, f[i].label);
            continue;
        }
        if (i == sel) {
            vita2d_draw_rectangle(10, y + 2, 940, FORM_H - 4, UI_SEL);
            vita2d_draw_rectangle(10, y + 2, 4, FORM_H - 4, UI_ACCENT);
        }
        ui_text(36, y + 29, i == sel ? UI_TEXT : lc, 1.0f, f[i].label);
        int bx = 290, bw = 640;
        vita2d_draw_rectangle(bx, y + 8, bw, FORM_H - 16, i == sel ? RGBA8(30, 52, 110, 255) : UI_PANEL);
        const char *v = f[i].value && f[i].value[0] ? f[i].value : (on ? T("(empty)") : "-");
        unsigned col = f[i].value && f[i].value[0] ? vc : RGBA8(100, 104, 122, 255);
        if (f[i].kind == SCR_F_CHOICE) {
            ui_text(bx + 12, y + 29, i == sel ? UI_ACCENT : lc, 1.0f, "<");
            ui_text_center(bx + bw / 2, y + 29, col, 1.0f, v);
            ui_text(bx + bw - 24, y + 29, i == sel ? UI_ACCENT : lc, 1.0f, ">");
        } else if (f[i].kind == SCR_F_TOGGLE) {
            int onv = !strcmp(v, T("On"));
            ui_badge(bx + 12, y + 14, v, onv ? RGBA8(40, 160, 90, 255) : RGBA8(110, 110, 130, 255));
        } else {
            ui_text_fit(bx + 12, y + 29, col, 1.0f, v, bw - 24);
        }
    }
    ui_footer(hints, nhints, status, status_err);
}

void scr_menu(const char *title, const char *const *items, int n, int sel)
{
    int w = 420, rh = 42, h = 64 + n * rh, x = (960 - w) / 2, y = (544 - h) / 2 - 10;
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 120));
    ui_panel(x, y, w, h, RGBA8(24, 28, 42, 245));
    ui_text_center(480, y + 36, UI_TEXT, 1.1f, title);
    vita2d_draw_rectangle(x + 20, y + 50, w - 40, 1, UI_LINE);
    for (int i = 0; i < n; i++) {
        int ry = y + 58 + i * rh;
        if (i == sel) vita2d_draw_rectangle(x + 12, ry, w - 24, rh - 6, UI_SEL);
        ui_text_center(480, ry + 26, UI_TEXT, 1.0f, items[i]);
    }
}

void scr_confirm(const char *title, const char *line)
{
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 120));
    ui_message(title, line, UI_WARN, 0, 0);
    int y = 544 / 2 + 46, x = 480 - 120;
    x += ui_button(x, y, UI_BTN_CROSS) + 8;
    ui_text(x, y + 7, UI_TEXT, 1.0f, T("Yes"));
    x = 480 + 30;
    x += ui_button(x, y, UI_BTN_CIRCLE) + 8;
    ui_text(x, y + 7, UI_TEXT, 1.0f, T("No"));
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
        if (version) ui_text(900 - ui_text_w(0.9f, version), 520, UI_DIM, 0.9f, version);
    }
    if (status && status[0]) {
        ui_spinner(480, 440, t_ms);
        ui_text_center(480, 490, UI_DIM, 0.9f, status);
    }
}

void scr_about(const char *version)
{
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 140));
    ui_panel(220, 90, 520, 340, RGBA8(24, 28, 42, 245));
    ui_logo(480, 170, 96);
    ui_text_center(480, 258, UI_TEXT, 1.4f, "Vita IPTV");
    ui_text_center(480, 286, UI_DIM, 0.9f, version);
    ui_text_center(480, 326, UI_DIM, 0.9f, T("Live TV (MPEG-TS, H.264 up to 720p, AAC) and radio."));
    ui_text_center(480, 350, UI_DIM, 0.9f, T("Other formats via the transcoding server (see README)."));
    ui_text_center(480, 400, UI_DIM, 0.9f, T("Press O to close"));
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
        if (is_sel) {
            vita2d_draw_rectangle(x0, y + 1, w - 10, ROW_H - 2, focused ? UI_SEL : UI_PANEL2);
            vita2d_draw_rectangle(x0, y + 1, 4, ROW_H - 2, focused ? UI_ACCENT : RGBA8(90, 110, 170, 255));
        } else if (r % 2) {
            vita2d_draw_rectangle(x0, y + 1, w - 10, ROW_H - 2, RGBA8(20, 23, 34, 255));
        }
        int x = x0 + 14;
        if (numbers && row.number > 0) {
            char nb[16];
            snprintf(nb, sizeof nb, "%d", row.number);
            ui_text(x0 + 54 - ui_text_w(0.85f, nb), y + 23, is_sel ? UI_TEXT : UI_DIM, 0.85f, nb);
            x = x0 + 64;
        }
        int rw = 0;
        if (row.right && row.right[0]) {
            rw = ui_text_w(0.85f, row.right);
            if (rw > 160) rw = 160;
            ui_text_fit(x0 + w - 18 - rw, y + 23, is_sel ? RGBA8(200, 210, 240, 255) : UI_DIM, 0.85f, row.right, 160);
        }
        ui_text_fit(x, y + 23, (focused || is_sel) ? UI_TEXT : RGBA8(190, 194, 210, 255), 1.0f, row.name, x0 + w - 28 - rw - x);
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
    if (d->rcount == 0 && d->rempty) ui_text_center(LW + (960 - LW) / 2, 270, UI_DIM, 1.0f, d->rempty);
    list_area(LW + 12, 960 - LW - 14, d->rcount, d->rsel, d->rscroll, d->rrow, d->ctx, d->focus == 1, 1, 1);
    ui_footer(d->hints, d->nhints, d->status, d->status_err);
}
