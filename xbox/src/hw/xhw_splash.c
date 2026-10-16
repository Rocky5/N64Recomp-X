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
 *   xhw_splash_finish()        waits out the minimum hold (and a BACK hold
 *                              in progress); returns the latched XHW_HOLD_*
 *   xhw_splash_release()       the mode is about to change: the thread stops
 *   xhw_splash_card(...)       an error card in the same dress (xhw_error_card)
 *
 * The holds: BACK held for HOLD_MS with neither trigger past 100 (L + R +
 * BACK is the exit combos' start) latches safe video for this boot; BACK +
 * START held for HOLD_MS latches safe video and default settings. A latch
 * only ever goes up. While a button is down the line says what it will do;
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
static volatile int s_back_pending;   /* BACK (or BACK + START) is down and not yet latched */
static volatile int s_dumped;
#ifdef XHW_SPLASH_DUMP
static volatile int s_dump_now;   /* the last loading-screen frame */
#endif
static int s_released;        /* the mode changed: the framebuffer is pbkit's now */
static volatile int s_pads_up;     /* the controllers answer */
static volatile DWORD s_pads_t0;   /* ... since then */
#define BACK_WINDOW_MS 1000        /* a BACK held from power-on latches within this */

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
    static DWORD back_t0, reset_t0;
    int i, back = 0, start = 0, trig = 0, other = 0;
    if (!s_pads_up || !xhw_pad_trylock()) return;   /* the boot thread is bringing them up */
    xhw_pad_poll();
    for (i = 0; i < 4; i++) {
        xhw_pad p;
        if (!xhw_pad_get(i, &p)) continue;
        if (xhw_combo_check(i, &p)) {
            trig = 1;
            continue;
        }
        if (p.buttons & XHW_BTN_BACK) back = 1;
        if (p.buttons & XHW_BTN_START) start = 1;
        if (p.lt > 100 || p.rt > 100) trig = 1;
        if ((p.buttons & ~(XHW_BTN_BACK | XHW_BTN_START)) || p.lt > 128 || p.rt > 128) other = 1;
    }
    xhw_pad_unlock();
    if (xhw_combo_pending()) {
        xhw_combo_act();   /* does not return */
        return;
    }
    if (back && !trig) {
        if (!back_t0) back_t0 = now;
        if (start) {
            if (!reset_t0) reset_t0 = now;
        } else {
            reset_t0 = 0;
        }
        s_holding = start ? XHW_HOLD_SAFE_RESET : XHW_HOLD_SAFE;
        if (start && now - reset_t0 >= HOLD_MS && s_hold < XHW_HOLD_SAFE_RESET) {
            s_hold = XHW_HOLD_SAFE_RESET;
            xhw_log("[BOOT] loading screen: BACK + START held (safe video and default settings)");
        } else if (now - back_t0 >= HOLD_MS && s_hold < XHW_HOLD_SAFE) {
            s_hold = XHW_HOLD_SAFE;
            xhw_log("[BOOT] loading screen: BACK held (safe video)");
        }
        s_back_pending = s_holding > s_hold;
    } else {
        back_t0 = reset_t0 = 0;
        s_holding = XHW_HOLD_NONE;
        s_back_pending = 0;
        if (other && !trig) s_skip = 1;
    }
}

static void splash_thread(void* arg) {
    (void)arg;
    while (s_run) {
        DWORD now = GetTickCount(), t;
        xhw_splash_state st;
        if (!s_st.card) look_at_pads(now);
        lock();
        st = s_st;
        unlock();
        st.t_ms = now - s_t0;
        st.still = XHW_SPLASH_STILL;
        st.hold = s_hold;
        st.holding = s_holding;
        xhw_splash_render(s_back, XHW_SPLASH_W, &st);
        if (XHW_TEST_BUILD) {   /* what a frame costs: [BOOT] once, after 30 frames */
            static unsigned s_frames, s_ms;
            s_ms += GetTickCount() - now;
            if (++s_frames == 30) xhw_logf("[BOOT] loading screen: %u ms a frame (30 frames)", s_ms / 30);
        }
        if (!s_run) break;   /* the mode may be changing: never touch the framebuffer then */
        copy_out();
#ifdef XHW_SPLASH_DUMP
        if (!s_dumped && (st.card || s_dump_now)) {   /* the screen as it ends, and a card */
            s_dumped = 1;
            xhw_logf("[BOOT] splash dump (%s)", st.card ? "card" : "loading screen");
            xhw_fbdump(s_back, XHW_SPLASH_W, XHW_SPLASH_H, 32, XHW_SPLASH_W * 4);
        }
#endif
        t = GetTickCount() - now;
        Sleep(t < FRAME_MS ? FRAME_MS - t : 1);
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
    snprintf(s_st.card_title, sizeof s_st.card_title, "%s", "Looking for a ROM...");
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

void xhw_splash_stage(const char* label) {
    DWORD now = GetTickCount();
    if (s_stage_name[0]) xhw_logf("[BOOT] stage %s %u", s_stage_name, (unsigned)(now - s_stage_t0));
    snprintf(s_stage_name, sizeof s_stage_name, "%s", label ? label : "");
    s_stage_t0 = now;
    if (!s_ready) return;
    lock();
    snprintf(s_st.stage, sizeof s_st.stage, "%s", label ? label : "");
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
    snprintf(s_st.card_title, sizeof s_st.card_title, "%s", title ? title : "");
    snprintf(s_st.card_sub, sizeof s_st.card_sub, "%s", sub ? sub : "");
    s_st.icon = icon64;
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
                         GetTickCount() - s_pads_t0 < BACK_WINDOW_MS || s_back_pending))
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
        xhw_logf("[BOOT] loading screen: %u ms%s, hold %d", (unsigned)(GetTickCount() - s_t0),
                 s_skip ? " (skipped)" : "", s_hold);
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
                if (xhw_pad_get(i, &p) && !xhw_combo_check(i, &p) && (p.buttons & XHW_BTN_BACK))
                    hold = (p.buttons & XHW_BTN_START) ? XHW_HOLD_SAFE_RESET : hold > XHW_HOLD_SAFE ? hold : XHW_HOLD_SAFE;
            }
            xhw_pad_unlock();
            xhw_combo_act();
            Sleep(30);
        }
        return hold;
    }
}

int xhw_splash_hold(void) { return s_hold; }

void xhw_splash_pads_up(void) {
    s_pads_t0 = GetTickCount();
    s_pads_up = 1;
}

/* XVideoSetMode frees this framebuffer: stop drawing, forget it */
void xhw_splash_release(void) {
    stop_thread();
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
