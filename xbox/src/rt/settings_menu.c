/* settings_menu.c - the BACK menu: the port's settings over the frozen game,
 * the same in every pack (PLAN.md 5.2). Melee-X's menu.c semantics (rows,
 * `*` for a restart, "720p -> 480i" where the dashboard disallows, the
 * held-direction repeat (timed by the clock: the retrace count is not a
 * clock while two threads present and wait), A changes, B returns), drawn
 * by the platform as a card in the loading screen's dress (xhw_panel_render:
 * its aurora, sparks, the title's light sweep, the bar's green) over the
 * game's last frame, dimmed (f3d_hold_last + f3d_present_held). It fades
 * and slides in over 200 ms and fades out over 100 ms; 30 frames a second
 * once it is up (the game is frozen: the CPU is the menu's).
 *
 * BACK opens it and closes it, on any port: a tap, taken when BACK is let
 * go with nothing else pressed meanwhile, so BACK + WHITE (screenshots) and
 * the exit combos (L + R + BACK + BLACK / WHITE) never open it on the way.
 * While it is open the VI thread reads the controllers once a retrace
 * (xrc_menu_vi: the exit combos are fed from there too) and changes the
 * settings; the menu's own thread (level 13) holds the renderer and draws
 * the card frame after frame, so a slow frame never costs a press.
 * Meanwhile:
 *   - no retrace messages go out (ultra/vi.c), so the game waits where it
 *     waits for the retrace; a graphics task that comes anyway waits in
 *     gfx.c until the menu closes;
 *   - controller reads give neutral input (ultra/si.c), and after it closes
 *     until the buttons that closed it are let go;
 *   - no new audio is made, so the AC97 ring drains to silence.
 * DIVERGES: a pause the N64 did not have.
 *
 * Rows (M2's): Video output (*), Widescreen (Auto, On, Off), Texture filter, FPS counter,
 * Volume, Stick dead zone (per port, with the stick's live tilt), Restart,
 * Exit to dashboard, Return. settings.ini is written on Return (or B, or
 * BACK) when something changed. Kill switch -DXRC_MENU=0: BACK does
 * nothing, settings.ini still works. */
#include <stdio.h>

#include "f3d_interp.h"
#include "settings.h"
#include "ultra/ultra.h"
#include "xgfx.h"

#ifndef XRC_MENU
#define XRC_MENU 1
#endif

void xrc_gfx_lock(void);
void xrc_gfx_unlock(void);
int xrc_gfx_ready(void);

enum { ROW_VIDEO, ROW_WIDE, ROW_FILTER, ROW_FPS, ROW_VOLUME, ROW_PORT, ROW_STICK, ROW_RESTART, ROW_EXIT, ROW_RETURN, N_ROWS };
static const char* const k_label[N_ROWS] = {
    "Video output", "Widescreen", "Texture filter", "FPS counter", "Volume", "Controller", "  Stick dead zone",
    "Restart", "Exit to dashboard", "Return",
};

#define DIRS (XHW_BTN_UP | XHW_BTN_DOWN | XHW_BTN_LEFT | XHW_BTN_RIGHT)
#define STICK_DIR 16000      /* the left stick as a d-pad: about half tilt */
#define TRIG_CHORD 100       /* a trigger past this while BACK is down: not a tap (L + R + BACK starts the combos) */
#define REPEAT_DELAY_MS 300  /* a held direction: the first repeat, then every REPEAT_EVERY_MS */
#define REPEAT_EVERY_MS 85
#define RELEASE_RETRACES 60  /* after closing, the game reads neutral until release, at most this long */
#define FADE_IN_MS 200
#define FADE_OUT_MS 100
#define SLIDE_PX 14

static volatile int s_open, s_want, s_close, s_leave, s_apply;
static int s_sel, s_port, s_inited;
static uint32_t s_prev;
static uint64_t s_repeat_at;   /* us */
static int s_release;
static int s_dash_720p, s_dash_480p, s_dash_wide;
static XrcSettings s_boot, s_at_open;
static xhw_panel s_panel;
static uint32_t s_img[512 * 512];   /* the card, a power-of-two texture's worth (1 MB) */
static uint64_t s_open_us;
static xhw_pad s_raw[4];
static int s_raw_ok[4];
/* the BACK tap, per port: down, and nothing else meanwhile */
static uint8_t s_back_down[4], s_back_clean[4];

int xrc_menu_open(void) { return s_open; }

/* every raw pad state the runtime reads (si.c, under the pad lock): the tap */
void xrc_menu_feed(int port, const xhw_pad* p) {
    uint32_t others;
    if (!XRC_MENU || port < 0 || port > 3) return;
    others = p->buttons & ~XHW_BTN_BACK;
    if (p->buttons & XHW_BTN_BACK) {
        if (!s_back_down[port]) s_back_down[port] = 1, s_back_clean[port] = 1;
        if (others || p->lt > TRIG_CHORD || p->rt > TRIG_CHORD) s_back_clean[port] = 0;
    } else {
        if (s_back_down[port] && s_back_clean[port] && !xhw_combo_pending()) s_want = 1;
        s_back_down[port] = 0;
    }
}

/* si.c: the game reads neutral (open, or the closing press still down) */
int xrc_menu_blocks(uint32_t raw_buttons) {
    if (s_open) return 1;
    if (s_release > 0) {
        if (raw_buttons) return 1;
        s_release = 0;
    }
    return 0;
}

static int step(int v, int dir, int inc, int lo, int hi, int wrap) {
    int old = v;
    v = dir > 0 ? (v / inc + 1) * inc : ((v + inc - 1) / inc - 1) * inc;
    if (v > hi) v = wrap ? lo : old > hi ? old : hi;
    if (v < lo) v = wrap ? hi : old < lo ? old : lo;
    return v;
}

static const char* const k_out[4] = { "Auto", "480i", "480p", "720p" };
/* the menu's order: Auto, 480i, 480p, 720p (XHW_OUT_*; progressive follows) */
static int out_index(const XrcSettings* st) {
    if (st->output == XHW_OUT_AUTO) return 0;
    if (st->output >= XHW_OUT_720P) return 3;
    if (st->output == XHW_OUT_480P && st->progressive) return 2;
    return 1;
}
/* what the console runs with these settings (the dashboard has the last word) */
static int out_used(const XrcSettings* st) {
    int i = out_index(st);
    if (i == 0) return s_dash_720p ? 3 : s_dash_480p ? 2 : 1;
    if (i == 3 && !s_dash_720p) return s_dash_480p && st->progressive ? 2 : 1;
    if (i == 2 && !s_dash_480p) return 1;
    return i;
}
static int restart_needed(void) { return out_index(&g_xrc_settings) != out_index(&s_boot); }

static int wide_now(const XrcSettings* st) {
    const xhw_video_mode* vm = xhw_video();
    if (st->widescreen == XRC_WS_ON || st->widescreen == XRC_WS_STRETCH) return 1;
    if (st->widescreen == XRC_WS_AUTO) return vm->height >= 720 || s_dash_wide;
    return 0;
}

static void apply_now(void) {
    const XrcSettings* g = &g_xrc_settings;
    f3d_set_widescreen(g->widescreen != XRC_WS_OFF && wide_now(g));
    xgfx_set_tex_filter(g->texture_filter ? 2 : 0);
    xgfx_set_fps_overlay(g->fps);
}

static int stick_pct(int16_t x, int16_t y) {
    float fx = x / 32767.0f, fy = y / 32767.0f, m = __builtin_sqrtf(fx * fx + fy * fy);
    return m >= 1.0f ? 100 : (int)(m * 100.0f);
}

/* a row's value ("" for the actions) and whether it waits for a restart */
static void row_value(int r, char* v, size_t cap, int* star) {
    const XrcSettings* g = &g_xrc_settings;
    *star = 0;
    v[0] = 0;
    switch (r) {
    case ROW_VIDEO: {
        int i = out_index(g), u = out_used(g);
        if (i == 0) snprintf(v, cap, "Auto, %s", k_out[u]);
        else if (u != i) snprintf(v, cap, "%s -> %s", k_out[i], k_out[u]);
        else snprintf(v, cap, "%s", k_out[i]);
        *star = i != out_index(&s_boot);
        break;
    }
    case ROW_WIDE:
        if (g->widescreen == XRC_WS_AUTO) snprintf(v, cap, "Auto, %s", wide_now(g) ? "16:9" : "4:3");
        else snprintf(v, cap, "%s", g->widescreen == XRC_WS_ON ? "On" : g->widescreen == XRC_WS_STRETCH ? "Stretch" : "Off");
        break;
    case ROW_FILTER: snprintf(v, cap, "%s", g->texture_filter ? "Sharp" : "N64 (bilinear)"); break;
    case ROW_FPS: snprintf(v, cap, "%s", g->fps ? "On" : "Off"); break;
    case ROW_VOLUME: snprintf(v, cap, "%d%%", g->volume); break;
    case ROW_PORT: snprintf(v, cap, "Port %d%s", s_port + 1, s_raw_ok[s_port] ? "" : " (none)"); break;
    case ROW_STICK: snprintf(v, cap, "%d%%", g->stick_deadzone[s_port]); break;
    }
}

static void info_text(char* out, size_t cap) {
    const XrcSettings* g = &g_xrc_settings;
    switch (s_sel) {
    case ROW_VIDEO:
        if (out_used(g) != out_index(g) && out_index(g)) snprintf(out, cap, "The dashboard does not allow %s", k_out[out_index(g)]);
        else snprintf(out, cap, "Applies after a restart");
        break;
    case ROW_WIDE:
        if (xhw_video()->height >= 720) snprintf(out, cap, "Off: the 4:3 picture in the middle");
        else if (!s_dash_wide) snprintf(out, cap, "At 480 lines 16:9 needs the dashboard's widescreen");
        else snprintf(out, cap, "More world at the sides; the HUD stays 4:3");
        break;
    case ROW_FILTER: snprintf(out, cap, "Sharp: no texture smoothing"); break;
    case ROW_FPS: snprintf(out, cap, "Frames per second, top-left corner"); break;
    case ROW_VOLUME: snprintf(out, cap, "The game's sound"); break;
    case ROW_PORT: snprintf(out, cap, "The row below is per port"); break;
    case ROW_STICK:
        if (!s_raw_ok[s_port]) snprintf(out, cap, "No controller in port %d", s_port + 1);
        else {
            int m = stick_pct(s_raw[s_port].lx, s_raw[s_port].ly);
            snprintf(out, cap, "Left stick now %d%% (%s)", m, m > g->stick_deadzone[s_port] ? "moves" : "ignored");
        }
        break;
    case ROW_RESTART: snprintf(out, cap, "Saves settings.ini and restarts the game"); break;
    case ROW_EXIT: snprintf(out, cap, "L + R + BACK + BLACK does it from anywhere"); break;
    default: snprintf(out, cap, "Saves settings.ini (B and BACK do too)"); break;
    }
}

/* the card's frame: fade 0..256, slide in pixels */
static void draw(int fade, int slide) {
    const xhw_video_mode* vm = xhw_video();
    int r;
    memset(&s_panel, 0, sizeof s_panel);
    s_panel.t_ms = (uint32_t)((xhw_time_us() - s_open_us) / 1000u);
    s_panel.sel = s_sel;
    s_panel.rows = N_ROWS;
    snprintf(s_panel.title, sizeof s_panel.title, "%s", xrc_game.title ? xrc_game.title : "N64Recomp-X");
    snprintf(s_panel.sub_left, sizeof s_panel.sub_left, "SETTINGS");
    snprintf(s_panel.sub_right, sizeof s_panel.sub_right, "running %s %s",
             vm->height >= 720 ? "720p" : vm->progressive ? "480p" : "480i",
             vm->height >= 720 || (vm->widescreen && s_dash_wide) ? "16:9" : "4:3");
    for (r = 0; r < N_ROWS; r++) {
        int star;
        snprintf(s_panel.label[r], sizeof s_panel.label[r], "%s", k_label[r]);
        row_value(r, s_panel.value[r], sizeof s_panel.value[r], &star);
        s_panel.star[r] = star;
        s_panel.kind[r] = r >= ROW_RESTART ? XHW_PANEL_ACTION : XHW_PANEL_SETTING;
    }
    info_text(s_panel.info, sizeof s_panel.info);
    if (restart_needed()) snprintf(s_panel.note, sizeof s_panel.note, "* used after a restart");
    snprintf(s_panel.footer, sizeof s_panel.footer, "A / Left / Right change     B return");
    xhw_panel_render(s_img, 512, &s_panel);
    f3d_present_held(s_img, 512, 512, XHW_PANEL_W, XHW_PANEL_H, fade, slide);
}

/* the opening frames: faded and slid in, eased */
static void draw_now(void) {
    uint32_t ms = (uint32_t)((xhw_time_us() - s_open_us) / 1000u);
    if (ms >= FADE_IN_MS) {
        draw(256, 0);
    } else {
        int k = (int)(ms * 256u / FADE_IN_MS), e = 256 - ((256 - k) * (256 - k) >> 8);   /* ease out */
        draw(e, SLIDE_PX * (256 - e) / 256);
    }
}

static int save_if_changed(void) {
    if (!memcmp(&g_xrc_settings, &s_at_open, sizeof s_at_open)) return 1;
    if (!xrc_settings_save()) return 0;
    s_at_open = g_xrc_settings;
    return 2;
}

static void open_menu(void) {
    if (!s_inited) s_inited = 1, s_boot = g_xrc_settings;
    s_sel = 0;
    s_port = 0;
    s_prev = ~0u;     /* the press that opened it counts as held */
    s_close = s_leave = s_apply = 0;
    s_at_open = g_xrc_settings;
    s_dash_720p = xhw_video_720p_allowed();
    s_dash_480p = xhw_video_480p_allowed();
    s_dash_wide = xhw_video_widescreen_set();
    f3d_hold_last(1);
    s_open_us = xhw_time_us();
    s_open = 1;
    xhw_logf("[MENU] opened");
}

static void close_menu(void) {
    int ok = save_if_changed();
    uint64_t t0 = xhw_time_us(), t;
    while ((t = xhw_time_us() - t0) < FADE_OUT_MS * 1000ull) {   /* fade out (each present waits a vblank) */
        int k = 256 - (int)(t * 256u / (FADE_OUT_MS * 1000u));
        draw(k, (256 - k) * SLIDE_PX / 512);
    }
    f3d_present_held(NULL, 0, 0, 0, 0, 0, 0);
    s_open = 0;
    s_release = RELEASE_RETRACES;
    f3d_hold_last(0);
    xhw_logf("[MENU] closed (%s)", !ok ? "settings.ini could not be saved" : ok == 2 ? "settings.ini saved" : "nothing changed");
}

static void leave(int restart) {
    xu_save_flush();
    if (!save_if_changed()) xhw_log("[MENU] settings.ini could not be saved");
    xhw_logf("[MENU] %s", restart ? "Restart" : "Exit to dashboard");
    xhw_exit_reason(restart ? "restart from the BACK menu" : "exit to the dashboard from the BACK menu");
    xhw_pad_lock();   /* for good: nobody else enters SDL on the way out */
    if (restart) xhw_reboot_self();
    xhw_quit_to_dashboard();
}

static void change(int dir, int wrap) {
    XrcSettings* g = &g_xrc_settings;
    char v[40];
    int star;
    switch (s_sel) {
    case ROW_VIDEO: {
        int i = step(out_index(g), dir, 1, 0, 3, wrap);
        g->output = i == 0 ? XHW_OUT_AUTO : i == 1 ? XHW_OUT_480I : i == 2 ? XHW_OUT_480P : XHW_OUT_720P;
        if (i == 1) g->progressive = 0;
        else if (i == 2) g->progressive = 1;
        break;
    }
    case ROW_WIDE: {
        /* Auto, On, Off (Stretch comes with M6's interpreter work) */
        static const int order[3] = { XRC_WS_AUTO, XRC_WS_ON, XRC_WS_OFF };
        int i = 0;
        while (i < 3 && order[i] != g->widescreen) i++;
        g->widescreen = order[step(i % 3, dir, 1, 0, 2, 1)];
        break;
    }
    case ROW_FILTER: g->texture_filter = !g->texture_filter; break;
    case ROW_FPS: g->fps = !g->fps; break;
    case ROW_VOLUME: g->volume = step(g->volume, dir, 10, 0, 100, wrap); break;
    case ROW_PORT: s_port = step(s_port, dir, 1, 0, 3, 1); break;
    case ROW_STICK: g->stick_deadzone[s_port] = step(g->stick_deadzone[s_port], dir, 5, 0, 60, wrap); break;
    default: return;
    }
    s_apply = 1;   /* the menu's thread applies it, holding the renderer */
    row_value(s_sel, v, sizeof v, &star);
    xhw_logf("[MENU] %s: %s", k_label[s_sel] + (s_sel == ROW_STICK ? 2 : 0), v);
}

/* the four ports, raw, under the pad lock (try: never wait on a game read);
 * the exit combos are fed from here too. 0: the lock was busy */
static int poll(uint32_t* buttons) {
    uint32_t b = 0;
    int i;
    if (!xhw_pad_trylock()) return 0;
    xhw_pad_poll();
    for (i = 0; i < 4; i++) {
        xhw_pad* p = &s_raw[i];
        s_raw_ok[i] = xhw_pad_get(i, p) && p->connected;
        if (!s_raw_ok[i]) continue;
        if (xhw_combo_check(i, p)) continue;
        xrc_menu_feed(i, p);
        b |= p->buttons & ~XHW_BTN_BACK;
        if (p->ly > STICK_DIR) b |= XHW_BTN_UP;
        else if (p->ly < -STICK_DIR) b |= XHW_BTN_DOWN;
        if (p->lx > STICK_DIR) b |= XHW_BTN_RIGHT;
        else if (p->lx < -STICK_DIR) b |= XHW_BTN_LEFT;
    }
    xhw_pad_unlock();
    xhw_combo_act();
    *buttons = b;
    return 1;
}

/* the menu's thread: opens the menu between two frames (it takes the
 * renderer, so a graphics task in flight finishes first and the next one
 * waits), draws until it is told to close, then lets go */
static void menu_thread(void* arg) {
    (void)arg;
    for (;;) {
        unsigned frames = 0;
        if (!s_want) {
            xhw_sleep_ms(8);
            continue;
        }
        s_want = 0;
        xrc_gfx_lock();
        open_menu();
        while (!s_close && !s_leave) {
            uint64_t t0 = xhw_time_us();
            if (s_apply) s_apply = 0, apply_now();
            draw_now();
            if (XHW_TEST_BUILD && ++frames <= 3)
                xhw_logf("[MENU] frame %u: %u ms", frames, (unsigned)((xhw_time_us() - t0) / 1000u));
        }
        if (s_apply) s_apply = 0, apply_now();
        if (s_leave) leave(s_leave == 2);
        close_menu();
        s_close = 0;
        xrc_gfx_unlock();
    }
}

/* the VI thread, once a retrace: the controllers while the menu is open */
void xrc_menu_vi(unsigned retrace) {
    static int s_started;
    uint32_t cur = 0, press;
    uint64_t now;
    if (!XRC_MENU || !xrc_gfx_ready()) return;
    if (!s_started) s_started = 1, xu_spawn(menu_thread, NULL, 13, 64 * 1024);
    if (!s_open || s_close || s_leave) {
        if (!s_open && s_release > 0 && !s_want) s_release--;
        return;
    }
    if (!poll(&cur)) return;
    now = xhw_time_us();
    if (XHW_TEST_BUILD) {   /* retraces seen in the menu's first two seconds */
        static uint64_t s_t0;
        static unsigned s_r0, s_said;
        if (s_said != s_open_us) {
            if (!s_t0 || s_t0 < s_open_us) s_t0 = now, s_r0 = retrace;
            else if (now - s_t0 >= 2000000u) {
                xhw_logf("[MENU] %u retraces in %u ms", retrace - s_r0, (unsigned)((now - s_t0) / 1000u));
                s_said = (unsigned)s_open_us;
            }
        }
    }
    press = cur & ~s_prev;
    if ((cur & DIRS) && (cur & DIRS) == (s_prev & DIRS)) {
        if (now >= s_repeat_at) {
            press |= cur & DIRS;
            s_repeat_at = now + REPEAT_EVERY_MS * 1000u;
        }
    } else {
        s_repeat_at = now + REPEAT_DELAY_MS * 1000u;
    }
    s_prev = cur;
    if (XHW_TEST_BUILD && press) xhw_logf("[MENU] press %04x at retrace %u", (unsigned)press, retrace);
    if (s_want || (press & XHW_BTN_B)) {   /* a BACK tap or B: close */
        s_want = 0;
        s_close = 1;
        return;
    }
    if (press & XHW_BTN_UP) s_sel = (s_sel + N_ROWS - 1) % N_ROWS;
    else if (press & XHW_BTN_DOWN) s_sel = (s_sel + 1) % N_ROWS;
    else if (press & XHW_BTN_LEFT) change(-1, 0);
    else if (press & XHW_BTN_RIGHT) change(+1, 0);
    else if (press & XHW_BTN_A) {
        if (s_sel == ROW_RETURN) s_close = 1;
        else if (s_sel == ROW_RESTART) s_leave = 2;
        else if (s_sel == ROW_EXIT) s_leave = 1;
        else change(+1, 1);
    }
}
