#include "ui.h"
#include "lang.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static vita2d_pgf *g_pgf;
static vita2d_pvf *g_pvf;                   /* the system UI font: has Turkish and other Latin letters */

void ui_init(vita2d_pgf *pgf)
{
    g_pgf = pgf;
    g_pvf = vita2d_load_default_pvf();
}

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

/* Turkish letters are drawn as their plain Latin forms (ğ->g, ı->i, ş->s, ç->c, ü->u, ö->o),
 * which reads better than the font's missing or odd glyphs. Returns s itself if nothing changes. */
const char *ui_plain(const char *s, char *buf, size_t cap)
{
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

void ui_text_fit(int x, int y, unsigned col, float scale, const char *s, int maxw)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", s);
    size_t n = strlen(buf);
    if (ui_text_w(scale, buf) > maxw) {
        while (n > 0) {
            n--;
            while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;
            buf[n] = 0;
            char tmp[256];
            if (n > sizeof tmp - 4) continue;
            memcpy(tmp, buf, n);
            memcpy(tmp + n, "...", 4);
            if (ui_text_w(scale, tmp) <= maxw) { memcpy(buf, tmp, n + 4); break; }
        }
    }
    ui_text(x, y, col, scale, buf);
}

void ui_text_center(int cx, int y, unsigned col, float scale, const char *s)
{
    ui_text(cx - ui_text_w(scale, s) / 2, y, col, scale, s);
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
    vita2d_draw_rectangle(18, 14, 6, 26, UI_ACCENT);                   /* little brand mark */
    ui_text(34, 35, UI_TEXT, 1.2f, title);
    ui_text(35, 35, UI_TEXT, 1.2f, title);                              /* faux bold */
    int x = 34 + ui_text_w(1.2f, title) + 14;
    int rw = right ? ui_text_w(1.0f, right) : 0;
    if (subtitle && subtitle[0]) ui_text_fit(x, 34, UI_DIM, 1.0f, subtitle, 940 - rw - 20 - x);
    if (right && right[0]) ui_text(940 - rw, 34, UI_DIM, 1.0f, right);
}

static void thick_line(float x0, float y0, float x1, float y1, unsigned col)
{
    vita2d_draw_line(x0, y0, x1, y1, col);
    vita2d_draw_line(x0 + 1, y0, x1 + 1, y1, col);
    vita2d_draw_line(x0, y0 + 1, x1, y1 + 1, col);
}

int ui_button(int x, int cy, int btn)
{
    const unsigned dark = RGBA8(44, 48, 66, 255);
    int cx = x + 11;
    switch (btn) {
    case UI_BTN_CROSS: {
        unsigned c = RGBA8(124, 178, 232, 255);
        vita2d_draw_fill_circle(cx, cy, 11, dark);
        thick_line(cx - 5, cy - 5, cx + 5, cy + 5, c);
        thick_line(cx + 5, cy - 5, cx - 5, cy + 5, c);
        return 22;
    }
    case UI_BTN_CIRCLE: {
        vita2d_draw_fill_circle(cx, cy, 11, dark);
        vita2d_draw_fill_circle(cx, cy, 7, RGBA8(255, 102, 102, 255));
        vita2d_draw_fill_circle(cx, cy, 5, dark);
        return 22;
    }
    case UI_BTN_TRIANGLE: {
        unsigned c = RGBA8(64, 226, 160, 255);
        vita2d_draw_fill_circle(cx, cy, 11, dark);
        thick_line(cx, cy - 6, cx - 6, cy + 5, c);
        thick_line(cx, cy - 6, cx + 6, cy + 5, c);
        thick_line(cx - 6, cy + 5, cx + 6, cy + 5, c);
        return 22;
    }
    case UI_BTN_SQUARE: {
        vita2d_draw_fill_circle(cx, cy, 11, dark);
        vita2d_draw_rectangle(cx - 5, cy - 5, 11, 11, RGBA8(255, 105, 220, 255));
        vita2d_draw_rectangle(cx - 3, cy - 3, 7, 7, dark);
        return 22;
    }
    case UI_BTN_DPAD: case UI_BTN_UPDOWN: {
        unsigned c = RGBA8(200, 205, 220, 255);
        vita2d_draw_fill_circle(cx, cy, 11, dark);
        if (btn == UI_BTN_DPAD) vita2d_draw_rectangle(cx - 7, cy - 2, 15, 5, c);
        vita2d_draw_rectangle(cx - 2, cy - 7, 5, 15, c);
        return 22;
    }
    default: {
        const char *t = btn == UI_BTN_L ? "L" : btn == UI_BTN_R ? "R" : btn == UI_BTN_LR ? "L/R" :
                        btn == UI_BTN_START ? "START" : "SELECT";
        int w = ui_text_w(0.8f, t) + 14;
        vita2d_draw_rectangle(x, cy - 10, w, 20, dark);
        vita2d_draw_rectangle(x, cy - 10, w, 1, UI_LINE);
        ui_text(x + 7, cy + 6, RGBA8(210, 214, 228, 255), 0.8f, t);
        return w;
    }
    }
}

void ui_footer(const UiHint *hints, int n, const char *status, int status_err)
{
    if (status && status[0]) {
        vita2d_draw_rectangle(0, UI_FOOTER_Y - 30, 960, 30, RGBA8(20, 22, 32, 230));
        ui_text_fit(18, UI_FOOTER_Y - 9, status_err ? UI_ERR : UI_DIM, 1.0f, status, 924);
    }
    vita2d_draw_rectangle(0, UI_FOOTER_Y, 960, 544 - UI_FOOTER_Y, UI_PANEL);
    vita2d_draw_rectangle(0, UI_FOOTER_Y, 960, 1, UI_LINE);
    int x = 16, cy = UI_FOOTER_Y + 19;
    for (int i = 0; i < n; i++) {
        const char *label = T(hints[i].label);
        int need = 22 + 8 + ui_text_w(1.0f, label) + 24;
        if (x + need > 950) break;
        x += ui_button(x, cy, hints[i].btn) + 8;
        ui_text(x, cy + 7, UI_TEXT, 1.0f, label);
        x += ui_text_w(1.0f, label) + 24;
    }
}

int ui_badge(int x, int y, const char *txt, unsigned col)
{
    int w = ui_text_w(0.8f, txt) + 12;
    vita2d_draw_rectangle(x, y, w, 18, col);
    ui_text(x + 6, y + 14, RGBA8(255, 255, 255, 255), 0.8f, txt);
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
                                RGBA8(70, 130, 255, alpha));
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
    int w = 620, h = spinner ? 150 : 120, x = (960 - w) / 2, y = (544 - h) / 2 - 10;
    ui_panel(x, y, w, h, RGBA8(24, 28, 42, 240));
    int ty = y + 40;
    if (spinner) { ui_spinner(480, y + 40, t_ms); ty = y + 92; }
    ui_text_center(480, ty, title_col, 1.15f, title);
    if (line && line[0]) {
        char buf[200];
        snprintf(buf, sizeof buf, "%s", line);
        if (ui_text_w(1.0f, buf) > w - 40) ui_text_fit(x + 20, ty + 32, UI_DIM, 1.0f, buf, w - 40);
        else ui_text_center(480, ty + 32, UI_DIM, 1.0f, buf);
    }
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

void ui_logo(int cx, int cy, int size)
{
    int x0 = cx - size / 2, y0 = cy - size / 2;
    float r = size * 0.22f;
    for (int y = 0; y < size; y++) {                        /* rounded square, blue -> violet */
        float t = (float)y / (float)size;
        unsigned c = RGBA8((int)(59 + t * (123 - 59)), (int)(108 + t * (77 - 108)), 255, 255);
        float inset = 0;
        if (y < r) inset = r - sqrtf(r * r - (r - y) * (r - y));
        else if (y > size - r) inset = r - sqrtf(r * r - (y - (size - r)) * (y - (size - r)));
        vita2d_draw_rectangle(x0 + inset, y0 + y, size - 2 * inset, 1, c);
    }
    unsigned w = RGBA8(255, 255, 255, 255);
    float s = size / 100.0f;
    float tx = x0 + 18 * s, ty = y0 + 34 * s, tw = 64 * s, th = 44 * s, lw = 5 * s;   /* the TV */
    vita2d_draw_rectangle(tx, ty, tw, lw, w);
    vita2d_draw_rectangle(tx, ty + th - lw, tw, lw, w);
    vita2d_draw_rectangle(tx, ty, lw, th, w);
    vita2d_draw_rectangle(tx + tw - lw, ty, lw, th, w);
    for (int k = -1; k <= 1; k++) {                         /* antennas */
        vita2d_draw_line(cx + k * 0.6f, ty, x0 + 36 * s + k * 0.6f, y0 + 18 * s, w);
        vita2d_draw_line(cx + k * 0.6f, ty, x0 + 64 * s + k * 0.6f, y0 + 18 * s, w);
    }
    vita2d_draw_fill_circle(x0 + 36 * s, y0 + 18 * s, 3.5f * s, w);
    vita2d_draw_fill_circle(x0 + 64 * s, y0 + 18 * s, 3.5f * s, w);
    ui_fill_triangle(cx - 8 * s, ty + 11 * s, cx - 8 * s, ty + th - 11 * s, cx + 12 * s, ty + th / 2, w);   /* play */
    vita2d_draw_rectangle(cx - 14 * s, ty + th + 4 * s, 28 * s, 4 * s, RGBA8(255, 255, 255, 200));            /* stand */
}
