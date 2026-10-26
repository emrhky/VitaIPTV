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
static const char *src_badge[] = { "XTREAM", "XTREAM", "M3U", "STREAM", "FILE" };
static const char *src_sub[] = { "tv.example.net:8080", "spor.example.com", "raw.example.org", "trt.example.tr", "Turkiye_IP_TV.m3u" };
static const unsigned src_col[] = { RGBA8(124, 77, 230, 255), RGBA8(124, 77, 230, 255), RGBA8(33, 120, 235, 255), RGBA8(22, 160, 105, 255), RGBA8(222, 128, 30, 255) };
static int fav_demo;                                         /* 0.5.0: favourites in the sample data */
static void src_row(int i, ScrRow *r, void *c) { (void)c; r->name = src_names[i]; r->badge = src_badge[i]; r->badge_col = src_col[i]; r->sub = src_sub[i]; r->fav = fav_demo && i < 2; }

/* channels */
static const char *ch_names[] = { "TRT 1 HD", "Kanal D HD", "Show TV HD", "Star TV HD", "ATV HD", "TV8 HD", "FOX HD", "TRT Haber HD",
    "CNN Türk HD", "Habertürk HD", "NTV HD", "TRT Spor HD", "TRT Çocuk", "Minika Çocuk", "Cartoon Network", "beIN Sports 1 HD (FHD)" };
static const char *ch_groups[] = { "Ulusal", "Ulusal", "Ulusal", "Ulusal", "Ulusal", "Ulusal", "Ulusal", "Haber", "Haber", "Haber", "Haber",
    "Spor", "Çocuk", "Çocuk", "Çocuk", "Spor" };
static int show_group = 0;
static int check_demo;                                       /* 0.7.0: watched episodes */
static void ch_row(int i, ScrRow *r, void *c) { (void)c; r->name = ch_names[i]; r->number = i + 1; r->right = show_group ? ch_groups[i] : NULL; r->fav = fav_demo && i < 3; r->moving = fav_demo == 2 && i == 1; r->check = check_demo && i < 3; }
static int search_idx[] = { 12, 13 };
static void search_row(int i, ScrRow *r, void *c) { (void)c; r->name = ch_names[search_idx[i]]; r->number = search_idx[i] + 1; r->right = ch_groups[search_idx[i]]; }

static void grp_row_pv(int i, ScrRow *r, void *c)
{
    static const char *gr[] = { "All", "Ulusal", "Haber", "Spor", "Çocuk", "Belgesel", "Müzik", "Yerel", "Radyolar" };
    static const char *cnt[] = { "16", "7", "4", "2", "3", "0", "0", "0", "0" };
    (void)c;
    if (fav_demo) {                                          /* "Favorites" first, then the rest one down */
        if (i == 0) { r->name = T("Favorites"); r->right = "3"; r->fav = 1; return; }
        i--;
    }
    r->name = i == 0 ? T("All") : gr[i]; r->right = cnt[i];
}


/* 0.8.0: guide rows: half-hour-ish programmes around now (UTC 1761410880) */
static const char *prog_names[] = { "Ana Haber Bülteni", "Kızılcık Şerbeti", "Gönül Dağı", "Spor Haberleri", "Hava Durumu",
                                    "Belgesel: Anadolu", "Yabancı Film: Uzak", "Müzik Kutusu", "Çizgi Film Saati", "Gece Haberleri" };
static void gd_row_pv(int i, ScrGuideRow *r, void *ctx)
{
    const ScrGuide *g = ctx;
    r->name = ch_names[i]; r->number = i + 1; r->archive = i < 6; r->playing = i == 0 && g->pic; r->sel = i == 0;
    r->loading = i == 9;
    if (r->loading) return;
    int64_t t = g->t0 - (i * 7 % 5) * 600;
    for (int k = 0; k < 7 && r->nprogs < SCR_GUIDE_PROGS; k++) {
        int len = (30 + ((i * 3 + k * 5) % 4) * 15) * 60;
        ScrProg *p = &r->progs[r->nprogs++];
        p->start = t; p->end = t + len; p->title = prog_names[(i + k) % 10];
        p->flags = (r->archive && p->end <= g->now ? SCR_PROG_ARCHIVE : 0) | (p->start <= g->now && p->end > g->now ? SCR_PROG_LIVE : 0) |
                   (i == 0 && p->start <= g->now && p->end > g->now ? SCR_PROG_SEL : 0) | (!r->archive && p->end <= g->now ? SCR_PROG_DIM : 0);
        t += len;
    }
}
static void cg_row_pv(int i, ScrCGRow *r, void *c)
{
    (void)c;
    static char tm[12][16];
    snprintf(tm[i], sizeof tm[i], "%02d:%02d", 8 + i, i % 2 ? 30 : 0);
    r->time = tm[i]; r->title = prog_names[i % 10]; r->archive = i < 11; r->live = i == 11; r->progress = i == 11 ? 0.4f : 0;
}
static void ep_row_pv(int i, ScrEpRow *r, void *c)
{
    (void)c;
    static char nm[12][48];
    snprintf(nm[i], sizeof nm[i], "%d. Bölüm", i + 1);
    r->name = nm[i]; r->right = "1 sa 52 dk"; r->check = i < 2; r->progress = i == 2 ? 0.35f : 0;
}
static vita2d_texture wait_tex;
static void load_backdrop(void)                              /* raw RGBA written by the test script from resources/waiting.png */
{
    FILE *f = fopen("/tmp/preview/waiting.rgba", "rb");
    if (!f) return;
    wait_tex.w = 960; wait_tex.h = 544; wait_tex.px = malloc(960 * 544 * 4);
    if (fread(wait_tex.px, 4, 960 * 544, f) == 960 * 544) {
        for (int i = 0; i < 960 * 544; i++) {               /* file is R,G,B,A bytes; the raster wants RGBA8() order */
            uint8_t *b = (uint8_t *)&wait_tex.px[i];
            wait_tex.px[i] = RGBA8(b[0], b[1], b[2], b[3]);
        }
        scr_set_backdrop(&wait_tex);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    load_backdrop();
    (void)argv;
    preview_init();
    ui_init(NULL, 0);                                        /* (0.4.5: Turkish letters as they are) */
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
    UiHint hp[] = { { UI_BTN_CROSS, "Pause" }, { UI_BTN_SQUARE, "1080p" }, { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_TRIANGLE, "Info" },
                    { UI_BTN_SELECT, "Resolution" }, { UI_BTN_CIRCLE, "Back" } };   /* (0.4.0, an HLS channel with sizes) */
    ScrPlayer pl = { "TRT 1 HD", "Ulusal", "1280x720  25 fps", 1, hp, 6,
                     "Video 1280x720   shown 1624   dropped 6   late 0   damaged 0   errors 0   16864 KB",
                     "Audio AAC 48000 Hz 2 ch   A/V +12 ms", NULL, NULL, 0, 0, 0 };
    preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&pl); shot("5_player_osd");
    {   /* the info table (Triangle) as main.c fills it */
        static char r0[200], r1[200], r2[200];
        snprintf(r0, sizeof r0, "%s\v1920x1080\t%s\v43 / 50 fps\t%s\v12345\t%s\v678", T("Video"), T("Frame rate"), T("Shown"), T("Dropped"));
        snprintf(r1, sizeof r1, "%s\vAAC 48 kHz stereo\tA/V\v-20 ms\t%s\v2\t%s\v0 / 0", T("Audio"), T("Waits"), T("Damaged / errors"));
        snprintf(r2, sizeof r2, "%s\v312 KB/s\t%s\v340 KB/s\t%s\v48 MB\t%s\v0", T("Network"), T("Needed"), T("Received"), T("Not decoded"));
        ScrPlayer pt = pl;
        pt.stats1 = r0; pt.stats2 = r1; pt.stats3 = r2; pt.stats_table = 1;
        pt.stats_note = T("The Vita cannot decode this 1080p at %d fps: switched to 720p");
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&pt); shot("5d_info_table");
    }

    ScrPlayer pc = { "Kanal D HD", "Ulusal", NULL, 1, hp, 4, NULL, NULL, "Connecting...", "Waiting for the stream", UI_TEXT, 1, 300, 1 };
    preview_clear(0); scr_player(&pc); shot("6_connecting");

    ScrPlayer pe = { "beIN Sports 1 HD (FHD)", "Spor", NULL, 1, hp, 4, NULL, NULL,
                     "Cannot play this channel", "1920x1080 is above the Vita decoder limit (720p)", UI_ERR, 0, 0 };
    preview_clear(0); scr_player(&pe); shot("7_error");

    UiHint hfilm[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_LEFTRIGHT, "Seek" }, { UI_BTN_TRIANGLE, "Info" },
                       { UI_BTN_SQUARE, "720p" }, { UI_BTN_SELECT, "Resolution" }, { UI_BTN_CIRCLE, "Back" } };
    ScrPlayer pf;
    memset(&pf, 0, sizeof pf);
    pf.name = "Film A (2020)"; pf.group = "Aksiyon"; pf.info = "1920x1080  H.264  AAC"; pf.osd = 1; pf.hints = hfilm; pf.nhints = 6;
    pf.film = 1; pf.pos_ms = 2470000; pf.dur_ms = 6312000; pf.seek_ms = 3070000; pf.paused = 1;
    preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&pf); shot("5b_film");
    const char *rm[2] = { "Continue from 41:10", "Start from the beginning" };
    preview_clear(UI_BG); scr_menu("Film A (2020)", rm, 2, 0); shot("5c_resume");

    UiHint hr[] = { { UI_BTN_UPDOWN, "Station" }, { UI_BTN_SELECT, "Analyze" }, { UI_BTN_CIRCLE, "Back" } };
    for (int k = 0; k < 12; k++) { preview_clear(UI_BG); scr_radio("Radyo Fenomen", "Radyolar", "AAC  48 kHz  stereo", 760, 1234 + k * 16, hr, 3, "", 0); }
    preview_clear(UI_BG);
    scr_radio("Radyo Fenomen", "Radyolar", "AAC  48 kHz  stereo", 760, 1234, hr, 3, NULL, 1);
    shot("8_radio");

    const char *lines[] = { "HTTP 200, 4192 KB in 6.0 s (5694 kbit/s)", "Streams: 0x1B=h264, 0x0F=aac",
        "Video: H.264 Main L3.1  1280x720  25.00 fps  8-bit", "Keyframes: 2 (+0 other I-pictures) of 451 pictures, reference frames: 1",
        "Audio: AAC-LC 48000 Hz, 2 ch, 836 frames", "Packet problems: lost 0, damaged pictures 0, sync lost 0",
        "Playable by hardware: YES: H.264 8-bit up to 720p" };
    UiHint ha[] = { { UI_BTN_CROSS, "Again" }, { UI_BTN_CIRCLE, "Back" } };
    preview_clear(UI_BG); scr_lines(T("Stream analysis"), "TRT 1 HD", lines, 7, NULL, 0, 0, ha, 2); shot("9_analysis");
    /* new screens */
    UiHint hs2[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_SQUARE, "Add" }, { UI_BTN_TRIANGLE, "Edit" }, { UI_BTN_SELECT, "Menu" } };
    ScrList sl2 = { "Vita IPTV", T("Playlists"), "5 playlists", 5, 1, &scroll, src_row, NULL, T("No playlists"), hs2, 4, T("Saved as Xtream (faster, live channels only)"), 0 };
    const char *menu[] = { T("Add playlist"), T("Settings"), T("Reload sources"), T("About"), T("Quit") };
    preview_clear(UI_BG); scroll = 0; scr_list(&sl2); scr_menu(T("Menu"), menu, 5, 1); shot("a_menu");
    UiHint hf2[] = { { UI_BTN_CROSS, "Edit / choose" }, { UI_BTN_CIRCLE, "Cancel" }, { UI_BTN_SEP, "" }, { UI_BTN_START, "Save" } };
    UiHint hf[] = { { UI_BTN_UPDOWN, "Move" }, { UI_BTN_CROSS, "Edit / choose" }, { UI_BTN_START, "Save" }, { UI_BTN_CIRCLE, T("Cancel") } };
    ScrField ff[] = { { T("Type"), T("Xtream (server + username + password)"), SCR_F_CHOICE, 1 }, { T("Name"), "Ev internet", SCR_F_TEXT, 1 },
                      { T("Server address"), "http://tv.example.com:8080", SCR_F_TEXT, 1 }, { T("Username"), "ali", SCR_F_TEXT, 1 },
                      { T("Password"), "********", SCR_F_TEXT, 1 }, { T("Save"), NULL, SCR_F_BUTTON, 1 }, { T("Cancel"), NULL, SCR_F_BUTTON, 1 },
                      { T("Delete playlist"), NULL, SCR_F_DANGER, 1 } };
    preview_clear(UI_BG); scr_form(T("Edit playlist"), T("Tip: a pasted get.php link fills everything"), ff, 8, 2, hf2, 4, NULL, 0); shot("c_form");
    preview_clear(UI_BG); scr_form(T("Edit playlist"), NULL, ff, 8, 7, hf, 4, NULL, 0);
    scr_confirm(T("Delete this playlist?"), "\"Spor paketi\" will be removed from the list."); shot("b_confirm");
    ScrField fs[] = { { T("Transcoding server"), "http://192.168.1.20:8090", SCR_F_TEXT, 1 }, { T("Use automatically"), T("When needed"), SCR_F_CHOICE, 1 },
                      { T("Language"), T("Automatic (system)"), SCR_F_CHOICE, 1 }, { T("Hide adult channels"), T("On"), SCR_F_TOGGLE, 1 }, { T("Parental lock (PIN)"), "****", SCR_F_TEXT, 1 },
                      { T("Hide channels named 4K / UHD"), T("Off"), SCR_F_TOGGLE, 1 },
                      { T("Preferred resolution"), T("Automatic"), SCR_F_CHOICE, 1 },
                      { T("Theme"), T("Blue"), SCR_F_CHOICE, 1 },
                      { T("Test server"), NULL, SCR_F_BUTTON, 1 }, { T("Clear Recently Watched"), NULL, SCR_F_BUTTON, 1 },
                      { T("Clear watched episodes"), NULL, SCR_F_BUTTON, 1 },
                      { T("Save"), NULL, SCR_F_BUTTON, 1 }, { T("Cancel"), NULL, SCR_F_BUTTON, 1 } };
    preview_clear(UI_BG); scr_form(T("Settings"), T("Server: plays HEVC, 1080i, AC-3 and other formats (tools/vita_iptv_proxy.py)"), fs, 13, 2, hf2, 4, T("Server is running"), 0); shot("d_settings");
    preview_clear(UI_BG); scr_form(T("Settings"), NULL, fs, 13, 7, hf2, 4, NULL, 0); shot("d_settings_end");
    scr_confirm(T("Clear watched episodes?"), T("The ticks of watched episodes are removed. Where films stopped is kept.")); shot("d3_clear_watched");
    preview_clear(UI_BG); scr_form(T("Settings"), NULL, fs, 11, 7, hf2, 4, NULL, 0);
    scr_confirm(T("Clear recently watched?"), T("The recently watched channels of every playlist are removed. Favorites stay.")); shot("d2_clear_recent");
    {   /* 0.5.0: the Select menu while playing: resolutions, the server line, audio tracks */
        static char a1[80], a2[80];
        snprintf(a1, sizeof a1, "> %s: %s (AAC)", T("Audio"), "Türkçe");
        snprintf(a2, sizeof a2, "%s: %s (E-AC-3)", T("Audio"), "English");
        const char *sm[] = { T("Automatic"), "1920x1080", "> 1280x720", "720x576", T("Through the Server"), a1, a2, T("Stream analysis") };
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f);
        scr_menu(T("Resolution"), sm, 8, 5); shot("5e_select_menu");
    }
    preview_clear(UI_BG); scroll = 0; scr_list(&sl2); scr_about("v0.4.0"); shot("e_about");
    preview_clear(UI_BG); scr_splash(NULL, "Loading playlists...", "v1.0", 900); shot("f_splash_drawn");
    /* two-pane channel screen */
    {
        static const char *gr[] = { "All", "Ulusal", "Haber", "Spor", "Çocuk", "Belgesel", "Müzik", "Yerel", "Radyolar" };
        static int gscroll2, cscroll2;
        UiHint hl[] = { { UI_BTN_UPDOWN, "Category" }, { UI_BTN_CROSS, "Channels" }, { UI_BTN_SQUARE, "Search" },
                        { UI_BTN_SELECT, "Menu" }, { UI_BTN_CIRCLE, "Back" } };
        UiHint hr2[] = { { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_CROSS, "Play" }, { UI_BTN_LR, "Page" },
                         { UI_BTN_SQUARE, "Search" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_CIRCLE, "Back" } };
        for (int f = 0; f < 2; f++) {
            ScrDual d; memset(&d, 0, sizeof d);
            d.title = "Ev internet - Xtream"; d.subtitle = T("All"); d.right = "16 channels";
            d.lcount = 9; d.lsel = f ? 1 : 3; d.lscroll = &gscroll2; d.lrow = grp_row_pv;
            d.rcount = 16; d.rsel = f ? 4 : 0; d.rscroll = &cscroll2; d.rrow = ch_row;
            d.focus = f; d.lmark = 1; d.hints = f ? hr2 : hl; d.nhints = f ? 6 : 5;
            (void)gr;
            show_group = 0;
            preview_clear(UI_BG); gscroll2 = cscroll2 = 0; scr_dual(&d); shot(f ? "g_dual_right" : "g_dual_left");
        }
    }
    /* 0.5.0: long notes wrap; favourites */
    {
        static char msg[200];
        snprintf(msg, sizeof msg, "The Vita decoder gives no picture for this stream (%u errors). The transcoding server can play it", 37u);
        ScrPlayer pw = { "beIN Sports 1 HD (FHD)", "Spor", NULL, 1, hp, 4, NULL, NULL, "Cannot play this channel", T_msg(msg), UI_ERR, 0, 0 };
        pw.stats1 = T("Tip: a transcoding server on your PC can play this channel (channel list: Select > Settings, see README)");
        preview_clear(0); scr_player(&pw); shot("h_wrap_error");
        ScrPlayer pn = pl;
        pn.stats1 = T("This channel has one picture size. Another needs the transcoding server (channel list: Select > Settings)");
        pn.stats2 = NULL;
        pn.fav = 1;
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&pn); shot("h_wrap_note_fav");
        ScrPlayer pt2 = pl;
        static char r0[200];
        snprintf(r0, sizeof r0, "%s\v1920x1080\t%s\v43 / 50 fps\t%s\v12345\t%s\v678", T("Video"), T("Frame rate"), T("Shown"), T("Dropped"));
        pt2.stats1 = r0; pt2.stats2 = r0; pt2.stats3 = r0; pt2.stats_table = 1;
        pt2.stats_note = T("This film has one picture size (%dx%d). 720p needs the transcoding server (channel list: Select > Settings)");
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&pt2); shot("h_wrap_table");
        preview_clear(UI_BG); scr_radio("Radyo Fenomen", "Radyolar", "AAC  48 kHz  stereo", 760, 1234, hr, 3,
            T("This channel has one picture size. Another needs the transcoding server (channel list: Select > Settings)"), 1); shot("h_wrap_radio");
        ScrList sw = sl2;
        sw.status = T_msg("HTTPS failed (0x80435001): this site needs newer TLS than the Vita has. Copy the .m3u file to ux0:data/VitaIPTV/ instead");
        sw.status_err = 1;
        fav_demo = 1;
        UiHint hs3[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_SQUARE, "Add" }, { UI_BTN_TRIANGLE, "Edit" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_START, "Fav." } };
        sw.hints = hs3; sw.nhints = 5;
        preview_clear(UI_BG); scroll = 0; scr_list(&sw); shot("i_fav_playlists");
        static int gs, cs;
        UiHint hr3[] = { { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_CROSS, "Play" }, { UI_BTN_LR, "Page" },
                         { UI_BTN_SQUARE, "Search" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_CIRCLE, "Back" }, { UI_BTN_START, "Fav." } };
        ScrDual d; memset(&d, 0, sizeof d);
        d.title = "Ev internet - Xtream"; d.subtitle = T("Favorites"); d.right = "3 channels";
        d.lcount = 10; d.lsel = 0; d.lscroll = &gs; d.lrow = grp_row_pv;
        d.rcount = 16; d.rsel = 1; d.rscroll = &cs; d.rrow = ch_row;
        d.focus = 1; d.lmark = 1; d.hints = hr3; d.nhints = 7; d.status = T("Added to favorites");
        preview_clear(UI_BG); gs = cs = 0; scr_dual(&d); shot("i_fav_channels");
        preview_clear(UI_BG); scr_confirm(T("Delete this playlist?"),
            "\"A very long playlist name that goes on and on - Xtream backup server\" will be removed from the list."); shot("i_confirm_wrap");
        fav_demo = 0;
    }
    /* 0.4.5: reordering a favourite, the EPG line in the player */
    {
        static int gs, cs;
        UiHint hm[] = { { UI_BTN_UPDOWN, "Up / down" }, { UI_BTN_CROSS, "Save" }, { UI_BTN_CIRCLE, "Cancel" } };   /* (0.8.0: X saves) */
        fav_demo = 2;
        ScrDual d; memset(&d, 0, sizeof d);
        d.title = "Ev internet - Xtream"; d.subtitle = T("Favorites"); d.right = "3 channels";
        d.lcount = 10; d.lsel = 0; d.lscroll = &gs; d.lrow = grp_row_pv;
        d.rcount = 3; d.rsel = 1; d.rscroll = &cs; d.rrow = ch_row;
        d.focus = 1; d.lmark = 1; d.hints = hm; d.nhints = 3; d.status = T("Up / down moves the channel, X saves the order");
        preview_clear(UI_BG); gs = cs = 0; scr_dual(&d); shot("k_fav_move");
        d.hints = NULL; d.nhints = 0; d.status = "Şimdi: Ana Haber Bülteni (19:00-20:30)  ·  Sonra: Kızılcık Şerbeti (20:30)";
        fav_demo = 1; d.rcount = 16;
        preview_clear(UI_BG); gs = cs = 0; scr_dual(&d); shot("k_epg_list");
        fav_demo = 0;
        ScrPlayer pe2 = pl;
        pe2.group = "Şimdi: Ana Haber Bülteni (19:00-20:30)  ·  Sonra: Kızılcık Şerbeti (20:30)";
        pe2.fav = 1;
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&pe2); shot("k_epg_player");
    }
    /* 0.7.0: the Select menu's tabs, a subtitle, the clock, the next episode, watched episodes */
    {
        const char *tabs[] = { T("Picture"), T("Audio"), T("Subtitles"), T("More") };
        static char sy[64];
        snprintf(sy, sizeof sy, T("Sync:  < %+d.%d s >"), 1, 5);
        const char *pic[] = { T("Automatic"), "1920x1080", "1280x720", "720x576", T("Through the Server") };
        const char *sub[] = { T("Off"), "Türkçe", "English (SDH)", "Türkçe (Zorunlu)", sy };
        int dim0[] = { 0, 0, 0, 0, 0 };
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f);
        scr_tabmenu(tabs, 4, 0, pic, dim0, 5, 2, T("L / R: tab   X: choose   O: close")); shot("l_tab_picture");
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f);
        scr_tabmenu(tabs, 4, 2, sub, dim0, 5, 4, T("Left / Right: earlier / later   X: back to 0   O: close")); shot("l_tab_subs");
        const char *none[] = { T("No subtitles in this film (MKV / MP4 text subtitles only)") };
        int dim1[] = { 1 };
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f);
        scr_tabmenu(tabs, 4, 2, none, dim1, 1, 0, T("L / R: tab   X: choose   O: close")); shot("l_tab_nosubs");
        ScrPlayer ps = pl;
        ps.clock = "21:47"; ps.subtitle = "Bunu sana daha önce söylemiştim.\nAma dinlemedin.";
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&ps); shot("l_player_clock_sub");
        ps.osd = 0;
        ps.corner = "Sonraki bölüm 4 sn içinde"; ps.corner2 = T("X: now   O: cancel");
        if (lang_get() != LANG_TR) ps.corner = "Next episode in 4 s";
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&ps); shot("l_next_episode");
        static int gs, cs;
        check_demo = 1; show_group = 1;
        ScrDual d; memset(&d, 0, sizeof d);
        d.title = "Ev internet - Xtream"; d.subtitle = "Dizi - Sezon 1"; d.right = "16";
        d.lcount = 10; d.lsel = 0; d.lscroll = &gs; d.lrow = grp_row_pv;
        d.rcount = 16; d.rsel = 3; d.rscroll = &cs; d.rrow = ch_row; d.focus = 1;
        preview_clear(UI_BG); gs = cs = 0; scr_dual(&d); shot("l_watched");
        check_demo = 0; show_group = 0;
    }
    /* 0.8.0: the guide, a channel's schedule, catch-up in the player, film / series details, the preview card */
    {
        static uint32_t ppx[240 * 360];                       /* a made-up poster */
        for (int y = 0; y < 360; y++) for (int x = 0; x < 240; x++)
            ppx[y * 240 + x] = RGBA8(160 - y * 120 / 360, 40 + x * 60 / 240, 50 + (y > 250 ? 90 : 0), 255);
        vita2d_texture poster = { 240, 360, ppx };
        const int64_t now = 1761410880;                       /* Sat 25 Oct 2025 16:48 UTC (19:48 at +3) */
        const int tz = 180;
        ScrGuide g; memset(&g, 0, sizeof g);
        g.title = "Ev internet - Xtream"; g.subtitle = T("Guide"); g.right = "Ulusal";
        g.t0 = now - now % 1800 - 1800; g.now = now; g.span_min = 150; g.tz_min = tz; g.day = T("Today");
        g.count = 16; g.first = 0;
        g.sel_title = "Ana Haber Bülteni"; g.sel_line = "TRT 1 HD  ·  19:00 - 20:30  ·  90 dk";
        g.sel_desc = "Günün önemli gelişmeleri, son dakika haberleri ve hava durumu.";
        g.sel_live = 1; g.sel_archive = 1; g.sel_progress = 0.54f; g.sel_left = "42 dk kaldı";
        g.sel_note = T("X: watch from the start (archive) or live");
        if (lang_get() != LANG_TR) { g.sel_line = "TRT 1 HD  ·  19:00 - 20:30  ·  90 min"; g.sel_left = "42 min left"; }
        UiHint HL[] = { { UI_BTN_CROSS, "Watch" }, { UI_BTN_LEFTRIGHT, "Time" }, { UI_BTN_LR, "Day" }, { UI_BTN_SQUARE, "Schedule" },
                        { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_SELECT, "Category" } };
        UiHint HP[] = { { UI_BTN_CROSS, "Watch" }, { UI_BTN_LEFTRIGHT, "Time" }, { UI_BTN_UPDOWN, "Channel" }, { UI_BTN_SQUARE, "Schedule" },
                        { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_LR, "Day" } };
        g.hints = HL; g.nhints = 7;
        g.row = gd_row_pv; g.ctx = (void *)&g;
        preview_clear(UI_BG); scr_guide(&g); shot("m_guide_list");
        g.pic = &tex; g.pic_w = 1280; g.pic_h = 720; g.pic_label = "TRT 1 HD  ·  oynatılıyor";
        g.hints = HP; g.nhints = 7;
        preview_clear(UI_BG); scr_guide(&g); shot("m_guide_player");
        g.pic = NULL; g.hints = HL;
        static int ds;
        UiHint HD[] = { { UI_BTN_CROSS, "Choose" }, { UI_BTN_UPDOWN, "Category" }, { UI_BTN_CIRCLE, "Close" } };
        g.drawer = 1; g.dcount = 9; g.dsel = 1; g.dscroll = &ds; g.drow = grp_row_pv; g.hints = HD; g.nhints = 3;
        preview_clear(UI_BG); scr_guide(&g); shot("m_guide_drawer");
        g.drawer = 0;
        UiHint HPOP[] = { { UI_BTN_CROSS, "Choose" }, { UI_BTN_CIRCLE, "Cancel" } };
        g.popup = 1; g.popup_sel = 0; g.popup_title = "Ana Haber Bülteni"; g.popup_line = "TRT 1 HD  ·  Bugün 19:00 - 20:30";
        g.popup_a = T("Watch from the start (archive)"); g.popup_b = T("Watch live"); g.hints = HPOP; g.nhints = 2;
        preview_clear(UI_BG); scr_guide(&g); shot("m_guide_popup");
        g.popup = 0; g.hints = HL; g.nhints = 7; g.count = 0; g.empty = T("No channels");
        g.sel_title = "TRT 1 HD"; g.sel_line = T("Loading the guide..."); g.sel_desc = NULL; g.sel_note = NULL; g.sel_live = 0;
        preview_clear(UI_BG); scr_guide(&g); shot("m_guide_loading");

        /* a channel's schedule */
        static const char *days[] = { "Çar 22", "Per 23", "Dün", "Bugün", "Yarın" };
        static int cgs;
        ScrChanGuide c; memset(&c, 0, sizeof c);
        c.title = "TRT 1 HD"; c.subtitle = T("Schedule"); c.right = "arşiv 3 gün";
        c.days = days; c.ndays = 5; c.day = 3; c.count = 12; c.sel = 4; c.scroll = &cgs; c.row = cg_row_pv;
        c.d_title = "Gönül Dağı"; c.d_line1 = "Bugün 14:00 - 16:00  ·  120 dk";
        c.d_desc = "Dilek ile Taner'in yolları yeniden kesişir. Köyde düğün hazırlıkları başlar."; c.d_archive = 1;
        UiHint HC[] = { { UI_BTN_CROSS, "Watch" }, { UI_BTN_LR, "Day" }, { UI_BTN_UPDOWN, "Programme" }, { UI_BTN_CIRCLE, "Back" } };
        c.hints = HC; c.nhints = 4;
        preview_clear(UI_BG); cgs = 0; scr_chan_guide(&c); shot("n_schedule");

        /* catch-up in the player */
        UiHint ha[] = { { UI_BTN_CROSS, "Pause" }, { UI_BTN_SQUARE, "Live" }, { UI_BTN_TRIANGLE, "Schedule" }, { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" },
                        { UI_BTN_LEFTRIGHT, "Seek" }, { UI_BTN_L, "P. Channel" }, { UI_BTN_R, "Info" }, { UI_BTN_SELECT, "Res." }, { UI_BTN_START, "Fav." } };
        ScrPlayer pa; memset(&pa, 0, sizeof pa);
        pa.name = "TRT 1 HD  ·  Gönül Dağı"; pa.group = "Bugün 14:00 - 16:00"; pa.info = "1280x720  25 fps"; pa.osd = 1;
        pa.hints = ha; pa.nhints = 10; pa.film = 1; pa.pos_ms = 42 * 60000; pa.dur_ms = 120 * 60000; pa.seek_ms = -1;
        pa.badge = T("ARCHIVE"); pa.time_l = "14:42"; pa.time_r = "16:00"; pa.clock = "19:48";
        pa.time_note_l = "42 dk izlendi  ·  78 dk kaldı"; pa.time_note_r = "Sonraki: Kızılcık Şerbeti (20:00)";
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&pa); shot("o_archive_player");
        pa.osd = 0; pa.corner = "Sonraki program 4 sn içinde"; pa.corner2 = T("X: now   O: cancel");
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&pa); shot("o_archive_next");
        ScrPlayer pz = pl;
        pz.osd = 0; pz.corner = "Uyku zamanlayıcı: 42 sn sonra duracak"; pz.corner2 = T("X: 15 more minutes");
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&pz); shot("o_sleep_warning");
        const char *tabs[] = { T("Picture"), T("Audio"), T("Subtitles"), T("More") };
        const char *more[] = { "Akış analizi", "Uyku zamanlayıcı:  < 30 dk >   (28 dk kaldı)", T("Channel number...") };
        int dimm[] = { 0, 0, 0 };
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f);
        scr_tabmenu(tabs, 4, 3, more, dimm, 3, 1, T("Left / Right / X: change   O: close")); shot("o_more_sleep");

        /* the films list with its preview card (layout A) */
        ScrVodCard card; memset(&card, 0, sizeof card);
        card.poster = &poster; card.title = "Kurak Günler"; card.line = "2022  ·  2 sa 9 dk";
        card.genre = "Dram, Gerilim"; card.director = "Emin Alper"; card.cast = "Selahattin Paşalı, Ekin Koç, Erol Babaoğlu";
        card.rating = 7.4f;
        card.plot = "Genç ve idealist savcı Emre, siyasi çekişmelerle sarsılan küçük bir kasabaya atanır. Kasabadaki su krizi ve "
                    "bir av partisinin ardından yaşananlar, Emre'yi tehlikeli bir oyunun içine çeker.";
        static int gs, cs;
        UiHint hv[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_TRIANGLE, "Details" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" },
                        { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_START, "Fav." } };
        ScrDual d; memset(&d, 0, sizeof d);
        d.title = "Ev internet - Xtream"; d.subtitle = "Yerli Filmler"; d.right = "16 film";
        d.lcount = 9; d.lsel = 2; d.lscroll = &gs; d.lrow = grp_row_pv;
        d.rcount = 16; d.rsel = 2; d.rscroll = &cs; d.rrow = ch_row; d.focus = 1; d.lmark = 1;
        d.hints = hv; d.nhints = 8; d.preview = &card;
        preview_clear(UI_BG); gs = cs = 0; scr_dual(&d); shot("p_films_preview");
        ScrVodCard cl2 = card; cl2.poster = NULL; cl2.loading = 1;
        d.preview = &cl2;
        preview_clear(UI_BG); gs = cs = 0; scr_dual(&d); shot("p_films_preview_loading");

        /* film details */
        static const char *btn[] = { "Devam et (1:12:40)", "Baştan", "Favori" };
        UiHint hdet[] = { { UI_BTN_CROSS, "Choose" }, { UI_BTN_LEFTRIGHT, "Button" }, { UI_BTN_UPDOWN, "Previous / next" },
                          { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_START, "Fav." } };
        ScrVodDetail v; memset(&v, 0, sizeof v);
        v.header = "Ev internet - Xtream"; v.header_sub = T("Details"); v.card = card;
        v.buttons = btn; v.nbuttons = 3; v.sel = 0; v.progress = 0.56f; v.fav = 1;
        v.note = T("Details and poster come from the provider; without them only the name shows.");
        v.hints = hdet; v.nhints = 6;
        preview_clear(UI_BG); scr_vod_detail(&v); shot("q_film_detail");
        ScrVodDetail vn = v; vn.card.poster = NULL; vn.card.genre = vn.card.director = vn.card.cast = vn.card.plot = NULL;
        vn.card.rating = -1; vn.card.line = NULL; vn.progress = -1; vn.fav = 0; vn.buttons = btn + 1; vn.nbuttons = 2;
        preview_clear(UI_BG); scr_vod_detail(&vn); shot("q_film_detail_bare");
        ScrVodDetail vo = v; vo.overlay = 1;
        UiHint hov[] = { { UI_BTN_CIRCLE, "Close" } };   /* (0.8.1) */
        vo.hints = hov; vo.nhints = 1;
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_vod_detail(&vo); shot("q_film_overlay");

        /* a series */
        static const char *sn[] = { "Sezon 1", "Sezon 2", "Sezon 3", "Sezon 4" };
        static int es;
        ScrSeries se; memset(&se, 0, sizeof se);
        se.header = "Ev internet - Xtream"; se.header_sub = "Diziler"; se.card = card;
        se.card.title = "Bir Zamanlar Çukurova"; se.card.line = "2018  ·  4 sezon"; se.card.director = NULL;
        se.seasons = sn; se.nseasons = 4; se.season = 1; se.count = 12; se.sel = 2; se.scroll = &es; se.row = ep_row_pv;
        UiHint hse[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_LR, "Season" }, { UI_BTN_UPDOWN, "Episode" }, { UI_BTN_CIRCLE, "Back" },
                         { UI_BTN_SEP, "" }, { UI_BTN_START, "Fav." } };
        se.hints = hse; se.nhints = 6;
        preview_clear(UI_BG); es = 0; scr_series(&se); shot("r_series");
    }
    /* 0.8.1: subtitle looks and the themes */
    {
        ScrPlayer ps = pl;
        ps.osd = 0;
        ps.subtitle = "Bunu sana daha önce söylemiştim.\nAma dinlemedin.";
        static const char *nm[] = { "s_sub_small_yellow_band", "s_sub_large_white_box", "s_sub_vlarge_green_none_higher", "s_sub_medium_blue_top" };
        int st[4][4] = { { 0, 1, 0, 0 }, { 2, 0, 0, 1 }, { 3, 2, 1, 2 }, { 1, 3, 2, 0 } };
        for (int k = 0; k < 4; k++) {
            scr_sub_style(st[k][0], st[k][1], st[k][2], st[k][3]);
            ps.osd = k == 3;
            preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&ps); shot(nm[k]);
        }
        scr_sub_style(3, 0, 0, 0);                           /* the longest: 4 lines, very large, with the bars */
        ps.osd = 1;
        ps.subtitle = "Bu çok uzun bir altyazı satırı, ekranın genişliğini aşıyor ve bölünmesi gerekiyor.\nİkinci satır da uzun, o da bölünüyor.";
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&ps); shot("s_sub_vlarge_long");
        scr_sub_style(1, 0, 0, 0);
        const char *tabs[] = { T("Picture"), T("Audio"), T("Subtitles"), T("More") };
        static char rows[8][64];
        snprintf(rows[0], 64, "%s", T("Off")); snprintf(rows[1], 64, "> Türkçe"); snprintf(rows[2], 64, T("Sync:  < %+d.%d s >"), 0, 0);
        snprintf(rows[3], 64, "%s:  < %s >", T("Size"), T("Large")); snprintf(rows[4], 64, "%s:  < %s >", T("Colour"), T("Yellow"));
        snprintf(rows[5], 64, "%s:  < %s >", T("Place"), T("Bottom")); snprintf(rows[6], 64, "%s:  < %s >", T("Background"), T("Light band"));
        const char *it[7] = { rows[0], rows[1], rows[2], rows[3], rows[4], rows[5], rows[6] };
        int dz[7] = { 0 };
        scr_sub_style(2, 1, 0, 0);
        ps.osd = 0; ps.subtitle = "Bunu sana daha önce söylemiştim.";
        preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&ps);
        scr_tabmenu(tabs, 4, 2, it, dz, 7, 4, T("Left / Right / X: change   O: close")); shot("s_sub_menu");
        scr_sub_style(1, 0, 0, 0);
        /* each theme: the channels, the settings, a film's details */
        static const char *tn[4] = { "blue", "green", "purple", "orange" };
        for (int t = 0; t < UI_THEMES; t++) {
            ui_set_theme(t);
            char nmb[48];
            static int gs, cs;
            UiHint hr2[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_TRIANGLE, "Guide" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" },
                             { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_START, "Fav." } };
            ScrDual d; memset(&d, 0, sizeof d);
            d.title = "Ev internet - Xtream"; d.subtitle = "Ulusal"; d.right = "16 channels";
            d.lcount = 9; d.lsel = 1; d.lscroll = &gs; d.lrow = grp_row_pv;
            d.rcount = 16; d.rsel = 3; d.rscroll = &cs; d.rrow = ch_row; d.focus = 1; d.lmark = 1; d.hints = hr2; d.nhints = 8;
            d.status = "Şimdi: Ana Haber Bülteni (19:00-20:30)";
            fav_demo = 1;
            snprintf(nmb, sizeof nmb, "t_%s_channels", tn[t]);
            preview_clear(UI_BG); gs = cs = 0; scr_dual(&d); shot(nmb);
            fav_demo = 0;
            fs[7].value = T(ui_theme_name(t));
            snprintf(nmb, sizeof nmb, "t_%s_settings", tn[t]);
            preview_clear(UI_BG); scr_form(T("Settings"), NULL, fs, 13, 7, hf2, 4, NULL, 0); shot(nmb);
            snprintf(nmb, sizeof nmb, "t_%s_menu", tn[t]);
            preview_clear(UI_BG); scroll = 0; scr_list(&sl2); scr_menu(T("Menu"), menu, 5, 1); shot(nmb);
            snprintf(nmb, sizeof nmb, "t_%s_player", tn[t]);
            preview_clear(0); vita2d_draw_texture_part_scale(&tex, 0, 0, 0, 0, 1280, 720, 0.75f, 0.7555f); scr_player(&pf); shot(nmb);
        }
        ui_set_theme(0);
        fs[7].value = T("Blue");
    }
    /* 0.4.1: the button bars, left and right */
    {
        UiHint b1[] = { { UI_BTN_CROSS, "Open" }, { UI_BTN_SQUARE, "Add" }, { UI_BTN_TRIANGLE, "Edit" }, { UI_BTN_SEP, "" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_START, "Fav." } };
        UiHint b2[] = { { UI_BTN_CROSS, "Channels" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" } };
        UiHint b3[] = { { UI_BTN_CROSS, "Play" }, { UI_BTN_SQUARE, "Search" }, { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_LR, "Page" }, { UI_BTN_SELECT, "Menu" }, { UI_BTN_START, "Fav." } };
        UiHint b4[] = { { UI_BTN_CROSS, "Pause" }, { UI_BTN_SQUARE, "1080p" }, { UI_BTN_TRIANGLE, "Info" }, { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_L, "P. Channel" }, { UI_BTN_SELECT, "Res." }, { UI_BTN_START, "Fav." } };
        UiHint b5[] = { { UI_BTN_CROSS, "Pause" }, { UI_BTN_SQUARE, "720p" }, { UI_BTN_TRIANGLE, "Info" }, { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_LEFTRIGHT, "Seek" }, { UI_BTN_L, "P. Channel" }, { UI_BTN_SELECT, "Res." }, { UI_BTN_START, "Fav." } };
        UiHint b6[] = { { UI_BTN_CROSS, "Edit / choose" }, { UI_BTN_CIRCLE, "Cancel" }, { UI_BTN_SEP, "" }, { UI_BTN_START, "Save" } };
        UiHint b7[] = { { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_L, "P. Channel" }, { UI_BTN_SELECT, "Analyze" }, { UI_BTN_START, "Fav." } };
        /* 0.8.0: the fullest player bars (live with sizes; an episode with the list behind; catch-up) */
        UiHint b8[] = { { UI_BTN_CROSS, "Pause" }, { UI_BTN_SQUARE, "1080p" }, { UI_BTN_TRIANGLE, "Guide" }, { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_L, "P. Channel" }, { UI_BTN_R, "Info" }, { UI_BTN_SELECT, "Res." }, { UI_BTN_START, "Fav." } };
        UiHint b9[] = { { UI_BTN_CROSS, "Pause" }, { UI_BTN_SQUARE, "Original" }, { UI_BTN_TRIANGLE, "Details" }, { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_LEFTRIGHT, "Seek" }, { UI_BTN_L, "P. Channel" }, { UI_BTN_R, "Info" }, { UI_BTN_SELECT, "Res." }, { UI_BTN_START, "Fav." } };
        UiHint b10[] = { { UI_BTN_CROSS, "Pause" }, { UI_BTN_SQUARE, "Live" }, { UI_BTN_TRIANGLE, "Schedule" }, { UI_BTN_CIRCLE, "Back" }, { UI_BTN_SEP, "" }, { UI_BTN_LEFTRIGHT, "Seek" }, { UI_BTN_L, "P. Channel" }, { UI_BTN_R, "Info" }, { UI_BTN_SELECT, "Res." }, { UI_BTN_START, "Fav." } };
        const UiHint *bars[] = { b1, b2, b3, b4, b5, b6, b7, b8, b9, b10 };
        int nb[] = { 6, 6, 7, 8, 9, 4, 5, 9, 10, 10 };
        preview_clear(UI_BG);
        for (int k = 0; k < 10; k++) {
            /* each bar drawn, then its strip copied up (the bar is always at the bottom) */
            ui_footer(bars[k], nb[k], NULL, 0);
            extern uint32_t fb[];
            memmove(&fb[(k * 49 + 2) * 960], &fb[496 * 960], sizeof(uint32_t) * 960 * 48);
        }
        shot("j_bars");
    }
    return 0;
}
