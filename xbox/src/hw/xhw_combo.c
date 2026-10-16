/* xhw_combo.c - L + R + BACK + BLACK (dashboard) and L + R + BACK + WHITE
 * (restart) in every game, in every state (PLAN.md 3.9).
 *
 * Wings64-X's pad.c mechanics, moved into the platform so no game pack can
 * lose them: detected on the raw Xbox pad state before any N64 mapping (a
 * remapped layout can't hide the combo), both triggers past XHW_COMBO_TRIG
 * (200 of 255, Melee-X's) plus BACK and BLACK or WHITE, at once, no hold
 * time. The first full combo is the one carried out; from then on every read
 * gives the game neutral input.
 *
 * Three detection points share the pad lock (xhw_pad.c), so SDL is never
 * entered from two threads: the game's own reads (the runtime's pad.c calls
 * xhw_combo_check on each port it read), the VI thread when the game has not
 * read for XHW_COMBO_VI_RETRACES (a stalled game, a cutscene, the BACK menu;
 * try-lock only), and the watchdog's tick when nothing has looked for
 * XHW_COMBO_WATCHDOG_MS (the VI thread itself stuck; try-lock only;
 * -DXRC_COMBO_WATCHDOG=0 turns that one off). The loading screen and the
 * error cards poll through xhw_combo_look.
 *
 * Carrying it out (xhw_combo_act) waits until the combo's buttons are let go,
 * at most 3 s: leaving with L, R, BACK and BLACK still down hung the console
 * on the way to the dashboard, power button included (Wings64-X console runs
 * 1-6). Then 200 ms, the save flush (the runtime's, bounded to 1 s), the pad
 * lock taken for good, and xhw_quit_to_dashboard (XLaunchXBE(NULL)) or
 * xhw_reboot_self, which write lastexit.txt and release the controllers.
 *
 * The core (feed, due, the look timers) is plain C and runs on the host in
 * tests/xbox/test_pad_combo.c (-DXHW_COMBO_HOST=1); the glue below it needs
 * the kernel. Diagnosis builds only: -DXRC_COMBOS=0. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xhw.h"
#include "xhw_combo.h"

#ifndef XRC_COMBOS
#define XRC_COMBOS 1
#endif
#ifndef XRC_COMBO_WATCHDOG
#define XRC_COMBO_WATCHDOG 1
#endif
#ifndef XHW_COMBO_HOST
#define XHW_COMBO_HOST 0
#endif

#define HELD_TRIG 100          /* a trigger still counts as down past this, for the release wait */
#define RELEASE_WAIT_MS 3000   /* act anyway after this long with the buttons held */
#define PORTS 4

static volatile long s_combo;          /* XHW_COMBO_*; set once */
static int s_combo_port;
static uint32_t s_combo_at;            /* ms */
static volatile uint32_t s_held;       /* per port: a combo button is down */
static volatile unsigned s_read_at;    /* retrace of the game's latest read */
static volatile uint32_t s_look_at;    /* ms of the latest look, any detection point */
static volatile int s_looked;

static int full(const xhw_pad* p, uint32_t button) {
    return (p->buttons & XHW_BTN_BACK) && (p->buttons & button) && p->lt > XHW_COMBO_TRIG && p->rt > XHW_COMBO_TRIG;
}

static int any_combo_button(const xhw_pad* p) {
    return (p->buttons & (XHW_BTN_BACK | XHW_BTN_BLACK | XHW_BTN_WHITE)) || p->lt > HELD_TRIG || p->rt > HELD_TRIG;
}

void xhw_combo_note_look(uint32_t now_ms) {
    s_look_at = now_ms;
    s_looked = 1;
}

int xhw_combo_feed(int port, const xhw_pad* p, uint32_t now_ms) {
    int want = XHW_COMBO_NONE;
    if (!XRC_COMBOS || port < 0 || port >= PORTS) return 0;
    xhw_combo_note_look(now_ms);
    if (p->connected && any_combo_button(p)) s_held |= 1u << port;
    else s_held &= ~(1u << port);
    if (s_combo) return 1;
    if (!p->connected) return 0;
    if (full(p, XHW_BTN_BLACK)) want = XHW_COMBO_DASHBOARD;
    else if (full(p, XHW_BTN_WHITE)) want = XHW_COMBO_RESTART;
    if (want == XHW_COMBO_NONE || !__sync_bool_compare_and_swap(&s_combo, 0, want)) return s_combo != 0;
    s_combo_port = port;
    s_combo_at = now_ms;
    if (want == XHW_COMBO_DASHBOARD) {
        char why[96];
        xhw_logf("[PAD] port %d: L + R + BACK + BLACK", port + 1);
        snprintf(why, sizeof why, "quit to the dashboard: L + R + BACK + BLACK on port %d", port + 1);
        xhw_exit_reason_final(why);
    } else {
        char why[96];
        xhw_logf("[PAD] port %d: L + R + BACK + WHITE", port + 1);
        snprintf(why, sizeof why, "restart: L + R + BACK + WHITE on port %d", port + 1);
        xhw_exit_reason_final(why);
    }
    return 1;
}

int xhw_combo_request(int* port) {
    if (port) *port = s_combo ? s_combo_port : -1;
    return (int)s_combo;
}

int xhw_combo_pending(void) { return s_combo != 0; }

int xhw_combo_due(uint32_t now_ms) {
    if (!s_combo) return 0;
    return !s_held || now_ms - s_combo_at >= RELEASE_WAIT_MS;
}

int xhw_combo_vi_due(unsigned retrace) {
    return XRC_COMBOS && retrace - s_read_at >= XHW_COMBO_VI_RETRACES && (retrace & 7) == 0;
}

int xhw_combo_watchdog_due(uint32_t now_ms) {
    return XRC_COMBOS && XRC_COMBO_WATCHDOG && (!s_looked || now_ms - s_look_at >= XHW_COMBO_WATCHDOG_MS);
}

void xhw_combo_note_read(unsigned retrace) { s_read_at = retrace; }

void xhw_combo_reset(void) {
    s_combo = 0;
    s_combo_port = 0;
    s_combo_at = 0;
    s_held = 0;
    s_read_at = 0;
    s_look_at = 0;
    s_looked = 0;
}

#if !XHW_COMBO_HOST
/* ---- the console side ---- */
#include "xhw_internal.h"

static void (*s_flush)(void);

void xhw_combo_set_flush(void (*flush)(void)) { s_flush = flush; }

static uint32_t now_ms(void) { return (uint32_t)(xhw_time_us() / 1000u); }

int xhw_combo_check(int port, const xhw_pad* p) { return xhw_combo_feed(port, p, now_ms()); }

/* with the pad lock held */
static void look_locked(void) {
    int i;
    xhw_pad_poll();
    for (i = 0; i < PORTS; i++) {
        xhw_pad p;
        xhw_pad_get(i, &p);
        xhw_combo_check(i, &p);
    }
}

void xhw_combo_look(void) {
    if (!XRC_COMBOS) return;
    xhw_pad_lock();
    look_locked();
    xhw_pad_unlock();
    xhw_combo_act();
}

void xhw_combo_vi_look(unsigned retrace) {
    if (!xhw_combo_vi_due(retrace) || !xhw_pad_trylock()) return;
    if (retrace - s_read_at >= XHW_COMBO_VI_RETRACES) look_locked();   /* the game did not read meanwhile */
    xhw_pad_unlock();
    xhw_combo_act();
}

void xhw_combo_watchdog_look(void) {
    if (!xhw_combo_watchdog_due(now_ms()) || !xhw_pad_trylock()) return;
    look_locked();
    xhw_pad_unlock();
    xhw_combo_act();
}

void xhw_combo_act(void) {
    static volatile long s_acting;
    int c = (int)s_combo;
    if (!c) return;
    if (!__sync_bool_compare_and_swap(&s_acting, 0, 1)) {
        /* another thread carries it out: park here, out of its way (a busy
         * caller at a higher level would starve it: the loading screen's
         * thread runs at level 3) */
        for (;;) xhw_sleep_ms(1000);
    }
    xhw_watchdog_busy(1);
    while (!xhw_combo_due(now_ms())) {
        xhw_sleep_ms(30);
        if (xhw_pad_trylock()) {
            look_locked();
            xhw_pad_unlock();
        }
    }
    xhw_sleep_ms(200);
    if (s_flush) s_flush();
    xhw_pad_lock();   /* for good: nobody polls USB while it is torn down */
    if (c == XHW_COMBO_DASHBOARD) xhw_quit_to_dashboard();
    xhw_reboot_self();
}
#endif
