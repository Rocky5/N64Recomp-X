/* xhw_splash.c - the loading screen: its thread, its timing, the safe-video
 * holds and the error cards in its dress (PLAN.md 5.5). The drawing is
 * xhw_splash_draw.c's.
 *
 * Kept from the siblings' splash (Melee-X's xhw_splash.c, from OpenCrossing-
 * Xbox's xbox_splash.c): drawn by the CPU into the framebuffer
 * XVideoSetMode(640, 480, 32) hands back, before pbkit exists, in the
 * dashboard's own mode, so it shows on every TV; no XVideoWaitForVBlank (it
 * hooks the GPU interrupt and pb_init then fails with -4, OpenCrossing's
 * finding): the thread paces itself with Sleep; the minimum hold
 * XHW_SPLASH_MS (2000 ms), any button skips the hold (never the work); the
 * controllers are opened before it (Melee-X's order), so the BACK hold is
 * seen.
 *
 *   xhw_splash_show()          the mode, the thread (SPLASH_LEVEL, ~30 fps)
 *   xhw_splash_stage(label)    the stage line ("Reading ROM (8 MB)"); the
 *                              last stage's time goes to the log
 *   xhw_splash_progress(f)     the bar, 0..1
 *   xhw_splash_card_set(...)   the game card: title, a line, the icon
 *   xhw_splash_video(l, note)  the video line and the probation note
 *   xhw_splash_rom(id, where, ok)  the ROM's chips: code, region, revision;
 *                              its size and place; SHA-1 verified
 *   xhw_splash_panel(img, p)   the BACK menu's card over the screen (the
 *                              menu before the game), or NULL
 *   xhw_splash_finish()        waits out the minimum hold (and a BACK hold
 *                              in progress); returns the latched XHW_HOLD_*
 *   xhw_splash_release()       the mode is about to change: the thread stops
 *   xhw_splash_card(...)       an error card in the same dress (xhw_error_card)
 *
 * The holds (maintainer, 2026-10-18: the triggers are safe video, BACK the
 * menu): L + R (both triggers past 100) held for HOLD_MS without BACK (L + R
 * + BACK is the exit combos' start) latches safe video for this boot; L + R
 * + START held for HOLD_MS latches safe video and default settings; BACK
 * held for HOLD_MS with no trigger opens the BACK menu before the game
 * starts (xhw_splash_settings). A latch only ever goes up. While a button is down the line says what it will do;
 * once latched it stays lit. The thread also feeds the exit combos
 * (PLAN.md 3.9): L + R + BACK + BLACK works on the loading screen.
 *
 * Kill switches: -DXHW_NO_SPLASH (straight to the mode switch),
 * -DXHW_SPLASH_MS=<n>, -DXHW_SPLASH_STILL=1 (background frozen, for xemu
 * shot comparisons), -DXHW_SPLASH_DUMP ([FBDUMP] of the settled screen and
 * of a card). */
#include <hal/video.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xhw.h"
#include "xhw_internal.h"
#include "xhw_splash.h"

#ifndef XHW_SPLASH_MS
#define XHW_SPLASH_MS 2000
#endif
#ifndef XHW_SPLASH_STILL
#define XHW_SPLASH_STILL 0
#endif
#define SPLASH_TEXT "TechProGabe Presents..."
#define FRAME_MS 33
#define MIN_REST_MS 12   /* the least the splash thread sleeps a frame */
#define HOLD_MS 500
/* DIVERGES from PLAN.md 5.5's level 3: below the boot thread (normal, 8) the
 * loading screen starved whenever the boot worked (settings.ini's write,
 * the ROM read, SHA-1): no frames and, in xemu, BACK or the exit combos
 * pressed meanwhile were missed. One level above it, a frame is drawn every
 * FRAME_MS and the boot gets the rest (docs/decisions.md). */
#define SPLASH_LEVEL 9

static uint32_t* s_fb;            /* the visible framebuffer (write-combined) */
static uint32_t* s_back;          /* the frame is drawn here (cached), then copied out */
static int s_ready;
static volatile int s_run, s_stopped;
static xhw_event* s_done;
static volatile LONG s_lock;      /* the state below: boot thread writes, splash thread reads */
static xhw_splash_state s_st;
static DWORD s_t0, s_stage_t0;
static char s_stage_name[64];
static volatile int s_skip;       /* a button (not BACK/START) skipped the hold */
static volatile int s_hold, s_holding;
static volatile int s_hold_pending;   /* L + R (+ START) or BACK is down and not yet latched */
static volatile int s_settings;       /* BACK: 1 held, 2 latched (the menu before the game) */
static volatile int s_dumped;
#ifdef XHW_SPLASH_DUMP
static volatile int s_dump_now;   /* the last loading-screen frame */
#endif
static uint32_t* s_frozen;    /* the screen under the menu before the game */
static int s_released;        /* the mode changed: the framebuffer is pbkit's now */
static volatile int s_pads_up;     /* the controllers answer */
static volatile DWORD s_pads_t0;   /* ... since then */
#define BACK_WINDOW_MS 1000        /* a hold from power-on latches within this */
#define TRIG_ON 100                /* a trigger past this is held (as the exit combos read it) */
#ifndef XHW_SPLASH_MENU
#define XHW_SPLASH_MENU 1          /* -DXHW_SPLASH_MENU=0: BACK at boot does nothing */
#endif

static void lock(void) {
    while (InterlockedCompareExchange(&s_lock, 1, 0)) Sleep(0);
}
static void unlock(void) { InterlockedExchange(&s_lock, 0); }

static void copy_out(void) {
    int y;
    for (y = 0; y < XHW_SPLASH_H; y++)
        memcpy(s_fb + y * XHW_SPLASH_W, s_back + y * XHW_SPLASH_W, XHW_SPLASH_W * 4);
    __asm__ volatile("sfence" ::: "memory");
}

/* the controllers, once a frame: the holds, the skip, the exit combos */
static void look_at_pads(DWORD now) {
    static DWORD lr_t0, reset_t0, back_t0;
    int i, back = 0, start = 0, lr = 0, one_trig = 0, other = 0, combo = 0;
    if (!s_pads_up || !xhw_pad_trylock()) return;   /* the boot thread is bringing them up */
    xhw_pad_poll();
    for (i = 0; i < 4; i++) {
        xhw_pad p;
        if (!xhw_pad_get(i, &p)) continue;
        if (xhw_combo_check(i, &p)) {
            combo = 1;
            continue;
        }
        if (p.buttons & XHW_BTN_BACK) back = 1;
        if (p.buttons & XHW_BTN_START) start = 1;
        if (p.lt > TRIG_ON && p.rt > TRIG_ON) lr = 1;
        else if (p.lt > TRIG_ON || p.rt > TRIG_ON) one_trig = 1;
        if (p.buttons & ~(XHW_BTN_BACK | XHW_BTN_START)) other = 1;
    }
    xhw_pad_unlock();
    if (xhw_combo_pending()) {
        xhw_combo_act();   /* does not return */
        return;
    }
    /* L + R (both triggers) without BACK: safe video; with START, and default
     * settings. BACK alone, no trigger: the BACK menu before the game. L + R
     * + BACK is the exit combos' start: neither. */
    if (lr && !back && !combo) {
        if (!lr_t0) lr_t0 = now;
        if (start) {
            if (!reset_t0) reset_t0 = now;
        } else {
            reset_t0 = 0;
        }
        s_holding = start ? XHW_HOLD_SAFE_RESET : XHW_HOLD_SAFE;
        if (start && now - reset_t0 >= HOLD_MS && s_hold < XHW_HOLD_SAFE_RESET) {
            s_hold = XHW_HOLD_SAFE_RESET;
            xhw_log("[BOOT] loading screen: L + R + START held (safe video and default settings)");
        } else if (now - lr_t0 >= HOLD_MS && s_hold < XHW_HOLD_SAFE) {
            s_hold = XHW_HOLD_SAFE;
            xhw_log("[BOOT] loading screen: L + R held (safe video)");
        }
    } else {
        lr_t0 = reset_t0 = 0;
        s_holding = XHW_HOLD_NONE;
    }
    if (back && !lr && !one_trig && !combo && XHW_SPLASH_MENU) {
        if (!back_t0) back_t0 = now;
        if (s_settings < 2) s_settings = 1;
        if (now - back_t0 >= HOLD_MS && s_settings < 2) {
            s_settings = 2;
            xhw_log("[BOOT] loading screen: BACK held (the settings before the game)");
        }
    } else {
        back_t0 = 0;
        if (s_settings == 1) s_settings = 0;
    }
    s_hold_pending = s_holding > s_hold || s_settings == 1;
    if (!s_hold_pending && other && !back && !lr && !one_trig) s_skip = 1;
}

static void splash_thread(void* arg) {
    (void)arg;
    while (s_run) {
        DWORD now = GetTickCount(), t;
        xhw_splash_state st;
        if (!s_st.card && !s_st.panel) look_at_pads(now);   /* the menu before the game reads them itself */
        lock();
        st = s_st;
        unlock();
        st.t_ms = now - s_t0;
        st.still = XHW_SPLASH_STILL;
        st.hold = s_hold;
        st.holding = s_holding;
        st.settings = s_settings;
        {   /* the bar eases towards where the stage is: it never jumps */
            static float s_shown;
            float f = st.progress < 0 ? 0 : st.progress > 1 ? 1 : st.progress;
            float target = st.step ? ((float)(st.step - 1) + f) / (float)XHW_SPLASH_STEPS : f;
            if (target > s_shown) s_shown += (target - s_shown) * 0.3f + 0.002f;
            if (s_shown > target) s_shown = target;
            st.shown = s_shown > 0.001f ? s_shown : 0.001f;
        }
        if (st.panel) {   /* the menu before the game: the screen under it frozen, as a game is under the menu */
            static DWORD s_panel_t0;
            int fade;
            if (!s_frozen && (s_frozen = (uint32_t*)malloc(XHW_SPLASH_W * XHW_SPLASH_H * 4)) != NULL) {
                const uint32_t* p = st.panel;
                st.panel = NULL;
                xhw_splash_render(s_frozen, XHW_SPLASH_W, &st);
                st.panel = p;
                s_panel_t0 = now;
            }
            fade = now - s_panel_t0 >= 200 ? 256 : (int)((now - s_panel_t0) * 256 / 200);
            fade = 256 - ((256 - fade) * (256 - fade) >> 8);   /* ease out */
            if (s_frozen) memcpy(s_back, s_frozen, XHW_SPLASH_W * XHW_SPLASH_H * 4);
            else xhw_splash_render(s_back, XHW_SPLASH_W, &st);
            xhw_splash_panel_over(s_back, XHW_SPLASH_W, &st, fade);
        } else {
            xhw_splash_render(s_back, XHW_SPLASH_W, &st);
        }
        {   /* what a frame costs: [BOOT] once, after 30 frames (the first draws the sky's cache) */
            static unsigned s_frames, s_ms, s_first;
            unsigned ms = GetTickCount() - now;
            if (!s_frames) s_first = ms;
            else s_ms += ms;
            if (++s_frames == 31) xhw_logf("[BOOT] loading screen: %u ms a frame (30 frames; the first %u ms)", s_ms / 30, s_first);
        }
        if (!s_run) break;   /* the mode may be changing: never touch the framebuffer then */
        copy_out();
#ifdef XHW_SPLASH_DUMP
        {   /* the menu before the game, once, a second after it opens */
            static DWORD s_panel_t0;
            static int s_panel_dumped;
            if (st.panel && !s_panel_t0) s_panel_t0 = now;
            if (st.panel && !s_panel_dumped && now - s_panel_t0 > 1000) {
                s_panel_dumped = 1;
                xhw_log("[BOOT] splash dump (the menu before the game)");
                xhw_fbdump(s_back, XHW_SPLASH_W, XHW_SPLASH_H, 32, XHW_SPLASH_W * 4);
            }
        }
        if (!s_dumped && (st.card || s_dump_now)) {   /* the screen as it ends, and a card */
            s_dumped = 1;
            xhw_logf("[BOOT] splash dump (%s)", st.card ? "card" : "loading screen");
            xhw_fbdump(s_back, XHW_SPLASH_W, XHW_SPLASH_H, 32, XHW_SPLASH_W * 4);
        }
#endif
        /* the boot thread is below this one: it gets at least half of the CPU
         * however slow a frame is (red's first deep-space build gave it none) */
        t = GetTickCount() - now;
        Sleep(t + MIN_REST_MS < FRAME_MS ? FRAME_MS - t : (t > MIN_REST_MS ? t : MIN_REST_MS));
    }
    s_stopped = 1;
    xhw_event_signal(s_done);
}

static int init_fb(void) {
    if (s_ready) return 1;
    if (!XVideoSetMode(XHW_SPLASH_W, XHW_SPLASH_H, 32, REFRESH_DEFAULT)) return 0;
    s_fb = (uint32_t*)XVideoGetFB();
    if (!s_fb) return 0;
    if (!s_back) s_back = (uint32_t*)malloc(XHW_SPLASH_W * XHW_SPLASH_H * 4);
    if (!s_back) return 0;
    s_ready = 1;
    return 1;
}

void xhw_splash_show(void) {
#ifndef XHW_NO_SPLASH
    if (!init_fb()) return;
    lock();
    memset(&s_st, 0, sizeof s_st);
    s_st.wordmark = SPLASH_TEXT;
    snprintf(s_st.version, sizeof s_st.version, "N64Recomp-X %s", XRC_VERSION);
    unlock();
    s_t0 = s_stage_t0 = GetTickCount();
    s_done = xhw_event_create();
    s_run = 1;
    if (!xhw_thread_spawn(splash_thread, NULL, SPLASH_LEVEL, 64 * 1024)) {
        s_run = 0;
        return;
    }
    xhw_logf("[BOOT] splash: %s", SPLASH_TEXT);
#endif
}

static void stop_thread(void) {
    if (!s_run) return;
    s_run = 0;
    if (s_done) xhw_event_wait(s_done, 2000);
}

/* the stage's place among the four the screen counts: the ROM, memory, the
 * code, the start (a label it does not know keeps the step it had) */
static int stage_step(const char* l, int was) {
    if (!strncmp(l, "Reading", 7) || !strncmp(l, "Checking", 8) || !strncmp(l, "Verifying", 9)) return 1;
    if (!strncmp(l, "Preparing", 9)) return 2;
    if (!strncmp(l, "Loading", 7)) return 3;
    if (!strncmp(l, "Starting", 8)) return 4;
    return was;
}

void xhw_splash_stage(const char* label) {
    DWORD now = GetTickCount();
    if (s_stage_name[0]) xhw_logf("[BOOT] stage %s %u", s_stage_name, (unsigned)(now - s_stage_t0));
    snprintf(s_stage_name, sizeof s_stage_name, "%s", label ? label : "");
    s_stage_t0 = now;
    if (!s_ready) return;
    lock();
    snprintf(s_st.stage, sizeof s_st.stage, "%s", label ? label : "");
    s_st.t_stage = now - s_t0;
    s_st.stage_ms[0] = '\0';
    unlock();
}

/* test builds: the stage's time at the right of its label */
static void stage_time(void) {
    if (!XHW_TEST_BUILD || !s_ready) return;
    lock();
    snprintf(s_st.stage_ms, sizeof s_st.stage_ms, "%u ms", (unsigned)(GetTickCount() - s_stage_t0));
    unlock();
}

void xhw_splash_progress(float f) {
    if (!s_ready) return;
    lock();
    s_st.progress = f < 0 ? 0 : f > 1 ? 1 : f;
    unlock();
    stage_time();
}

void xhw_splash_card_set(const char* title, const char* sub, const uint32_t* icon64) {
    if (!s_ready) return;
    lock();
    if (strcmp(s_st.card_title, title ? title : "")) s_st.t_title = GetTickCount() - s_t0;
    snprintf(s_st.card_title, sizeof s_st.card_title, "%s", title ? title : "");
    snprintf(s_st.card_sub, sizeof s_st.card_sub, "%s", sub ? sub : "");
    s_st.icon = icon64;
    unlock();
}

void xhw_splash_rom(const char* id, const char* where, int verified) {
    if (!s_ready) return;
    lock();
    if (id && !s_st.rom_id[0]) s_st.t_rom = GetTickCount() - s_t0;
    if (verified && !s_st.rom_ok) s_st.t_ok = GetTickCount() - s_t0;
    if (id) snprintf(s_st.rom_id, sizeof s_st.rom_id, "%s", id);
    if (where) snprintf(s_st.rom_where, sizeof s_st.rom_where, "%s", where);
    s_st.rom_ok = verified;
    unlock();
}

void xhw_splash_panel(const uint32_t* img, int pitch_px) {
    if (!s_ready) return;
    lock();
    s_st.panel = img;
    s_st.panel_pitch = pitch_px;
    unlock();
}

void xhw_splash_video(const char* line, const char* note) {
    if (!s_ready) return;
    lock();
    snprintf(s_st.video, sizeof s_st.video, "%s", line ? line : "");
    snprintf(s_st.video_note, sizeof s_st.video_note, "%s", note ? note : "");
    unlock();
}

int xhw_splash_finish(void) {
#ifndef XHW_NO_SPLASH
    if (s_run) {
        /* the minimum hold, and the BACK window after the controllers came up
         * (a skip ends the first, never the second) */
        while (s_run && ((!s_skip && GetTickCount() - s_t0 < XHW_SPLASH_MS) || !s_pads_up ||
                         GetTickCount() - s_pads_t0 < BACK_WINDOW_MS || s_hold_pending))
            Sleep(16);
#ifdef XHW_SPLASH_DUMP
        {
            DWORD t0 = GetTickCount();
            s_dump_now = 1;
            while (!s_dumped && GetTickCount() - t0 < 30000) Sleep(16);   /* a dump over COM1 takes seconds */
        }
#endif
        if (s_stage_name[0]) {
            xhw_logf("[BOOT] stage %s %u", s_stage_name, (unsigned)(GetTickCount() - s_stage_t0));
            s_stage_name[0] = '\0';
        }
        xhw_logf("[BOOT] loading screen: %u ms%s, hold %d%s", (unsigned)(GetTickCount() - s_t0),
                 s_skip ? " (skipped)" : "", s_hold, s_settings == 2 ? ", settings first" : "");
        return s_hold;
    }
#endif
    {   /* no splash: the controllers still get their 1.5 s (OpenCrossing's rule) */
        DWORD t0 = GetTickCount();
        int hold = XHW_HOLD_NONE;
        while (GetTickCount() - t0 < 1500) {
            int i;
            xhw_pad_lock();
            xhw_pad_poll();
            for (i = 0; i < 4; i++) {
                xhw_pad p;
                if (xhw_pad_get(i, &p) && !xhw_combo_check(i, &p) && p.lt > TRIG_ON && p.rt > TRIG_ON &&
                    !(p.buttons & XHW_BTN_BACK))
                    hold = (p.buttons & XHW_BTN_START) ? XHW_HOLD_SAFE_RESET : hold > XHW_HOLD_SAFE ? hold : XHW_HOLD_SAFE;
                else if (xhw_pad_get(i, &p) && (p.buttons & XHW_BTN_BACK) && p.lt <= TRIG_ON && p.rt <= TRIG_ON &&
                         XHW_SPLASH_MENU)
                    s_settings = 2;
            }
            xhw_pad_unlock();
            xhw_combo_act();
            Sleep(30);
        }
        return hold;
    }
}

int xhw_splash_hold(void) { return s_hold; }

int xhw_splash_settings(void) { return s_settings == 2; }

void xhw_splash_pads_up(void) {
    s_pads_t0 = GetTickCount();
    s_pads_up = 1;
}

/* XVideoSetMode frees this framebuffer: stop drawing, forget it */
void xhw_splash_release(void) {
    stop_thread();
    xhw_splash_free_cache();
    free(s_frozen);
    s_frozen = NULL;
    s_released = 1;
    s_ready = 0;
    s_fb = NULL;
}

int xhw_splash_card(const char* title, const char* const* lines) {
    int i;
    if (s_released) return 0;
    if (!s_ready) {
        if (!init_fb()) return 0;
        s_t0 = GetTickCount();
    }
    lock();
    s_st.wordmark = SPLASH_TEXT;
    s_st.card = 1;
    snprintf(s_st.card_title, sizeof s_st.card_title, "%s", title);
    s_st.card_sub[0] = '\0';
    s_st.icon = NULL;
    for (i = 0; i < XHW_CARD_LINES; i++) {
        const char* l = lines ? lines[i] : NULL;
        snprintf(s_st.card_lines[i], sizeof s_st.card_lines[i], "%s", l ? l : "");
        if (!l) lines = NULL;   /* NULL ends the list */
    }
    s_st.card_footer = "Press any button or wait to return to the dashboard.";
    s_dumped = 0;
    unlock();
    if (!s_run) {
        if (!s_done) s_done = xhw_event_create();
        s_run = 1;
        if (!xhw_thread_spawn(splash_thread, NULL, SPLASH_LEVEL, 64 * 1024)) s_run = 0;
    }
    return 1;
}
