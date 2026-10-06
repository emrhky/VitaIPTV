/* Renders every screen to PNG with sample data (PC only). Uses the real src/ui.c and src/screens.c. */
#include "screens.h"
#include "lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
void preview_init(void);
void preview_clear(unsigned c);
void preview_save(const char *path);

static void shot(const char *name)
{
    char p[256];
    snprintf(p, sizeof p, "/tmp/preview/%s.ppm", name);
    preview_save(p);
}

/* sources */
static const char *src_names[] = { "Ev internet - Xtream", "Spor paketi", "Belgesel listesi", "TRT 1 (tek link)", "[Local] test_ts_h264_aac.ts" };
static const char *src_badge[] = { "XTREAM", "XTREAM", "M3U", "STREAM", "LOCAL" };
static const unsigned src_col[] = { RGBA8(130, 80, 220, 255), RGBA8(130, 80, 220, 255), RGBA8(40, 110, 220, 255), RGBA8(30, 150, 100, 255), RGBA8(110, 110, 130, 255) };
static void src_row(int i, ScrRow *r, void *c) { (void)c; r->name = src_names[i]; r->badge = src_badge[i]; r->badge_col = src_col[i]; }

/* channels */
static const char *ch_names[] = { "TRT 1 HD", "Kanal D HD", "Show TV HD", "Star TV HD", "ATV HD", "TV8 HD", "FOX HD", "TRT Haber HD",
    "CNN Türk HD", "Habertürk HD", "NTV HD", "TRT Spor HD", "TRT Çocuk", "Minika Çocuk", "Cartoon Network", "beIN Sports 1 HD (FHD)" };
static const char *ch_groups[] = { "Ulusal", "Ulusal", "Ulusal", "Ulusal", "Ulusal", "Ulusal", "Ulusal", "Haber", "Haber", "Haber", "Haber",
    "Spor", "Çocuk", "Çocuk", "Çocuk", "Spor" };
static int show_group = 0;
static void ch_row(int i, ScrRow *r, void *c) { (void)c; r->name = ch_names[i]; r->number = i + 1; r->right = show_group ? ch_groups[i] : NULL; }
static int search_idx[] = { 12, 13 };
static void search_row(int i, ScrRow *r, void *c) { (void)c; r->name = ch_names[search_idx[i]]; r->number = search_idx[i] + 1; r->right = ch_groups[search_idx[i]]; }

static void grp_row_pv(int i, ScrRow *r, void *c)
{
    static const char *gr[] = { "All", "Ulusal", "Haber", "Spor", "Çocuk", "Belgesel", "Müzik", "Yerel", "Radyolar" };
    static const char *cnt[] = { "16", "7", "4", "2", "3", "0", "0", "0", "0" };
    (void)c; r->name = i == 0 ? T("All") : gr[i]; r->right = cnt[i];
}

int main(int argc, char **argv)
{
    (void)argv;
    preview_init();
    if (argc > 1) lang_set(LANG_TR);
    int scroll;

    UiHint hs[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_TRIANGLE, "Reload" }, { UI_BTN_START, T("Quit") } };
    ScrList sl = { "Vita IPTV", T("Sources"), T("5 sources"), 5, 0, &scroll, src_row, NULL, "No sources", hs, 3, NULL, 0 };
    preview_clear(UI_BG); scroll = 0; scr_list(&sl); shot("1_sources");

    UiHint hc[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_LR, "Group" }, { UI_BTN_DPAD, "Page" }, { UI_BTN_CIRCLE, "Back" } };
    show_group = 1;
    ScrList cl = { "Ev internet - Xtream", T("All channels"), "16 channels", 16, 3, &scroll, ch_row, NULL, T("No channels"), hc, 5, NULL, 0 };
    preview_clear(UI_BG); scroll = 0; scr_list(&cl); shot("2_channels");

    UiHint hq[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_SQUARE, "New search" }, { UI_BTN_SELECT, "Clear" }, { UI_BTN_CIRCLE, "Back" } };
    ScrList ql = { "Ev internet - Xtream", "Search: \"cocuk\"", "2 found", 2, 0, &scroll, search_row, NULL, T("Nothing found"), hq, 4, NULL, 0 };
    preview_clear(UI_BG); scroll = 0; scr_list(&ql); shot("3_search");

    preview_clear(UI_BG); scr_loading(T("Loading channels..."), "Ev internet - Xtream", 450); shot("4_loading");

    /* fake 1280x720 picture */
    static uint32_t px[1280 * 720];
    for (int y = 0; y < 720; y++) for (int x = 0; x < 1280; x++)
        px[y * 1280 + x] = RGBA8(40 + x * 120 / 1280, 60 + y * 100 / 720, 140 + (x + y) % 80, 255);
    vita2d_texture tex = { 1280, 720, px };
    UiHint hp[] = { { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_CROSS, "Info" }, { UI_BTN_TRIANGLE, "Analyze" }, { UI_BTN_CIRCLE, "Back" } };
    ScrPlayer pl = { "TRT 1 HD", "Ulusal", "1280x720  25 fps", 1, hp, 4,
                     "Video 1280x720   shown 1624   dropped 6   late 0   damaged 0   errors 0   16864 KB",
                     "Audio AAC 48000 Hz 2 ch   A/V +12 ms", NULL, NULL, 0, 0, 0 };
    preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&pl); shot("5_player_osd");

    ScrPlayer pc = { "Kanal D HD", "Ulusal", NULL, 1, hp, 4, NULL, NULL, "Connecting...", "Waiting for the stream", UI_TEXT, 1, 300 };
    preview_clear(0); scr_player(&pc); shot("6_connecting");

    ScrPlayer pe = { "beIN Sports 1 HD (FHD)", "Spor", NULL, 1, hp, 4, NULL, NULL,
                     "Cannot play this channel", "1920x1080 is above the Vita decoder limit (720p)", UI_ERR, 0, 0 };
    preview_clear(0); scr_player(&pe); shot("7_error");

    UiHint hr[] = { { UI_BTN_UPDOWN, "Station" }, { UI_BTN_CIRCLE, "Back" } };
    for (int k = 0; k < 12; k++) { preview_clear(UI_BG); scr_radio("Radyo Fenomen", "Radyolar", "AAC  48 kHz  stereo", 760, 1234 + k * 16, hr, 2, ""); }
    preview_clear(UI_BG);
    scr_radio("Radyo Fenomen", "Radyolar", "AAC  48 kHz  stereo", 760, 1234, hr, 2, T("START turns the screen off; the radio keeps playing."));
    shot("8_radio");

    const char *lines[] = { "HTTP 200, 4192 KB in 6.0 s (5694 kbit/s)", "Streams: 0x1B=h264, 0x0F=aac",
        "Video: H.264 Main L3.1  1280x720  25.00 fps  8-bit", "Keyframes: 2 (+0 other I-pictures) of 451 pictures, reference frames: 1",
        "Audio: AAC-LC 48000 Hz, 2 ch, 836 frames", "Packet problems: lost 0, damaged pictures 0, sync lost 0",
        "Playable by hardware: YES: H.264 8-bit up to 720p" };
    UiHint ha[] = { { UI_BTN_CROSS, "Again" }, { UI_BTN_TRIANGLE, "Play" }, { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_CIRCLE, "Back" } };
    preview_clear(UI_BG); scr_lines(T("Stream analysis"), "TRT 1 HD", lines, 7, NULL, 0, 0, ha, 4); shot("9_analysis");
    /* new screens */
    UiHint hs2[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_SQUARE, "Add" }, { UI_BTN_TRIANGLE, "Edit" }, { UI_BTN_SELECT, "Delete" }, { UI_BTN_START, T("Menu") } };
    ScrList sl2 = { "Vita IPTV", T("Playlists"), "5 playlists", 5, 1, &scroll, src_row, NULL, T("No playlists"), hs2, 5, T("Saved as Xtream (faster, live channels only)"), 0 };
    const char *menu[] = { T("Add playlist"), T("Settings"), T("Reload sources"), T("About"), T("Quit") };
    preview_clear(UI_BG); scroll = 0; scr_list(&sl2); scr_menu(T("Menu"), menu, 5, 1); shot("a_menu");
    preview_clear(UI_BG); scroll = 0; scr_list(&sl2); scr_confirm(T("Delete this playlist?"), "\"Spor paketi\" will be removed from the list."); shot("b_confirm");
    UiHint hf[] = { { UI_BTN_UPDOWN, "Move" }, { UI_BTN_CROSS, "Edit / choose" }, { UI_BTN_CIRCLE, T("Cancel") } };
    ScrField ff[] = { { T("Type"), T("Xtream (server + username + password)"), SCR_F_CHOICE, 1 }, { T("Name"), "Ev internet", SCR_F_TEXT, 1 },
                      { T("Server address"), "http://tv.example.com:8080", SCR_F_TEXT, 1 }, { T("Username"), "ali", SCR_F_TEXT, 1 },
                      { T("Password"), "********", SCR_F_TEXT, 1 }, { T("Save"), NULL, SCR_F_BUTTON, 1 }, { T("Cancel"), NULL, SCR_F_BUTTON, 1 } };
    preview_clear(UI_BG); scr_form(T("Add playlist"), T("Tip: a pasted get.php link fills everything"), ff, 7, 2, hf, 3, NULL, 0); shot("c_form");
    ScrField fs[] = { { T("Transcoding server"), "http://192.168.1.20:8090", SCR_F_TEXT, 1 }, { T("Use automatically"), T("On"), SCR_F_TOGGLE, 1 },
                      { T("Test server"), NULL, SCR_F_BUTTON, 1 }, { T("Save"), NULL, SCR_F_BUTTON, 1 }, { T("Cancel"), NULL, SCR_F_BUTTON, 1 } };
    preview_clear(UI_BG); scr_form(T("Settings"), T("Server: plays 1080p, HEVC, MKV, MP2/AC-3 (tools/vita_iptv_proxy.py)"), fs, 5, 2, hf, 3, T("Server is running"), 0); shot("d_settings");
    preview_clear(UI_BG); scroll = 0; scr_list(&sl2); scr_about("v1.0"); shot("e_about");
    preview_clear(UI_BG); scr_splash(NULL, "Loading playlists...", "v1.0", 900); shot("f_splash_drawn");
    /* two-pane channel screen */
    {
        static const char *gr[] = { "All", "Ulusal", "Haber", "Spor", "Çocuk", "Belgesel", "Müzik", "Yerel", "Radyolar" };
        static int gscroll2, cscroll2;
        UiHint hl[] = { { UI_BTN_UPDOWN, "Category" }, { UI_BTN_CROSS, "Channels" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" } };
        UiHint hr2[] = { { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_CROSS, "Play" }, { UI_BTN_LR, "Page" }, { UI_BTN_DPAD, "Categories" },
                         { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" } };
        for (int f = 0; f < 2; f++) {
            ScrDual d; memset(&d, 0, sizeof d);
            d.title = "Ev internet - Xtream"; d.subtitle = T("All"); d.right = "16 channels";
            d.lcount = 9; d.lsel = f ? 1 : 3; d.lscroll = &gscroll2; d.lrow = grp_row_pv;
            d.rcount = 16; d.rsel = f ? 4 : 0; d.rscroll = &cscroll2; d.rrow = ch_row;
            d.focus = f; d.lmark = 1; d.hints = f ? hr2 : hl; d.nhints = f ? 6 : 4;
            (void)gr;
            show_group = 0;
            preview_clear(UI_BG); gscroll2 = cscroll2 = 0; scr_dual(&d); shot(f ? "g_dual_right" : "g_dual_left");
        }
    }
    return 0;
}
