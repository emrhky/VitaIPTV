#include "screens.h"
#include <stdio.h>
#include <string.h>

#define LIST_Y   62
#define ROW_H    34

void scr_list(const ScrList *l)
{
    ui_header(l->title, l->subtitle, l->right);
    int sel = l->sel, *scroll = l->scroll;
    if (sel < *scroll) *scroll = sel;
    if (sel >= *scroll + SCR_ROWS) *scroll = sel - SCR_ROWS + 1;
    if (*scroll < 0) *scroll = 0;

    if (l->count == 0 && l->empty) ui_message(l->empty, NULL, UI_DIM, 0, 0);

    for (int r = 0; r < SCR_ROWS && *scroll + r < l->count; r++) {
        int i = *scroll + r, y = LIST_Y + r * ROW_H;
        ScrRow row;
        memset(&row, 0, sizeof row);
        l->row(i, &row, l->ctx);
        if (i == sel) {
            vita2d_draw_rectangle(10, y + 1, 928, ROW_H - 2, UI_SEL);
            vita2d_draw_rectangle(10, y + 1, 4, ROW_H - 2, UI_ACCENT);
        } else if (r % 2) {
            vita2d_draw_rectangle(10, y + 1, 928, ROW_H - 2, RGBA8(20, 23, 34, 255));
        }
        int x = 26;
        if (row.number > 0) {
            char nb[16];
            snprintf(nb, sizeof nb, "%d", row.number);
            ui_text(72 - ui_text_w(0.9f, nb), y + 23, i == sel ? UI_TEXT : UI_DIM, 0.9f, nb);
            x = 86;
        }
        if (row.badge) x += ui_badge(x, y + 8, row.badge, row.badge_col) + 10;
        int rw = 0;
        if (row.right && row.right[0]) {
            rw = ui_text_w(0.9f, row.right);
            if (rw > 230) rw = 230;
            ui_text_fit(926 - rw, y + 23, i == sel ? RGBA8(200, 210, 240, 255) : UI_DIM, 0.9f, row.right, 230);
        }
        ui_text_fit(x, y + 23, UI_TEXT, 1.0f, row.name, 926 - rw - 16 - x);
    }
    ui_scrollbar(946, LIST_Y + 2, SCR_ROWS * ROW_H - 4, *scroll, SCR_ROWS, l->count);
    ui_footer(l->hints, l->nhints, l->status, l->status_err);
}

void scr_loading(const char *title, const char *line, unsigned t_ms)
{
    ui_header("Vita IPTV", NULL, NULL);
    ui_message(title, line, UI_TEXT, 1, t_ms);
}

void scr_player(const ScrPlayer *p)
{
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
    ui_header("Radio", group, NULL);
    ui_badge(480 - ui_text_w(0.8f, "ON AIR") / 2 - 6, 96, "ON AIR", RGBA8(220, 60, 80, 255));
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
    if (busy) { ui_message("Analyzing stream...", subtitle, UI_TEXT, 1, t_ms); ui_footer(hints, nhints, NULL, 0); return; }
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
