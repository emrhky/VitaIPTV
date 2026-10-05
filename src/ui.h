#ifndef UI_H
#define UI_H
/* Small drawing kit on top of vita2d: bars, PlayStation button glyphs, badges, spinner, meter. */
#include <vita2d.h>

#define UI_BG      RGBA8(14, 16, 24, 255)
#define UI_PANEL   RGBA8(26, 30, 44, 255)
#define UI_PANEL2  RGBA8(36, 41, 60, 255)
#define UI_LINE    RGBA8(58, 64, 90, 255)
#define UI_ACCENT  RGBA8(70, 130, 255, 255)
#define UI_SEL     RGBA8(42, 72, 148, 255)
#define UI_TEXT    RGBA8(236, 238, 245, 255)
#define UI_DIM     RGBA8(142, 148, 170, 255)
#define UI_ERR     RGBA8(255, 110, 110, 255)
#define UI_OK      RGBA8(90, 220, 140, 255)
#define UI_WARN    RGBA8(255, 196, 90, 255)
#define UI_SHADE   RGBA8(0, 0, 0, 170)

enum { UI_BTN_CROSS, UI_BTN_CIRCLE, UI_BTN_TRIANGLE, UI_BTN_SQUARE, UI_BTN_L, UI_BTN_R,
       UI_BTN_START, UI_BTN_SELECT, UI_BTN_DPAD, UI_BTN_UPDOWN, UI_BTN_LR };

typedef struct { int btn; const char *label; } UiHint;

#define UI_HEADER_H  52
#define UI_FOOTER_Y  506

void ui_init(vita2d_pgf *pgf);
void ui_text(int x, int y, unsigned col, float scale, const char *s);
int  ui_text_w(float scale, const char *s);
void ui_text_fit(int x, int y, unsigned col, float scale, const char *s, int maxw);
void ui_text_center(int cx, int y, unsigned col, float scale, const char *s);
void ui_panel(int x, int y, int w, int h, unsigned fill);
void ui_header(const char *title, const char *subtitle, const char *right);
/* hints along the bottom bar; status (if any) is drawn just above it */
void ui_footer(const UiHint *hints, int n, const char *status, int status_err);
int  ui_button(int x, int cy, int btn);           /* draws a glyph starting at x, returns its width */
int  ui_badge(int x, int y, const char *txt, unsigned col);   /* y = top; returns width */
void ui_scrollbar(int x, int y, int h, int first, int visible, int total);
void ui_spinner(int cx, int cy, unsigned t_ms);
void ui_meter(int x, int y, int w, int h, int level, unsigned t_ms);   /* level 0..1000 */
/* centred message box with an optional spinner */
void ui_message(const char *title, const char *line, unsigned title_col, int spinner, unsigned t_ms);

#endif
