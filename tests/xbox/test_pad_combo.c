/* test_pad_combo.c - the exit-combo detector on the host (PLAN.md 3.9, 4.6).
 *
 * Built by tools/xbox/test_pad_combo.py with xbox/src/hw/xhw_combo.c and
 * -DXHW_COMBO_HOST=1 (the core only: no kernel, no SDL); the clock is the
 * test's. Checks: it fires once per press and the first combo wins; never on
 * a partial combo, a trigger at or under 200, or an unplugged pad; the same
 * with a remapped layout (it reads the raw state); the release wait; the VI
 * thread's and the watchdog's look timers. Exit code 0 when all passed. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xhw_combo.h"

static int s_fail, s_checks;
static char s_why[128];

#define CHECK(cond, ...) do { \
        s_checks++; \
        if (!(cond)) { s_fail++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); \
                       printf("\n"); } \
    } while (0)

/* the platform functions the core calls */
void xhw_logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}
void xhw_exit_reason_final(const char* why) { snprintf(s_why, sizeof s_why, "%s", why); }

static xhw_pad pad(uint32_t buttons, int lt, int rt) {
    xhw_pad p;
    memset(&p, 0, sizeof p);
    p.connected = 1;
    p.buttons = buttons;
    p.lt = (uint8_t)lt;
    p.rt = (uint8_t)rt;
    return p;
}

static void reset(void) {
    xhw_combo_reset();
    s_why[0] = '\0';
}

static void partial_combos(void) {
    static const struct { uint32_t b; int lt, rt; const char* what; } k[] = {
        { XHW_BTN_BACK | XHW_BTN_BLACK, 0, 0, "no triggers" },
        { XHW_BTN_BACK | XHW_BTN_BLACK, 255, 0, "L only" },
        { XHW_BTN_BACK | XHW_BTN_BLACK, 0, 255, "R only" },
        { XHW_BTN_BLACK, 255, 255, "no BACK" },
        { XHW_BTN_BACK, 255, 255, "no BLACK or WHITE" },
        { XHW_BTN_BACK | XHW_BTN_BLACK, 200, 255, "L at 200" },
        { XHW_BTN_BACK | XHW_BTN_WHITE, 255, 200, "R at 200" },
        { XHW_BTN_START | XHW_BTN_BLACK | XHW_BTN_A, 255, 255, "START, not BACK" },
    };
    size_t i;
    int t;
    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        xhw_pad p = pad(k[i].b, k[i].lt, k[i].rt);
        reset();
        for (t = 0; t < 50; t++) xhw_combo_feed(0, &p, (uint32_t)t * 16);
        CHECK(xhw_combo_request(NULL) == XHW_COMBO_NONE, "%s fired", k[i].what);
    }
    {   /* just past the threshold: fires */
        xhw_pad p = pad(XHW_BTN_BACK | XHW_BTN_BLACK, 201, 201);
        reset();
        xhw_combo_feed(0, &p, 0);
        CHECK(xhw_combo_request(NULL) == XHW_COMBO_DASHBOARD, "201/201 did not fire");
    }
    {   /* an unplugged pad's stale state never fires */
        xhw_pad p = pad(XHW_BTN_BACK | XHW_BTN_BLACK, 255, 255);
        p.connected = 0;
        reset();
        xhw_combo_feed(0, &p, 0);
        CHECK(xhw_combo_request(NULL) == XHW_COMBO_NONE, "unplugged pad fired");
    }
}

static void once_per_press(void) {
    xhw_pad black = pad(XHW_BTN_BACK | XHW_BTN_BLACK, 255, 255);
    xhw_pad white = pad(XHW_BTN_BACK | XHW_BTN_WHITE, 255, 255);
    xhw_pad none = pad(0, 0, 0);
    int port, t, neutral = 0;
    reset();
    for (t = 0; t < 100; t++) neutral += xhw_combo_feed(2, &black, (uint32_t)t * 16);
    CHECK(xhw_combo_request(&port) == XHW_COMBO_DASHBOARD && port == 2, "request %d port %d",
          xhw_combo_request(NULL), port);
    CHECK(neutral == 100, "every read with the combo held is neutral (%d of 100)", neutral);
    CHECK(!strcmp(s_why, "quit to the dashboard: L + R + BACK + BLACK on port 3"), "reason '%s'", s_why);
    /* let go, then WHITE on another port: the first combo stands */
    xhw_combo_feed(2, &none, 2000);
    xhw_combo_feed(0, &white, 2016);
    CHECK(xhw_combo_request(&port) == XHW_COMBO_DASHBOARD && port == 2, "a second combo replaced the first");
    CHECK(xhw_combo_feed(1, &none, 2032) == 1, "a pending combo gives every read neutral input");
    /* WHITE alone */
    reset();
    xhw_combo_feed(3, &white, 0);
    CHECK(xhw_combo_request(&port) == XHW_COMBO_RESTART && port == 3, "WHITE: request %d port %d",
          xhw_combo_request(NULL), port);
    CHECK(!strcmp(s_why, "restart: L + R + BACK + WHITE on port 4"), "reason '%s'", s_why);
    /* an ordinary read is not neutral */
    reset();
    {
        xhw_pad a = pad(XHW_BTN_A | XHW_BTN_BACK, 0, 0);
        CHECK(xhw_combo_feed(0, &a, 0) == 0, "A + BACK made the read neutral");
    }
}

/* a remapped layout (the Buttons page) changes what the game sees, not what
 * the detector reads: the runtime feeds the raw pad before mapping */
static uint32_t remap(uint32_t raw) {
    uint32_t n64 = 0;
    if (raw & XHW_BTN_BLACK) n64 |= 1u << 0;   /* BLACK as A, say */
    if (raw & XHW_BTN_BACK) n64 |= 1u << 1;    /* BACK as B */
    return n64;
}

static void remapped(void) {
    xhw_pad raw = pad(XHW_BTN_BACK | XHW_BTN_BLACK, 255, 255);
    xhw_pad mapped = raw;
    mapped.buttons = remap(raw.buttons);
    reset();
    xhw_combo_feed(0, &raw, 0);
    CHECK(xhw_combo_request(NULL) == XHW_COMBO_DASHBOARD, "the raw state did not fire with a remap");
    reset();
    xhw_combo_feed(0, &mapped, 0);
    CHECK(xhw_combo_request(NULL) == XHW_COMBO_NONE, "the mapped state fired (it is not what is fed)");
}

static void release_wait(void) {
    xhw_pad black = pad(XHW_BTN_BACK | XHW_BTN_BLACK, 255, 255);
    xhw_pad back_only = pad(XHW_BTN_BACK, 0, 0);
    xhw_pad none = pad(0, 0, 0);
    reset();
    CHECK(!xhw_combo_due(0), "due with nothing pending");
    xhw_combo_feed(0, &black, 1000);
    CHECK(!xhw_combo_due(1100), "due with the buttons held");
    xhw_combo_feed(0, &back_only, 1200);
    CHECK(!xhw_combo_due(1300), "due with BACK still held");
    xhw_combo_feed(0, &none, 1400);
    CHECK(xhw_combo_due(1400), "not due after the release");
    /* held for good: due after 3 s anyway */
    reset();
    xhw_combo_feed(1, &black, 5000);
    CHECK(!xhw_combo_due(7999), "due before 3 s held");
    CHECK(xhw_combo_due(8000), "not due after 3 s held");
    /* held on another port keeps it waiting */
    reset();
    xhw_combo_feed(0, &black, 0);
    xhw_combo_feed(0, &none, 10);
    xhw_combo_feed(1, &back_only, 10);
    CHECK(!xhw_combo_due(20), "due while another port still holds BACK");
}

static void look_timers(void) {
    xhw_pad none = pad(0, 0, 0);
    unsigned r;
    int looks = 0;
    reset();
    /* VI: looks only when the game has not read for 30 retraces, on the 8-grid */
    xhw_combo_note_read(100);
    for (r = 100; r < 130; r++) looks += xhw_combo_vi_due(r);
    CHECK(looks == 0, "the VI thread looked while the game reads (%d)", looks);
    CHECK(!xhw_combo_vi_due(130), "130 is off the grid");
    CHECK(xhw_combo_vi_due(136), "no VI look at 136");
    xhw_combo_note_read(136);
    CHECK(!xhw_combo_vi_due(144), "a VI look right after a read");
    looks = 0;
    for (r = 136; r < 1136; r++) looks += xhw_combo_vi_due(r);
    CHECK(looks == (int)((1136 - 168) / 8), "VI looks after reads stopped: %d", looks);
    /* the watchdog: nothing has looked yet, then 2 s after the last look */
    reset();
    CHECK(xhw_combo_watchdog_due(500), "no watchdog look before any look");
    xhw_combo_feed(0, &none, 1000);
    CHECK(!xhw_combo_watchdog_due(2999), "watchdog looked 1999 ms after a look");
    CHECK(xhw_combo_watchdog_due(3000), "no watchdog look 2000 ms after a look");
}

int main(void) {
    partial_combos();
    once_per_press();
    remapped();
    release_wait();
    look_timers();
    printf("test_pad_combo: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail != 0;
}
