#include "ui.h"
#include "lang.h"
#include "btn_icons.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static vita2d_pgf *g_pgf;
static vita2d_pvf *g_pvf;                   /* the system fonts */
static int g_turkish;                       /* 0.4.5: Turkish letters drawn as they are (the Latin font is loaded) */
static vita2d_texture *g_btn_tex;           /* 0.6.0: the button symbols, smooth (src/btn_icons.h) */
enum { BTN_I_CROSS, BTN_I_CIRCLE, BTN_I_TRIANGLE, BTN_I_SQUARE, BTN_I_DPAD, BTN_I_UPDOWN, BTN_I_LEFTRIGHT };

/* 0.6.0: the symbols into a texture, once (a row of 30 x 30 pictures) */
static void btn_icons_load(void)
{
    const int w = BTN_ICON_SIZE * BTN_ICON_COUNT, h = BTN_ICON_SIZE;
    g_btn_tex = vita2d_create_empty_texture(w, h);
    if (!g_btn_tex) return;                                  /* (then drawn from shapes as before) */
    uint8_t *dst = vita2d_texture_get_datap(g_btn_tex);
    unsigned stride = vita2d_texture_get_stride(g_btn_tex);
    for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * stride, BTN_ICONS + (size_t)y * w, (size_t)w * 4);
}

/* 0.4.5: Latin letters (Turkish ğ ş ı İ included) from the system's Latin font, the one the Vita's own menus
 * use; everything else (Cyrillic, Greek, Japanese...) from the default font as before. vita2d's default font
 * is the one for the default language only, which drew ğ ş ı badly: until now they were drawn as g s i. */
static int in_latin(unsigned int c)
{
    return c < 0x250 || (c >= 0x1E00 && c < 0x1F00) || (c >= 0x2000 && c < 0x2070) || c == 0x20AC || c == 0x2122;
}

/* 0.8.1: themes. Blue is the look of every version before. The backgrounds stay the same in all of them. */
static const UiTheme THEMES[UI_THEMES] = {
    { RGBA8(236, 238, 245, 255), RGBA8(142, 148, 170, 255), RGBA8(70, 130, 255, 255), RGBA8(42, 72, 148, 255),
      RGBA8(30, 52, 110, 255), RGBA8(90, 110, 170, 255), RGBA8(200, 210, 240, 255), RGBA8(40, 80, 170, 255) },          /* blue */
    { RGBA8(234, 246, 238, 255), RGBA8(138, 164, 148, 255), RGBA8(40, 186, 116, 255), RGBA8(26, 104, 70, 255),
      RGBA8(20, 76, 52, 255), RGBA8(80, 144, 112, 255), RGBA8(196, 236, 214, 255), RGBA8(28, 134, 86, 255) },           /* green */
    { RGBA8(242, 236, 250, 255), RGBA8(158, 146, 178, 255), RGBA8(156, 104, 246, 255), RGBA8(86, 54, 154, 255),
      RGBA8(62, 38, 116, 255), RGBA8(134, 108, 184, 255), RGBA8(222, 208, 246, 255), RGBA8(112, 72, 196, 255) },         /* purple */
    { RGBA8(250, 242, 232, 255), RGBA8(176, 158, 138, 255), RGBA8(242, 138, 36, 255), RGBA8(146, 76, 22, 255),
      RGBA8(108, 56, 16, 255), RGBA8(186, 132, 82, 255), RGBA8(246, 222, 196, 255), RGBA8(186, 100, 26, 255) },          /* orange */
};
UiTheme ui_theme = {
    RGBA8(236, 238, 245, 255), RGBA8(142, 148, 170, 255), RGBA8(70, 130, 255, 255), RGBA8(42, 72, 148, 255),
    RGBA8(30, 52, 110, 255), RGBA8(90, 110, 170, 255), RGBA8(200, 210, 240, 255), RGBA8(40, 80, 170, 255) };
void ui_set_theme(int t) { ui_theme = THEMES[t >= 0 && t < UI_THEMES ? t : 0]; }
const char *ui_theme_name(int t)
{
    static const char *const N[UI_THEMES] = { "Blue", "Green", "Purple", "Orange" };
    return N[t >= 0 && t < UI_THEMES ? t : 0];
}

void ui_init(vita2d_pgf *pgf, int plain_letters)
{
    g_pgf = pgf;
    vita2d_system_pvf_config cfg[2];
    memset(cfg, 0, sizeof cfg);
    cfg[0].code = SCE_PVF_LANGUAGE_LATIN;
    cfg[0].in_font_group = in_latin;
    cfg[1].code = SCE_PVF_DEFAULT_LANGUAGE_CODE;
    cfg[1].in_font_group = NULL;
    g_pvf = vita2d_load_system_pvf(2, cfg);
    g_turkish = g_pvf != NULL && !plain_letters;
    if (!g_pvf) g_pvf = vita2d_load_default_pvf();          /* (as before 0.4.5) */
    if (!g_btn_tex) btn_icons_load();
}

int ui_font_state(void) { return g_turkish ? 2 : g_pvf ? 1 : 0; }

void ui_shutdown(void)
{
    if (g_pvf) vita2d_free_pvf(g_pvf);
    g_pvf = NULL;
}

static void raw_text(int x, int y, unsigned col, float scale, const char *s)
{
    if (g_pvf) vita2d_pvf_draw_text(g_pvf, x, y, col, scale, s);
    else vita2d_pgf_draw_text(g_pgf, x, y, col, scale, s);
}

static int raw_w(float scale, const char *s)
{
    return g_pvf ? vita2d_pvf_text_width(g_pvf, scale, s) : vita2d_pgf_text_width(g_pgf, scale, s);
}

/* Turkish letters are drawn as their plain Latin forms (g, i, s, c, u, o), which read better than the
 * font's glyphs; everything else is drawn as it is (as in v0.1, which shows the big Xtream list fine).
 * Returns s itself if nothing changes. */
const char *ui_plain(const char *s, char *buf, size_t cap)
{
    if (g_turkish) return s;                                 /* 0.4.5: the Latin font draws them */
    const unsigned char *p = (const unsigned char *)s;
    int any = 0;
    for (; *p; p++) if (*p == 0xC3 || *p == 0xC4 || *p == 0xC5) { any = 1; break; }
    if (!any) return s;
    size_t o = 0;
    for (p = (const unsigned char *)s; *p && o + 1 < cap;) {
        char c = 0;
        if (p[0] == 0xC4) {
            switch (p[1]) { case 0x9F: c = 'g'; break; case 0x9E: c = 'G'; break; case 0xB1: c = 'i'; break; case 0xB0: c = 'I'; break; }
        } else if (p[0] == 0xC5) {
            switch (p[1]) { case 0x9F: c = 's'; break; case 0x9E: c = 'S'; break; }
        } else if (p[0] == 0xC3) {
            switch (p[1]) { case 0xA7: c = 'c'; break; case 0x87: c = 'C'; break; case 0xBC: c = 'u'; break;
                            case 0x9C: c = 'U'; break; case 0xB6: c = 'o'; break; case 0x96: c = 'O'; break; }
        }
        if (c) { buf[o++] = c; p += 2; }
        else buf[o++] = (char)*p++;
    }
    buf[o] = 0;
    return buf;
}

/* 0.4.5: every word of a short label starts with a capital. Words start after a space or "("; Turkish:
 * i -> İ, ı -> I, ç ğ ö ş ü -> Ç Ğ Ö Ş Ü. Joining words stay small (not as the first word). */
static int small_word(const char *w, size_t n)
{
    static const char *const EN[] = { "a", "an", "the", "of", "from", "to", "and", "or", "in", "on", "with", "for", "by", "at", "via" };
    static const char *const TR[] = { "ve", "ile", "veya", "de", "da" };
    for (size_t i = 0; i < sizeof EN / sizeof EN[0]; i++) if (strlen(EN[i]) == n && !strncmp(EN[i], w, n)) return 1;
    for (size_t i = 0; i < sizeof TR / sizeof TR[0]; i++) if (strlen(TR[i]) == n && !strncmp(TR[i], w, n)) return 1;
    return 0;
}

const char *ui_title(const char *s, char *buf, size_t cap)
{
    size_t o = 0;
    int start = 1, first = 1, tr = lang_get() == LANG_TR;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 3 < cap;) {
        if (start && *p != ' ' && *p != '(') {
            size_t n = strcspn((const char *)p, " ()/");
            start = 0;
            if (!first && small_word((const char *)p, n)) { first = 0; goto copy; }
            first = 0;
            if (*p == 'i' && tr) { buf[o++] = (char)0xC4; buf[o++] = (char)0xB0; p++; continue; }   /* İ */
            if (*p >= 'a' && *p <= 'z') { buf[o++] = (char)(*p - 32); p++; continue; }
            if (p[0] == 0xC4 && p[1] == 0xB1) { buf[o++] = 'I'; p += 2; continue; }                   /* ı -> I */
            if (p[0] == 0xC3 && (p[1] == 0xA7 || p[1] == 0xB6 || p[1] == 0xBC)) { buf[o++] = (char)0xC3; buf[o++] = (char)(p[1] - 0x20); p += 2; continue; }
            if ((p[0] == 0xC4 || p[0] == 0xC5) && p[1] == 0x9F) { buf[o++] = (char)p[0]; buf[o++] = (char)0x9E; p += 2; continue; }   /* ğ ş */
        }
    copy:
        if (*p == ' ' || *p == '(') start = 1;
        buf[o++] = (char)*p++;
    }
    buf[o] = 0;
    return buf;
}

/* Both fonts draw with y on the baseline. */
void ui_text(int x, int y, unsigned col, float scale, const char *s)
{
    char buf[512];
    raw_text(x, y, col, scale, ui_plain(s, buf, sizeof buf));
}

int ui_text_w(float scale, const char *s)
{
    char buf[512];
    return raw_w(scale, ui_plain(s, buf, sizeof buf));
}

/* s in buf, cut with "..." at the end when it is wider than maxw */
void ui_fit_copy(char *buf, size_t cap, float scale, const char *s, int maxw)
{
    snprintf(buf, cap, "%s", s);
    size_t n = strlen(buf);
    if (ui_text_w(scale, buf) > maxw) {
        while (n > 0) {
            n--;
            while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;
            buf[n] = 0;
            char tmp[512];
            if (n + 4 > sizeof tmp || n + 4 > cap) continue;
            memcpy(tmp, buf, n);
            memcpy(tmp + n, "...", 4);
            if (ui_text_w(scale, tmp) <= maxw) { memcpy(buf, tmp, n + 4); break; }
        }
    }
}

void ui_text_fit(int x, int y, unsigned col, float scale, const char *s, int maxw)
{
    char buf[256];
    ui_fit_copy(buf, sizeof buf, scale, s, maxw);
    ui_text(x, y, col, scale, buf);
}

void ui_text_center(int cx, int y, unsigned col, float scale, const char *s)
{
    ui_text(cx - ui_text_w(scale, s) / 2, y, col, scale, s);
}

/* 0.5.0: long notes and error messages go on to the next line instead of running off the screen. Breaks at
 * spaces (a word wider than the line is cut between letters); what does not fit in maxlines ends in "...". */
int ui_wrap(UiWrap *w, float scale, const char *s, int maxw, int maxlines)
{
    w->n = 0;
    if (!s) return 0;
    if (maxlines > UI_WRAP_LINES) maxlines = UI_WRAP_LINES;
    while (*s == ' ') s++;
    while (*s && w->n < maxlines) {
        char *out = w->line[w->n];
        const size_t cap = sizeof w->line[0];
        if (w->n == maxlines - 1) {                          /* the last line takes the rest */
            ui_fit_copy(out, cap, scale, s, maxw);
            w->n++;
            break;
        }
        size_t best = 0, i = 0;                              /* best: the longest piece ending before a space */
        while (s[i]) {
            size_t j = i;
            while (s[j] == ' ') j++;
            while (s[j] && s[j] != ' ') j++;                 /* the next word */
            if (j >= cap) break;
            char tmp[sizeof w->line[0]];
            memcpy(tmp, s, j);
            tmp[j] = 0;
            if (ui_text_w(scale, tmp) > maxw) break;
            best = i = j;
        }
        if (!s[best] && best) {                              /* all of it fits */
            memcpy(out, s, best);
            out[best] = 0;
            w->n++;
            break;
        }
        if (best == 0) {                                     /* one long word: as many letters as fit */
            size_t j = 0;
            while (s[j] && s[j] != ' ' && j + 4 < cap) {
                size_t k = j + 1;
                while (((unsigned char)s[k] & 0xC0) == 0x80) k++;
                char tmp[sizeof w->line[0]];
                memcpy(tmp, s, k);
                tmp[k] = 0;
                if (j && ui_text_w(scale, tmp) > maxw) break;
                j = k;
            }
            best = j;
        }
        memcpy(out, s, best);
        out[best] = 0;
        while (best && out[best - 1] == ' ') out[--best] = 0;
        w->n++;
        s += strlen(out);
        while (*s == ' ') s++;
    }
    return w->n;
}

int ui_text_wrap(int x, int y, unsigned col, float scale, const char *s, int maxw, int line_h, int maxlines, int center)
{
    UiWrap w;
    ui_wrap(&w, scale, s, maxw, maxlines);
    for (int i = 0; i < w.n; i++) {
        if (center) ui_text_center(x + maxw / 2, y + i * line_h, col, scale, w.line[i]);
        else ui_text(x, y + i * line_h, col, scale, w.line[i]);
    }
    return w.n;
}

void ui_panel(int x, int y, int w, int h, unsigned fill)
{
    vita2d_draw_rectangle(x, y, w, h, fill);
    vita2d_draw_rectangle(x, y, w, 1, UI_LINE);
    vita2d_draw_rectangle(x, y + h - 1, w, 1, UI_LINE);
    vita2d_draw_rectangle(x, y, 1, h, UI_LINE);
    vita2d_draw_rectangle(x + w - 1, y, 1, h, UI_LINE);
}

void ui_header(const char *title, const char *subtitle, const char *right)
{
    vita2d_draw_rectangle(0, 0, 960, UI_HEADER_H, UI_PANEL);
    vita2d_draw_rectangle(0, UI_HEADER_H, 960, 2, UI_ACCENT);
    ui_logo(36, UI_HEADER_H / 2, 40);                                   /* the app logo */
    ui_text(66, 40, UI_TEXT, 1.4f, title);
    ui_text(67, 40, UI_TEXT, 1.4f, title);                              /* faux bold */
    int x = 66 + ui_text_w(1.4f, title) + 16;
    int rw = right ? ui_text_w(1.15f, right) : 0;
    if (subtitle && subtitle[0]) ui_text_fit(x, 39, UI_DIM, 1.15f, subtitle, 940 - rw - 20 - x);
    if (right && right[0]) ui_text(940 - rw, 39, UI_DIM, 1.15f, right);
}

static void thick_line(float x0, float y0, float x1, float y1, unsigned col)
{
    /* about 3 pixels wide (0.4.0: the 2-pixel lines looked thin on the Vita) */
    vita2d_draw_line(x0, y0, x1, y1, col);
    vita2d_draw_line(x0 + 1, y0, x1 + 1, y1, col);
    vita2d_draw_line(x0, y0 + 1, x1, y1 + 1, col);
    vita2d_draw_line(x0 - 1, y0, x1 - 1, y1, col);
    vita2d_draw_line(x0, y0 - 1, x1, y1 - 1, col);
}

#define BR 15                                  /* button radius (0.4.0: 13; 30 px like the SELECT pill) */
#define K(v) ((int)((v) * 1.35f * BR / 11.0f + 0.5f))   /* glyphs were drawn for radius 11; 0.4.0: 35% bigger */

int ui_button(int x, int cy, int btn)
{
    const unsigned dark = RGBA8(64, 70, 96, 255);      /* (0.4.0: lighter, the symbols stand out) */
    int cx = x + BR;
    int ic = btn == UI_BTN_CROSS ? BTN_I_CROSS : btn == UI_BTN_CIRCLE ? BTN_I_CIRCLE : btn == UI_BTN_TRIANGLE ? BTN_I_TRIANGLE :
             btn == UI_BTN_SQUARE ? BTN_I_SQUARE : btn == UI_BTN_DPAD ? BTN_I_DPAD : btn == UI_BTN_UPDOWN ? BTN_I_UPDOWN :
             btn == UI_BTN_LEFTRIGHT ? BTN_I_LEFTRIGHT : -1;
    if (ic >= 0 && g_btn_tex) {                              /* 0.6.0: the smooth symbols, one screen pixel each */
        vita2d_draw_texture_part_scale(g_btn_tex, (float)x, (float)(cy - BTN_ICON_SIZE / 2), (float)(ic * BTN_ICON_SIZE), 0,
                                       BTN_ICON_SIZE, BTN_ICON_SIZE, 1.0f, 1.0f);
        return 2 * BR;
    }
    switch (btn) {
    case UI_BTN_CROSS: {
        unsigned c = RGBA8(124, 178, 232, 255);
        vita2d_draw_fill_circle(cx, cy, BR, dark);
        thick_line(cx - K(5), cy - K(5), cx + K(5), cy + K(5), c);
        thick_line(cx + K(5), cy - K(5), cx - K(5), cy + K(5), c);
        return 2 * BR;
    }
    case UI_BTN_CIRCLE: {
        vita2d_draw_fill_circle(cx, cy, BR, dark);
        vita2d_draw_fill_circle(cx, cy, K(7), RGBA8(255, 102, 102, 255));
        vita2d_draw_fill_circle(cx, cy, K(7) - 3, dark);
        return 2 * BR;
    }
    case UI_BTN_TRIANGLE: {
        unsigned c = RGBA8(64, 226, 160, 255);
        vita2d_draw_fill_circle(cx, cy, BR, dark);
        thick_line(cx, cy - K(6), cx - K(6), cy + K(5), c);
        thick_line(cx, cy - K(6), cx + K(6), cy + K(5), c);
        thick_line(cx - K(6), cy + K(5), cx + K(6), cy + K(5), c);
        return 2 * BR;
    }
    case UI_BTN_SQUARE: {
        vita2d_draw_fill_circle(cx, cy, BR, dark);
        vita2d_draw_rectangle(cx - K(5), cy - K(5), 2 * K(5) + 1, 2 * K(5) + 1, RGBA8(255, 105, 220, 255));
        vita2d_draw_rectangle(cx - K(5) + 3, cy - K(5) + 3, 2 * K(5) - 5, 2 * K(5) - 5, dark);
        return 2 * BR;
    }
    case UI_BTN_DPAD: case UI_BTN_UPDOWN: {
        unsigned c = RGBA8(200, 205, 220, 255);
        vita2d_draw_fill_circle(cx, cy, BR, dark);
        if (btn == UI_BTN_DPAD) vita2d_draw_rectangle(cx - K(7), cy - K(2), 2 * K(7) + 1, 2 * K(2) + 1, c);
        vita2d_draw_rectangle(cx - K(2), cy - K(7), 2 * K(2) + 1, 2 * K(7) + 1, c);
        return 2 * BR;
    }
    case UI_BTN_LEFTRIGHT: {                           /* d-pad left / right: two small arrows */
        unsigned c = RGBA8(200, 205, 220, 255);
        vita2d_draw_fill_circle(cx, cy, BR, dark);
        ui_fill_triangle(cx - K(7), cy, cx - K(2), cy - K(5), cx - K(2), cy + K(5), c);
        ui_fill_triangle(cx + K(7), cy, cx + K(2), cy - K(5), cx + K(2), cy + K(5), c);
        return 2 * BR;
    }
    default: {
        const char *t = btn == UI_BTN_L ? "L" : btn == UI_BTN_R ? "R" : btn == UI_BTN_LR ? "L/R" :
                        btn == UI_BTN_START ? "START" : "SELECT";
        /* a light rounded pill with bold white text: small grey text on dark looked faded on the Vita */
        const float sc = 1.0f;
        const int h = 2 * BR, r = BR;
        int w = ui_text_w(sc, t) + 24;
        unsigned bg = RGBA8(86, 96, 128, 255), fg = RGBA8(255, 255, 255, 255);
        vita2d_draw_rectangle(x + r, cy - r, w - 2 * r, h, bg);
        vita2d_draw_fill_circle(x + r, cy, r, bg);
        vita2d_draw_fill_circle(x + w - r, cy, r, bg);
        ui_text(x + 12, cy + 7, fg, sc, t);
        ui_text(x + 13, cy + 7, fg, sc, t);                 /* second pass: bolder strokes */
        return w;
    }
    }
}

/* the width ui_button draws (without drawing) */
static int button_w(int btn)
{
    if (btn == UI_BTN_L || btn == UI_BTN_R || btn == UI_BTN_LR || btn == UI_BTN_START || btn == UI_BTN_SELECT) {
        const char *t = btn == UI_BTN_L ? "L" : btn == UI_BTN_R ? "R" : btn == UI_BTN_LR ? "L/R" : btn == UI_BTN_START ? "START" : "SELECT";
        return ui_text_w(1.0f, t) + 24;
    }
    return 2 * BR;
}

void ui_footer(const UiHint *hints, int n, const char *status, int status_err)
{
    if (status && status[0]) {                               /* 0.5.0: a long one takes two lines */
        UiWrap w;
        ui_wrap(&w, 1.0f, status, 924, 2);
        int h = 30 + (w.n - 1) * 24;
        vita2d_draw_rectangle(0, UI_FOOTER_Y - h, 960, h, RGBA8(20, 22, 32, 230));
        for (int i = 0; i < w.n; i++)
            ui_text(18, UI_FOOTER_Y - h + 21 + i * 24, status_err ? UI_ERR : UI_DIM, 1.0f, w.line[i]);
    }
    vita2d_draw_rectangle(0, UI_FOOTER_Y, 960, 544 - UI_FOOTER_Y, UI_PANEL);
    vita2d_draw_rectangle(0, UI_FOOTER_Y, 960, 1, UI_LINE);
    /* 0.4.1: the hints before UI_BTN_SEP from the left, the ones after it ending at the right edge. When they
     * do not all fit, the gaps get narrower and the labels a little smaller; still too wide: the last ones go. */
    char tb[96];                                             /* (0.4.5: labels with capitals: ui_title) */
    int cy = UI_FOOTER_Y + (544 - UI_FOOTER_Y) / 2, sep = n;
    for (int i = 0; i < n; i++) if (hints[i].btn == UI_BTN_SEP) { sep = i; break; }
    /* sizes tried in turn: the gaps get narrower first, then the labels smaller */
    static const float SC[] = { 1.15f, 1.15f, 1.05f, 1.0f, 0.95f, 0.9f };
    static const int GAP[] = { 20, 14, 12, 10, 10, 8 }, PAD[] = { 8, 6, 6, 5, 5, 4 };
    float sc = 1.15f;
    int gap = 20, pad = 8, wr = 0;
    for (int k = 0; k < 6; k++) {
        sc = SC[k]; gap = GAP[k]; pad = PAD[k];
        int wl = 0;
        wr = 0;
        for (int i = 0; i < n; i++) {
            if (i == sep) continue;
            int w = button_w(hints[i].btn) + pad + ui_text_w(sc, ui_title(T(hints[i].label), tb, sizeof tb)) + gap;
            if (i < sep) wl += w; else wr += w;
        }
        if (16 + wl + wr - gap + (sep < n ? 24 : 0) <= 944) break;
    }
    int ty = cy + 8 + (int)((sc - 1.15f) * 4);
    int x = 16;
    for (int i = 0; i < sep; i++) {
        const char *label = ui_title(T(hints[i].label), tb, sizeof tb);
        int need = button_w(hints[i].btn) + pad + ui_text_w(sc, label) + gap;
        if (x + need > 950) break;
        x += ui_button(x, cy, hints[i].btn) + pad;
        ui_text(x, ty, UI_TEXT, sc, label);
        x += ui_text_w(sc, label) + gap;
    }
    if (sep >= n) return;
    int rx = 944 - (wr - gap);
    if (rx < x + 4) rx = x + 4;                              /* (the right part moves over when the left is long) */
    for (int i = sep + 1; i < n; i++) {
        const char *label = ui_title(T(hints[i].label), tb, sizeof tb);
        int need = button_w(hints[i].btn) + pad + ui_text_w(sc, label);
        if (rx + need > 950) break;
        rx += ui_button(rx, cy, hints[i].btn) + pad;
        ui_text(rx, ty, UI_TEXT, sc, label);
        rx += ui_text_w(sc, label) + gap;
    }
}

int ui_badge(int x, int y, const char *txt, unsigned col)
{
    int w = ui_text_w(0.8f, txt) + 12;
    vita2d_draw_rectangle(x, y, w, 18, col);
    ui_text(x + 6, y + 14, RGBA8(255, 255, 255, 255), 0.8f, txt);
    return w;
}

void ui_round_rect(int x, int y, int w, int h, int r, unsigned col)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    vita2d_draw_rectangle(x + r, y, w - 2 * r, h, col);
    vita2d_draw_rectangle(x, y + r, r, h - 2 * r, col);
    vita2d_draw_rectangle(x + w - r, y + r, r, h - 2 * r, col);
    vita2d_draw_fill_circle(x + r, y + r, r, col);
    vita2d_draw_fill_circle(x + w - r - 1, y + r, r, col);
    vita2d_draw_fill_circle(x + r, y + h - r - 1, r, col);
    vita2d_draw_fill_circle(x + w - r - 1, y + h - r - 1, r, col);
}

/* A big, readable source-type tag: fixed width so the names line up, bold white letters. */
int ui_tag(int x, int y, int w, int h, const char *txt, unsigned col)
{
    ui_round_rect(x, y, w, h, 8, col);
    vita2d_draw_rectangle(x + 8, y + 1, w - 16, 2, RGBA8(255, 255, 255, 40));   /* a little light on top */
    float sc = 1.05f;
    while (sc > 0.7f && ui_text_w(sc, txt) > w - 14) sc -= 0.05f;
    int tw = ui_text_w(sc, txt);
    int ty = y + h / 2 + (int)(8 * sc);
    ui_text(x + (w - tw) / 2, ty, RGBA8(255, 255, 255, 255), sc, txt);
    ui_text(x + (w - tw) / 2 + 1, ty, RGBA8(255, 255, 255, 255), sc, txt);   /* bold */
    return w;
}

void ui_scrollbar(int x, int y, int h, int first, int visible, int total)
{
    if (total <= visible || total <= 0) return;
    vita2d_draw_rectangle(x, y, 4, h, UI_PANEL2);
    int th = h * visible / total;
    if (th < 16) th = 16;
    int ty = y + (h - th) * first / (total - visible);
    vita2d_draw_rectangle(x, ty, 4, th, UI_ACCENT);
}

void ui_spinner(int cx, int cy, unsigned t_ms)
{
    int head = (int)(t_ms / 90) % 10;
    for (int i = 0; i < 10; i++) {
        float a = (float)i * 6.2831853f / 10.0f;
        int age = (head - i + 10) % 10;
        int alpha = 255 - age * 22;
        vita2d_draw_fill_circle(cx + 20.0f * sinf(a), cy - 20.0f * cosf(a), 4.0f - age * 0.25f,
                                (ui_theme.accent & 0x00FFFFFFu) | ((unsigned)(alpha) << 24));
    }
}

void ui_meter(int x, int y, int w, int h, int level, unsigned t_ms)
{
    static float shown = 0;
    shown = shown * 0.75f + (float)level * 0.25f;            /* smooth so it does not flicker */
    const int bars = 32;
    int bw = w / bars;
    float t = (float)t_ms;
    for (int i = 0; i < bars; i++) {
        float shape = 0.55f + 0.45f * sinf(t * 0.0061f + i * 0.9f) * cosf(t * 0.0037f + i * 1.7f);
        float v = shown / 1000.0f * shape;
        if (v < 0.04f) v = 0.04f;
        int bh = (int)(v * h);
        int r = 70 + i * 3, g = 130 + i * 3;
        vita2d_draw_rectangle(x + i * bw + 2, y + h - bh, bw - 4, bh, RGBA8(r > 255 ? 255 : r, g > 255 ? 255 : g, 255, 230));
    }
}

void ui_message(const char *title, const char *line, unsigned title_col, int spinner, unsigned t_ms)
{
    title = T_msg(title);
    line = line ? T_msg(line) : NULL;
    /* 0.5.0: a long line goes on below (up to 4 lines, the box grows) instead of ending in "..." */
    const int w = 620, x = (960 - w) / 2;
    UiWrap lw;
    lw.n = 0;
    if (line && line[0]) ui_wrap(&lw, 1.0f, line, w - 40, 4);
    int extra = lw.n > 1 ? (lw.n - 1) * 26 : 0;
    int h = (spinner ? 150 : 120) + extra, y = (544 - h) / 2 - 10;
    ui_panel(x, y, w, h, RGBA8(24, 28, 42, 240));
    int ty = y + 40;
    if (spinner) { ui_spinner(480, y + 40, t_ms); ty = y + 92; }
    if (ui_text_w(1.15f, title) <= w - 40) ui_text_center(480, ty, title_col, 1.15f, title);
    else ui_text_fit(x + 20, ty, title_col, 1.15f, title, w - 40);
    for (int i = 0; i < lw.n; i++) ui_text_center(480, ty + 32 + i * 26, UI_DIM, 1.0f, lw.line[i]);
}

void ui_check(float cx, float cy, float r, unsigned col)
{
    /* two strokes, about 3 pixels thick: down to the bottom point, then up to the right */
    float ax = cx - r, ay = cy, bx = cx - r * 0.3f, by = cy + r * 0.7f, ex = cx + r, ey = cy - r * 0.8f;
    for (float o = -1.5f; o <= 1.5f; o += 0.5f) {
        vita2d_draw_line(ax, ay + o, bx, by + o, col);
        vita2d_draw_line(bx, by + o, ex, ey + o, col);
    }
}

void ui_star(float cx, float cy, float r, unsigned col)
{
    float px[10], py[10];
    for (int k = 0; k < 10; k++) {                           /* points and the corners between them, top first */
        float a = -1.5707963f + (float)k * 0.6283185f, rr = k % 2 ? r * 0.42f : r;
        px[k] = cx + rr * cosf(a);
        py[k] = cy + rr * sinf(a);
    }
    /* five points (no seams through them) and a disc for the middle */
    for (int k = 0; k < 10; k += 2) ui_fill_triangle(px[(k + 9) % 10], py[(k + 9) % 10], px[k], py[k], px[k + 1], py[k + 1], col);
    vita2d_draw_fill_circle(cx, cy, r * 0.42f, col);
}

void ui_fill_triangle(float x0, float y0, float x1, float y1, float x2, float y2, unsigned col)
{
    float ymin = fminf(y0, fminf(y1, y2)), ymax = fmaxf(y0, fmaxf(y1, y2));
    for (int y = (int)ymin; y <= (int)ymax; y++) {
        float fy = y + 0.5f, xs[3];
        int n = 0;
        float px[3] = { x0, x1, x2 }, py[3] = { y0, y1, y2 };
        for (int k = 0; k < 3; k++) {
            float ax = px[k], ay = py[k], bx = px[(k + 1) % 3], by = py[(k + 1) % 3];
            if ((fy >= ay && fy < by) || (fy >= by && fy < ay)) xs[n++] = ax + (fy - ay) * (bx - ax) / (by - ay);
        }
        if (n >= 2) {
            float a = fminf(xs[0], xs[1]), b = fmaxf(xs[0], xs[1]);
            vita2d_draw_rectangle(a, y, b - a, 1, col);
        }
    }
}

/* The app logo: the same shaded sphere as the LiveArea bubble (icon0), lit from the upper left, with a
 * gloss on the top and the white TV sign. Drawn from circles, so it scales to any size. */
void ui_logo(int cx, int cy, int size)
{
    float R = size * 0.5f;
    int steps = size / 3;
    if (steps < 10) steps = 10;
    if (steps > 48) steps = 48;
    for (int k = 0; k <= steps; k++) {                      /* from the dark rim to the lit spot */
        float t = (float)k / (float)steps;
        float rr = R * (1.0f - 0.62f * t);
        float ox = -R * 0.36f * t, oy = -R * 0.48f * t;     /* stays inside the previous circle */
        int r0, g0, b0;
        if (t < 0.5f) { float u = t * 2; r0 = (int)(70 + u * (96 - 70)); g0 = (int)(40 + u * (104 - 40)); b0 = (int)(176 + u * (238 - 176)); }
        else { float u = (t - 0.5f) * 2; r0 = (int)(96 + u * (128 - 96)); g0 = (int)(104 + u * (162 - 104)); b0 = (int)(238 + u * (255 - 238)); }
        vita2d_draw_fill_circle(cx + ox, cy + oy, rr, RGBA8(r0, g0, b0, 255));
    }
    for (int layer = 0; layer < 3; layer++) {               /* soft gloss: three nested ellipses */
        float ex = R * (0.68f - layer * 0.12f), ey = R * (0.44f - layer * 0.08f), ecy = cy - R * 0.45f;
        for (int y = (int)(ecy - ey); y <= (int)(ecy + ey); y++) {
            float dy = ((float)y + 0.5f - ecy) / ey;
            if (dy < -1.0f || dy > 1.0f) continue;
            float hw = ex * sqrtf(1.0f - dy * dy);
            vita2d_draw_rectangle(cx - hw, y, 2 * hw, 1, RGBA8(255, 255, 255, 18));
        }
    }
    unsigned w = RGBA8(255, 255, 255, 255);
    float s = size / 100.0f * 0.78f;                        /* the sign, a little smaller than the old square logo */
#define AT_X(v) (cx + ((v) - 50) * s)
#define AT_Y(v) (cy + ((v) - 50) * s)
    float tx = AT_X(18), ty = AT_Y(34), tw = 64 * s, th = 44 * s, lw = 5 * s;   /* the TV */
    if (lw < 1.5f) lw = 1.5f;
    vita2d_draw_rectangle(tx, ty, tw, lw, w);
    vita2d_draw_rectangle(tx, ty + th - lw, tw, lw, w);
    vita2d_draw_rectangle(tx, ty, lw, th, w);
    vita2d_draw_rectangle(tx + tw - lw, ty, lw, th, w);
    for (int k = -1; k <= 1; k++) {                         /* antennas */
        vita2d_draw_line(cx + k * 0.6f, ty, AT_X(36) + k * 0.6f, AT_Y(18), w);
        vita2d_draw_line(cx + k * 0.6f, ty, AT_X(64) + k * 0.6f, AT_Y(18), w);
    }
    vita2d_draw_fill_circle(AT_X(36), AT_Y(18), 3.5f * s, w);
    vita2d_draw_fill_circle(AT_X(64), AT_Y(18), 3.5f * s, w);
    ui_fill_triangle(cx - 8 * s, ty + 11 * s, cx - 8 * s, ty + th - 11 * s, cx + 12 * s, ty + th / 2, w);   /* play */
    vita2d_draw_rectangle(cx - 14 * s, ty + th + 4 * s, 28 * s, 4 * s, RGBA8(255, 255, 255, 200));            /* stand */
#undef AT_X
#undef AT_Y
}
