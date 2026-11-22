/* VitaIPTV: part of main.c - filters, keeping the Vita awake, buttons and the stick, stopping playback.
 * Not built on its own: main.c includes the parts in order (1.2.1), so what main.c and the parts
 * before this one declare is visible here. */

/* Settings saved over the channel screen with a filter changed: a filter turned on is applied to the list in
 * place (same category and channel where they are still there); one turned off needs the list again. */
static void apply_filters_now(void)
{
    int how = filters_changed;
    filters_changed = 0;
    if (state != ST_CHANNELS || !how || cur_src < 0 || cur_src >= nsources) return;
    const Source *s = &sources[cur_src];
    if (section != SEC_LIVE) {                               /* films / series: only the adult categories */
        if (how & 3) load_begin(s, section == SEC_FILMS ? LOAD_FILM_CATS : LOAD_SERIES_CATS, NULL, 0);
        return;
    }
    if (how & (2 | 8)) { leave_episodes(); load_start(s); return; }
    char cname[IPTV_NAME_MAX] = "";
    group_remember();
    if (nvis && sel < nvis) snprintf(cname, sizeof cname, "%s", chans.items[vis[sel]].name);
    int was_focus = focus, before = chans.count;
    build_groups();
    guide_setup();                                           /* 0.8.0 (the channels moved) */
    group_restore();
    rebuild_visible();
    for (int i = 0; i < nvis; i++) if (!strcmp(chans.items[vis[i]].name, cname)) { sel = i; break; }
    if (ngroups > 1) focus = nvis ? was_focus : 0;
    plog("list: filtered again after the settings: %d -> %d channels", before, chans.count);
}

/* ---- power: keep the screen on for video and radio; for radio also block standby */
static unsigned last_tick;

static int radio_playing(void)
{
    if (state != ST_PLAYING || play_mode != 2) return 0;
    const TspStatus *st = tsp_status();
    return st && st->audio_only && st->state < TSP_ENDED;
}

static void keep_awake(void)
{
    /* 0.4.0: no sceKernelPowerLock for the radio any more: closed with the PS button while a station was
     * playing, the app left the lock behind and the Vita hung when the next app started (every version
     * since radio). The ticks below keep it awake the same way while the app runs. */
    int radio = radio_playing();
    if (state != ST_PLAYING) return;
    int video = 0, audio = radio;
    if (play_mode == 0) video = 1;
    else if (play_mode == 2) {
        const TspStatus *st = tsp_status();
        if (st && st->state < TSP_ENDED && !st->audio_only) video = 1;
    }
    if (!video && !audio) return;
    /* the radio keeps the screen on like video (turning the screen off slowed Wi-Fi and cut the sound) */
    unsigned now = now_ms();
    if (now - last_tick < 1000) return;
    last_tick = now;
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    if (video || audio) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING);
    }
}

/* Channels the Vita cannot play go through the transcoding server by themselves. */
static void auto_proxy_check(void)
{
    if (play_mode != 2 || via_proxy || auto_proxy_tried || !srv_here() || !settings.auto_proxy) return;
    const TspStatus *st = tsp_status();
    if (!st) return;
    int format = st->state == TSP_ERROR && st->err_kind == TSP_ERRK_FORMAT;
    int silent = st->state == TSP_PLAYING && st->audio_msg[0] && st->audio_frames == 0 && st->decoded > 25;
    if (format || silent) {
        auto_proxy_tried = 1;
        plog("auto: %s -> transcoding server", format ? st->msg : st->audio_msg);
        play_channel_ex(play_vis, 1);
    }
}

/* ---- input ------------------------------------------------------------- */
static unsigned prev_btn;
static int hold;

/* 1.2: the left stick only (the right one does nothing): up / down as the d-pad, repeating, in every list and
 * menu; on the channel screen a push to the right from the middle opens the category, to the left leaves it.
 * lstick_ud: -1 up, 0 none, +1 down. stick_lr: -1 / +1 once per push (see stick_side). */
static int lstick_ud, stick_lr;
static int stick_x;                 /* sideways push of the left stick, -128..127 (films: scrubbing) */

/* a push to one side, once: from the middle (both axes near it), far enough, and straight (the stick at most half
 * as far up or down as sideways): a slanted push while scrolling a list never opens a category; a push up or
 * down first needs the stick back in the middle before a sideways one counts */
static int stick_side(int x, int y)
{
    static int armed = 1;
    int dx = x - 128, dy = y - 128, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ax < 32 && ay < 32) { armed = 1; return 0; }
    if (!armed) return 0;
    if (ay >= 64) { armed = 0; return 0; }                    /* (up / down: not sideways until the middle again) */
    if (ax >= 100 && ay * 2 <= ax) { armed = 0; return dx > 0 ? 1 : -1; }
    return 0;
}

static int stick_dir(int v, int *hold_count)      /* v is 0..255, 128 is centre */
{
    int d = v < 64 ? -1 : v > 191 ? 1 : 0;
    if (!d) { *hold_count = 0; return 0; }
    (*hold_count)++;
    if (*hold_count == 1) return d;                /* first push */
    if (*hold_count > 18 && *hold_count % 3 == 0) return d;   /* then repeat, like the d-pad */
    return 0;
}

static unsigned read_pressed(void)
{
    static int lhold;
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned pressed = pad.buttons & ~prev_btn;
    unsigned ud = pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN);
    if (ud) { hold++; if (hold > 20 && hold % 3 == 0) pressed |= ud; } else hold = 0;
    prev_btn = pad.buttons;
    lstick_ud = stick_dir(pad.ly, &lhold);
    stick_lr = stick_side(pad.lx, pad.ly);
    stick_x = (int)pad.lx - 128;                             /* 0.4.0: only the left stick moves through a film */
    return pressed;
}

static void move_sel(unsigned p, int count)
{
    if (!count) return;
    if (p & SCE_CTRL_DOWN)  sel = (sel + 1) % count;
    if (p & SCE_CTRL_UP)    sel = (sel + count - 1) % count;
    if (p & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER)) { sel += SCR_SRC_ROWS; if (sel >= count) sel = count - 1; }   /* a page */
    if (p & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER))  { sel -= SCR_SRC_ROWS; if (sel < 0) sel = 0; }
}

static void stop_all(void)
{
    player_stop();
    next_ep_at = 0;                                          /* 0.7.0 */
    probe_cancel();
    tsp_stop();
    play_path[0] = 0;
    if (net_playing) { conn_end_ms = now_ms(); net_playing = 0; }   /* 1.5 */
    dl_stream_ended();                                       /* 1.4.1: the downloads it stopped go on (in 3 s) */
}
